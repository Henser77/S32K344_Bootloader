/*==================================================================================================
 *  Mcal/fls.c —— Flash 驱动 MCAL 实现（基于 C40_Ip）
 *
 *  【实现要点，按重要性排序】
 *  ① 保护区检查：拒绝任何与 BLOCK_0（代码所在块）重叠的擦写请求
 *  ② 扇区语义：把"字节范围"翻译成"扇区列表"
 *  ③ 写拆包：自动满足 C40_Ip 的 8 字节对齐 + 128 字节边界约束
 *  ④ 循环计数超时：不用 OsIf（DUMMY 计数器不可靠）
 *  ⑤ RWW 验证：暴露轮询计数器，证明擦除期间 CPU 仍在取指
 *================================================================================================*/

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "fls.h"
#include "Det.h"

#include <string.h>                 /* memset / memcpy */

/* C40_Ip 的 IP 层驱动 */
#include "C40_Ip.h"

/* CT 生成的配置对象：C40_Ip_InitCfg */
#include "C40_Ip_Cfg.h"

/*==================================================================================================
 *                                   LOCAL DEFINES
 *================================================================================================*/
#if (FLS_DEV_ERROR_DETECT == STD_ON)
    #define FLS_DET_ERROR(api, err)                                     \
        ((void)Det_ReportError((uint16)FLS_MODULE_ID,                   \
                               (uint8)FLS_INSTANCE_ID,                  \
                               (uint8)(api),                            \
                               (uint8)(err)))
#else
    #define FLS_DET_ERROR(api, err)     ((void)0)
#endif

/** @brief 地址对齐掩码（用于向下/向上取整到 8 字节） */
#define FLS_ALIGN_MASK              (FLS_WRITE_ALIGN - 1UL)

/** @brief 128 字节边界的掩码 */
#define FLS_BOUNDARY_MASK           ((uint32)FLS_WRITE_MAX - 1UL)

/*==================================================================================================
 *                    CPU D-Cache 一致性 —— 交给 NXP 的 C40_Ip 自己处理
 *
 *  【问题背景（本项目最隐蔽的一个坑）】
 *  本工程有两件事同时成立：
 *      ① system.c 里使能了 D-Cache（编译参数带 -DD_CACHE_ENABLE）
 *      ② system.c 的 MPU 表把 Program Flash 配成 Write-Back/Allocate（可缓存）
 *
 *  于是 CPU 读 Flash 后数据进入 D-Cache，而 Flash 控制器的擦/写**不会**让
 *  CPU 缓存失效 → 再读同一地址就命中缓存、拿到擦写之前的旧数据。
 *
 *  实测症状（极易被误判成"擦除没生效"）：
 *      擦除 → 读回仍是写入时的数据 0x11 0x12 ...
 *      直接指针读同一地址 → 也是 0x11（同样经过 CPU 缓存，所以"两条独立读法"一致）
 *      但立刻再擦一次只用 0.26 ms（硬件认为扇区已经空了）
 *          → 说明**第一次擦除其实成功了**，问题在读
 *
 *  【正确的解法：用厂商提供的机制，不要自己写】
 *  C40_Ip 里本来就有 C40_Ip_SynchronizeCache()，它在**擦除后、编程后、读之前**
 *  都会调用 Cache_Ip_InvalidateByAddr() 失效 CPU 的 D-Cache。它被两个开关控制：
 *
 *      CT → C40_Ip → MemGeneral：  ☑ Synchronize Cache
 *                                  （C40_IP_SYNCRONIZE_CACHE = STD_ON）
 *      CT → Cache Ip 组件：        ☑ 启用
 *                                  （CACHE_IP_IS_AVAILABLE = STD_ON）
 *
 *  这两个开关都打开后，本模块**不需要**任何手写的缓存维护代码。
 *
 *  【历史】
 *  在发现上述开关之前，本文件手写过 DCIMVAC 循环做同样的事。现已删除 ——
 *  用厂商机制，代码更少、出处更清楚、也更容易向别人解释。
 *================================================================================================*/

/*==================================================================================================
 *                                   LOCAL VARIABLES
 *================================================================================================*/

/** @brief 初始化标志 */
static boolean Fls_Initialized = FALSE;

/** @brief 擦除等待期间的轮询次数（见 fls.h 的说明：用于验证 RWW） */
volatile uint32 g_FlsEraseSpinCount = 0U;

/** @brief 写入等待期间的轮询次数 */
volatile uint32 g_FlsWriteSpinCount = 0U;

/** @brief 最后一次 C40_Ip 的原始状态码（诊断用，0xEE = 还没开始） */
volatile uint8  g_FlsLastStatus = 0xEEU;

/**
 * @brief "未初始化"的自定义状态码
 * @details 这个值不会由 C40_Ip 返回，是我们自己写的，用来区分
 *          "底层硬件报错"和"调用方忘了先 Fls_Init"。
 *          曾经因为把 Fls_Init 藏在调试自检里、而自检被关掉，
 *          导致所有擦写瞬间失败且看不出原因 —— 这个码就是为了防止再犯。
 */
#define FLS_STATUS_NOT_INIT         (0xF0U)

/** @brief 最后一次操作的扇区号（诊断用） */
volatile uint32 g_FlsLastSector = 0U;

/** @brief 等待阶段的返回状态（诊断用） */
volatile uint8  g_FlsWaitStatus = 0xEEU;

/* 上一次 Fls_Erase 实际擦了多少个扇区、范围是多少（诊断用） */
volatile uint32 g_FlsLastEraseCount       = 0U;

/** @brief 上一次 Fls_Erase 里累计的"等待忙"轮询次数（跨所有扇区累加）
 *  @details 128 个扇区若都真的等过，这个数应该是百万级；
 *           如果是 0 或几十，说明等待循环根本没等（硬件被判成"立刻完成"）。 */
volatile uint32 g_FlsTotalEraseSpin       = 0U;

/** @brief 因擦除后验证不通过而重擦的次数（诊断用） */
volatile uint32 g_FlsEraseRetryCount      = 0U;

/** @brief 本次 Fls_Erase / Fls_Write 里"最慢的一次等待"花了多少毫秒（诊断用）
 *  @details 每次 Fls_Erase/Fls_Write 开头清零。正常值是：擦除单扇区 3~4 毫秒、
 *           写入 1 KB 十几毫秒。如果这个数接近超时上限，说明 Flash 快撑不住了。 */
volatile uint32 g_FlsMaxWaitMs             = 0U;

/** @brief 上一次 BlankCheck 失败时，第一个非 0xFF 字节的绝对地址 */
volatile uint32 g_FlsBlankFailAddr  = 0U;
/** @brief 该字节的实际值 */
volatile uint8  g_FlsBlankFailValue = 0U;
volatile uint32 g_FlsLastEraseStartSector = 0U;
volatile uint32 g_FlsLastEraseEndSector   = 0U;

/** @brief 写后校验失败的详细信息（诊断用） */
volatile uint32 g_FlsVerifyChunk    = 0U;
volatile uint32 g_FlsVerifyAddr     = 0U;
volatile uint32 g_FlsVerifyLen      = 0U;
volatile uint32 g_FlsVerifyOffset   = 0U;
volatile uint8  g_FlsVerifyExpected = 0U;
volatile uint8  g_FlsVerifyActual   = 0U;

/*==================================================================================================
 *                                    LOCAL HELPERS
 *================================================================================================*/


/**
 * @brief 判断一个地址范围是否完整落在 P-Flash 可用区内
 * @details 上界用 FLS_PFLASH_USABLE_END 而不是 0x007FFFFF，
 *          因为 BLOCK_3 末尾 200 KB 是 HSE 保留区，碰了会出问题。
 */
static boolean Fls_IsRangeValid(Fls_AddressType Address, Fls_LengthType Length)
{
    uint32 endAddr;

    if (Length == 0U)
    {
        return FALSE;
    }

    /* 起始地址必须在 P-Flash 内 */
    if ((Address < FLS_PFLASH_START) || (Address > FLS_PFLASH_USABLE_END))
    {
        return FALSE;
    }

    /* 防止 Address + Length 溢出 32 位 */
    if (Length > (0xFFFFFFFFUL - Address))
    {
        return FALSE;
    }

    endAddr = Address + Length - 1U;

    return (endAddr <= FLS_PFLASH_USABLE_END) ? TRUE : FALSE;
}

/**
 * @brief 判断地址范围是否与"代码所在 Block"重叠
 *
 * @details 【为什么这条检查最重要】
 *          擦掉自己 = 芯片变砖。
 *          更隐蔽的是擦**同一个 Block 的其他扇区**：代码字节还在，
 *          但 C40 在做擦写时同 Block 的取指被硬件阻塞 → HardFault。
 *          所以整个 BLOCK_0 一律划为禁区，不只是代码实际占用的那一段。
 *
 * 重叠判定（半开区间）：A.start <= B.end && B.start <= A.end
 */
static boolean Fls_OverlapsCodeBlock(Fls_AddressType Address, Fls_LengthType Length)
{
    uint32 endAddr = Address + Length - 1U;

    return ((Address <= FLS_CODE_BLOCK_END) &&
            (FLS_CODE_BLOCK_START <= endAddr)) ? TRUE : FALSE;
}

/**
 * @brief 解锁一个扇区（清 SETSLOCK 位）
 *
 * @details 【为什么每一步都要解锁】
 *  S32K344 的 P-Flash 扇区**复位后是锁住的**（SPELOCK.SLCK = 1）。
 *  在这种状态下 C40_Ip_MainInterfaceSectorErase 会直接返回
 *  C40_IP_STATUS_ERROR (0x02) —— 一发起就失败，硬件根本没开始干活。
 *
 *  而 C40_Ip 的 PreCheck 会检查这一点：
 *      C40_Ip_GetLock(sector) != C40_IP_STATUS_SECTOR_UNPROTECTED → 拒绝
 *
 *  解锁走 C40_Ip_ClearLock()，它内部：
 *      ① GetLock() 查当前状态，已是 UNPROTECTED 就直接成功
 *      ② 否则 CheckLockDomainID()：读 LOCKMASTER 判断锁属于哪个域
 *           - 域 ID = 0xFF（无主）或 = 我们传入的 DomainId → 可以解锁
 *           - 其它域 → 返回 ERROR（说明这条 Flash 被别的域占着）
 *      ③ 通过后清 SETSLOCK 的对应位
 *
 *  ⚠️ 这解释了为什么"真实 ECU 出厂时 Flash 是锁住的"：
 *     防意外擦写。Bootloader 启动后必须自己解锁才能刷写。
 *
 * @return C40_Ip_StatusType（SUCCESS = 已解锁或本来就解锁）
 */
static C40_Ip_StatusType Fls_UnlockSector(C40_Ip_VirtualSectorsType Sector)
{
    return C40_Ip_ClearLock(Sector, (uint8)FLS_DOMAIN_ID);
}

/**
 * @brief 解锁一段地址覆盖的所有扇区
 * @return E_OK 全部解锁成功
 */
static Std_ReturnType Fls_UnlockRange(Fls_AddressType Address, Fls_LengthType Length)
{
    C40_Ip_VirtualSectorsType startSector;
    C40_Ip_VirtualSectorsType endSector;
    uint32                    s;

    startSector = C40_Ip_GetSectorNumberFromAddress(Address);
    endSector   = C40_Ip_GetSectorNumberFromAddress(Address + Length - 1U);

    if ((startSector == (C40_Ip_VirtualSectorsType)C40_IP_SECTOR_ERROR) ||
        (endSector == (C40_Ip_VirtualSectorsType)C40_IP_SECTOR_ERROR))
    {
        return E_NOT_OK;
    }

    for (s = (uint32)startSector; s <= (uint32)endSector; s++)
    {
        g_FlsLastSector = s;
        g_FlsLastStatus = (uint8)Fls_UnlockSector((C40_Ip_VirtualSectorsType)s);

        if (g_FlsLastStatus != (uint8)C40_IP_STATUS_SUCCESS)
        {
            /* 解锁失败 —— 通常是这条 Flash 被别的域（比如另一个核）占着 */
            return E_NOT_OK;
        }
    }

    return E_OK;
}

/**
 * @brief 等 Flash 控制器真正开始执行 HV 操作（MCRS.DONE 清零）
 *
 * @details 【这个函数解决的是最隐蔽的一类 bug：假完成】
 *
 *  C40_Ip 的状态判断逻辑是：
 *      MCRS.DONE == 0  →  BUSY（还在干活）
 *      MCRS.DONE == 1  →  给出结论（看 PEG/PEP/PES）
 *
 *  但 MCRS.DONE 是**上一次操作留下的 1**。置 EHV 启动新操作之后，
 *  硬件要过若干周期才把 DONE 清零。如果立刻轮询，就会读到旧值
 *  → 误判"已完成" → **操作根本没做却返回成功**。
 *
 *  实测症状（非常重要，值得记住）：
 *      擦 128 个扇区只用了 9 毫秒（每扇区 70 微秒，NOR Flash 不可能），
 *      而且返回值是"成功"。其中：
 *          第 1 个扇区：轮询 12000 次（真的在等，正常）
 *          后 127 个  ：每个只轮询约 185 次（假完成）
 *
 *  【为什么单扇区正常、连续多扇区出错】
 *  第 1 次操作前 DONE 恰好是 0（上电初态），所以能正常等到完成。
 *  第 1 次结束后 DONE 留下 1，第 2 次立刻轮询就读到它 → 假完成。
 *
 *  【正确做法：两阶段等待】
 *      ① 先等 DONE 变 0 —— 证明硬件接受命令、真的开始干活
 *      ② 再等操作完成   —— 这时 DONE=1 才是真完成
 *  这是所有 Flash 驱动的标准写法。
 *
 * @return TRUE  已观察到 DONE 清零（硬件已开工）
 *         FALSE 一直没清零（超时兜底，避免死等）
 */
/*--------------------------------------------------------------------------------------------------
 *                            时间上限（2026-10-25 新增）
 *
 * 【为什么原来只有"轮询次数"上限，现在还要加"时间"上限】
 *  原来的 FLS_SPIN_LIMIT 是 2000 万次轮询。但"一次轮询花多少周期"取决于
 *  编译器优化和当时走的代码路径，所以这个上限换算成时间是不确定的
 *  （量级 4~6 秒）。作为兜底够用，但没法写进文档、也没法验证。
 *
 *  现在再加一道"真实时间"上限，两个上限谁先到谁生效：
 *      硬件开工：100 毫秒（正常微秒级）
 *      擦除    ：5 秒（实测单扇区约 3.4 毫秒，余量 1400 倍）
 *      写入    ：1 秒（实测 1 KB 十几毫秒）
 *
 * 【DWT 没使能时怎么办】
 *  CYCCNT 恒为 0 -> (now - start) 恒为 0 -> 时间检查永不触发，
 *  自动退化成"只靠轮询次数兜底"。所以这个改动对早期初始化阶段是安全的。
 *------------------------------------------------------------------------------------------------*/

/** @brief DWT 周期计数器（和 CanTp/Dcm 用的是同一个硬件计数器） */
#define FLS_REG_DWT_CYCCNT          (*(volatile uint32*)0xE0001004UL)

/** @brief 160 MHz 内核 -> 每毫秒 160000 个周期 */
#define FLS_CYCLES_PER_MS           (160000UL)

/**
 * @brief 从某个 CYCCNT 基准点到现在过了多少毫秒
 * @note 无符号减法，计数器回绕安全
 */
static uint32 Fls_ElapsedMs(uint32 StartCycles)
{
    return (uint32)(FLS_REG_DWT_CYCCNT - StartCycles) / FLS_CYCLES_PER_MS;
}

/**
 * @brief 记下"最慢的一次等待"花了多久（诊断用）
 * @note  每次 Fls_Erase / Fls_Write 开头清零 g_FlsMaxWaitMs
 */
static void Fls_TrackWaitMs(uint32 StartCycles)
{
    uint32 ms = Fls_ElapsedMs(StartCycles);

    if (ms > g_FlsMaxWaitMs)
    {
        g_FlsMaxWaitMs = ms;
    }
}

static boolean Fls_WaitHvStarted(void)
{
    uint32 spin     = 0U;
    uint32 startCyc = FLS_REG_DWT_CYCCNT;

    while (0U != (IP_FLASH->MCRS & FLASH_MCRS_DONE_MASK))
    {
        /* 两重上限：轮询次数（不依赖时基）+ 真实时间（便于解释和验证） */
        if ((spin >= FLS_SPIN_LIMIT) ||
            (Fls_ElapsedMs(startCyc) >= FLS_HV_START_TIMEOUT_MS))
        {
            return FALSE;
        }
        spin++;
    }

    return TRUE;
}

/**
 * @brief 等待擦除完成
 * @return C40_Ip 的最终状态（SUCCESS / ERROR / ERROR_BLANK_CHECK / ...）
 *
 * @details 【为什么要计轮询次数】
 *          这段循环在擦除期间一直在执行 —— 也就是说 CPU 一直在从 Flash 取指。
 *          如果擦除目标与代码在同一个 Block（RWW 无效），
 *          这段循环根本跑不起来：要么取指返回垃圾、要么 HardFault，
 *          函数不可能正常返回。
 *
 *          所以 g_FlsEraseSpinCount > 0 且函数返回
 *          → 反向证明 RWW 在当前场景下有效。
 */
static C40_Ip_StatusType Fls_WaitEraseDone(void)
{
    C40_Ip_StatusType status;
    uint32            spin     = 0U;
    uint32            startCyc;

    g_FlsEraseSpinCount = 0U;

    /* ① 先等硬件真的开工（DONE 清零），避免读到上一次残留的 DONE=1 */
    (void)Fls_WaitHvStarted();

    startCyc = FLS_REG_DWT_CYCCNT;

    /* ② 等操作完成 */
    do
    {
        status = C40_Ip_MainInterfaceSectorEraseStatus();

        if (status == C40_IP_STATUS_BUSY)
        {
            g_FlsEraseSpinCount++;

            /* 两重上限：轮询次数 + 真实时间，谁先到谁生效 */
            if ((spin >= FLS_SPIN_LIMIT) ||
                (Fls_ElapsedMs(startCyc) >= FLS_ERASE_TIMEOUT_MS))
            {
                /* 硬件一直不完成 —— 超时退出，避免永久卡死。
                 * 退出前必须 Abort：否则 Flash 控制器一直处于忙状态，
                 * 后面所有擦/写/读都会失败，看起来像"驱动彻底坏了"。 */
                Fls_TrackWaitMs(startCyc);
                (void)C40_Ip_Abort();
                return C40_IP_STATUS_ERROR_TIMEOUT;
            }
            spin++;
        }
    } while (status == C40_IP_STATUS_BUSY);

    Fls_TrackWaitMs(startCyc);

    g_FlsTotalEraseSpin += g_FlsEraseSpinCount;

    return status;
}

/**
 * @brief 等待写入完成
 */
static C40_Ip_StatusType Fls_WaitWriteDone(void)
{
    C40_Ip_StatusType status;
    uint32            spin     = 0U;
    uint32            startCyc;

    g_FlsWriteSpinCount = 0U;
    g_FlsWaitStatus = 0xEEU;

    /* ① 先等硬件真的开工（DONE 清零）—— 写入路径有同样的假完成风险：
     *    实测 [5] 写 128 字节返回成功，但读回发现数据根本不在。
     *    原因就是这里没有两阶段等待。 */
    (void)Fls_WaitHvStarted();

    startCyc = FLS_REG_DWT_CYCCNT;

    /* ② 等操作完成 */
    do
    {
        status = C40_Ip_MainInterfaceWriteStatus();

        if (status == C40_IP_STATUS_BUSY)
        {
            g_FlsWriteSpinCount++;

            if ((spin >= FLS_SPIN_LIMIT) ||
                (Fls_ElapsedMs(startCyc) >= FLS_WRITE_TIMEOUT_MS))
            {
                Fls_TrackWaitMs(startCyc);
                (void)C40_Ip_Abort();
                g_FlsWaitStatus = (uint8)C40_IP_STATUS_ERROR_TIMEOUT;
                return C40_IP_STATUS_ERROR_TIMEOUT;
            }
            spin++;
        }
    } while (status == C40_IP_STATUS_BUSY);

    Fls_TrackWaitMs(startCyc);

    g_FlsWaitStatus = (uint8)status;

    return status;
}

/*==================================================================================================
 *                                    Fls_Init
 *================================================================================================*/
Std_ReturnType Fls_Init(void)
{
    C40_Ip_StatusType status;

    if (Fls_Initialized != FALSE)
    {
        /* 重复初始化直接返回成功 —— C40_Ip_Init 里没有"已初始化"断言，
         * 但重复调也没有意义，而且会清掉内部状态。 */
        return E_OK;
    }

    /* C40_Ip_InitCfg 由 CT 生成（generate/src/C40_Ip_Cfg.c），
     * 里面是两个 NULL 回调：
     *      MemStartFlashAccessNotif    = NULL_PTR
     *      MemFinishedFlashAccessNotif = NULL_PTR
     *
     * 将来如果需要"擦写期间关中断/喂看门狗"，就在这里注册回调。 */
    status = C40_Ip_Init(&C40_Ip_InitCfg);

    if (status != C40_IP_STATUS_SUCCESS)
    {
        /* 初始化失败通常是时钟没配对（Flash 时序依赖正确的时钟频率） */
        FLS_DET_ERROR(FLS_API_INIT, FLS_E_ERASE_FAILED);
        return E_NOT_OK;
    }

    Fls_Initialized = TRUE;
    return E_OK;
}

/*==================================================================================================
 *                                    Fls_Erase
 *================================================================================================*/
Std_ReturnType Fls_Erase(Fls_AddressType TargetAddress, Fls_LengthType Length)
{
    g_FlsMaxWaitMs = 0U;    /* 重新统计"最慢单次等待" */

    C40_Ip_StatusType        status;
    C40_Ip_VirtualSectorsType startSector;
    C40_Ip_VirtualSectorsType endSector;
    C40_Ip_VirtualSectorsType sector;
    uint32                   endAddr;

    /*--------------------------------------------------------------------------
     * 1. 参数与安全检查
     *------------------------------------------------------------------------*/
    if (Fls_Initialized == FALSE)
    {
        g_FlsLastStatus = (uint8)FLS_STATUS_NOT_INIT;
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_UNINIT);
        return E_NOT_OK;
    }

    if (Length == 0U)
    {
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    if (Fls_IsRangeValid(TargetAddress, Length) == FALSE)
    {
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    /* 【最重要的一条】拒绝擦代码所在的 Block */
    if (Fls_OverlapsCodeBlock(TargetAddress, Length) != FALSE)
    {
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_PROTECTED_REGION);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 2. 把"字节范围"翻译成"扇区范围"
     *
     * 注意：
     *   起始地址不要求扇区对齐 —— 会**向下**取整到所属扇区的起点，
     *   也就是"多擦一点"，这是 Flash 擦除的固有语义（8 KB 最小单位）。
     *------------------------------------------------------------------------*/
    endAddr = TargetAddress + Length - 1U;

    startSector = C40_Ip_GetSectorNumberFromAddress(TargetAddress);
    endSector   = C40_Ip_GetSectorNumberFromAddress(endAddr);

    if ((startSector == (C40_Ip_VirtualSectorsType)C40_IP_SECTOR_ERROR) ||
        (endSector == (C40_Ip_VirtualSectorsType)C40_IP_SECTOR_ERROR))
    {
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 3. 解锁目标扇区
     *
     * 【必须在擦除之前做】
     * S32K344 的 P-Flash 扇区复位后是锁住的（SPELOCK.SLCK = 1）。
     * 不解锁的话 C40_Ip_MainInterfaceSectorErase 会一发起就返回
     * C40_IP_STATUS_ERROR (0x02)，硬件根本不动 —— 表现为 spin = 0。
     *------------------------------------------------------------------------*/
    if (Fls_UnlockRange(TargetAddress, Length) != E_OK)
    {
        FLS_DET_ERROR(FLS_API_ERASE, FLS_E_PROTECTED_REGION);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 4. 逐扇区擦除
     *
     * 用 uint32 做循环变量：C40_Ip_VirtualSectorsType 可能是 uint16，
     * 直接用它自增在边界上容易出问题。
     *------------------------------------------------------------------------*/
    {
        uint32 s;
        uint32 sFirst = (uint32)startSector;
        uint32 sLast  = (uint32)endSector;

        /* 记录本次擦除的范围和实际完成数（诊断用） */
        g_FlsLastEraseCount       = 0U;
        g_FlsTotalEraseSpin       = 0U;
        g_FlsEraseRetryCount      = 0U;
        g_FlsLastEraseStartSector = sFirst;
        g_FlsLastEraseEndSector   = sLast;

        for (s = sFirst; s <= sLast; s++)
        {
            uint32 tries;

            sector = (C40_Ip_VirtualSectorsType)s;

            /* 失败时能一眼看出是哪个扇区、返回了什么码 */
            g_FlsLastSector = s;

            /*------------------------------------------------------------------
             * 逐扇区擦除：判据用 C40_Ip 内建的 Erase Verification
             *
             * CT → C40_Ip → MemGeneral：☑ Erase Verification
             *      （C40_IP_ERASE_VERIFICATION_ENABLED = STD_ON）
             * 打开后，驱动在擦除结束时会自己做 blank check，
             * 擦不干净就返回 C40_IP_STATUS_ERROR_BLANK_CHECK。
             * 所以本模块**不需要**再手写"读回校验"。
             *
             * 【重擦策略】
             * 只有"擦了但没验通过"（ERROR_BLANK_CHECK）才值得重擦；
             * 保护错误 / 超时 / 参数错误重擦也没意义，直接失败上报。
             * 真实 Bootloader 里擦除偶发失败允许重试，这是常见做法。
             *----------------------------------------------------------------*/
            for (tries = 0U; tries < 3U; tries++)
            {
                g_FlsLastStatus = 0xEEU;

                status = C40_Ip_MainInterfaceSectorErase(sector, (uint8)FLS_DOMAIN_ID);
                g_FlsLastStatus = (uint8)status;

                if (status == C40_IP_STATUS_SUCCESS)
                {
                    /* 等待完成（内部计轮询次数，用于验证 RWW） */
                    status = Fls_WaitEraseDone();
                    g_FlsLastStatus = (uint8)status;
                }

                if (status == C40_IP_STATUS_SUCCESS)
                {
                    break;              /* 擦干净了 */
                }

                if (status != C40_IP_STATUS_ERROR_BLANK_CHECK)
                {
                    break;              /* 不是"没擦干净"，重擦没有意义 */
                }

                g_FlsEraseRetryCount++; /* 记录：内建校验报了没擦干净，重擦一次 */
            }

            if (status != C40_IP_STATUS_SUCCESS)
            {
                /* 状态映射到 Det 错误码：
                 *   ERROR_BLANK_CHECK → 擦了但校验没通过
                 *   ERROR_TIMEOUT     → 硬件一直 BUSY
                 *   其它              → 通用擦除失败（含被保护）
                 */
                if (status == C40_IP_STATUS_ERROR_TIMEOUT)
                {
                    FLS_DET_ERROR(FLS_API_ERASE, FLS_E_TIMEOUT);
                }
                else if (status == C40_IP_STATUS_ERROR_BLANK_CHECK)
                {
                    FLS_DET_ERROR(FLS_API_ERASE, FLS_E_NOT_ERASED);
                }
                else
                {
                    FLS_DET_ERROR(FLS_API_ERASE, FLS_E_ERASE_FAILED);
                }
                return E_NOT_OK;
            }

            g_FlsLastEraseCount++;
        }
    }

    /*
     * 注意：这里**不**做预取缓冲失效。
     * 实测擦除后直接读回是正常的（BlankCheck 能正确读到全 0xFF），
     * 说明擦除本身会让读路径更新。失效动作只在"刚编程完、又要立刻读回"
     * 这个特定场景才需要，所以只放在 Fls_Write 的校验之前，
     * 尽量减少对 Flash 控制器的额外操作。
     */

    return E_OK;
}

/*==================================================================================================
 *                                    Fls_Write
 *================================================================================================*/
Std_ReturnType Fls_Write(Fls_AddressType TargetAddress,
                         const uint8* SourceAddressPtr,
                         Fls_LengthType Length)
{
    g_FlsMaxWaitMs = 0U;    /* 重新统计"最慢单次等待" */

    C40_Ip_StatusType status = C40_IP_STATUS_SUCCESS;
    uint8             padBuf[FLS_WRITE_MAX];
    uint32            offset = 0U;

    /** @brief 当前是第几块（仅用于诊断记录） */
    uint32            chunkIndex = 0U;

#if (FLS_VERIFY_AFTER_WRITE == STD_ON)
    /* 写后校验用的读回缓冲（和 padBuf 分开，避免互相踩） */
    uint8             verifyBuf[FLS_WRITE_MAX];
#endif

    /*--------------------------------------------------------------------------
     * 1. 参数与安全检查
     *------------------------------------------------------------------------*/
    if (Fls_Initialized == FALSE)
    {
        g_FlsLastStatus = (uint8)FLS_STATUS_NOT_INIT;
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_UNINIT);
        return E_NOT_OK;
    }

    if (SourceAddressPtr == NULL_PTR)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    if (Length == 0U)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    /* 起始地址必须 8 字节对齐（C40_Ip 硬约束） */
    if ((TargetAddress & FLS_ALIGN_MASK) != 0U)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    if (Fls_IsRangeValid(TargetAddress, Length) == FALSE)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    if (Fls_OverlapsCodeBlock(TargetAddress, Length) != FALSE)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PROTECTED_REGION);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 2. 解锁目标扇区（理由同 Fls_Erase）
     *------------------------------------------------------------------------*/
    if (Fls_UnlockRange(TargetAddress, Length) != E_OK)
    {
        FLS_DET_ERROR(FLS_API_WRITE, FLS_E_PROTECTED_REGION);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 3. 拆包循环
     *
     * C40_Ip_MainInterfaceWrite 的三条硬约束：
     *   ① 地址 8 字节对齐
     *   ② 长度 8 字节对齐，最大 128
     *   ③ **不能跨 128 字节边界**（否则返回 ERROR_INPUT_PARAM）
     *
     * 所以每一轮算出"到下一个 128 字节边界还有多远"，
     * 和剩余长度取较小值，作为本轮长度。
     *------------------------------------------------------------------------*/
    while (offset < Length)
    {
        uint32 curAddr   = TargetAddress + offset;
        uint32 remain    = Length - offset;
        uint32 boundary  = (curAddr & ~FLS_BOUNDARY_MASK) + (uint32)FLS_WRITE_MAX;
        uint32 chunk     = boundary - curAddr;      /* 到 128 边界还能写多少 */
        uint32 xfer;

        if (chunk > (uint32)FLS_WRITE_MAX)
        {
            chunk = (uint32)FLS_WRITE_MAX;
        }
        if (chunk > remain)
        {
            chunk = remain;                          /* 最后一块可能不足 */
        }

        /* 补齐到 8 的倍数（C40_Ip 要求长度为 8 的倍数） */
        xfer = (chunk + FLS_ALIGN_MASK) & ~FLS_ALIGN_MASK;

        if (xfer != chunk)
        {
            /*
             * 尾部不足 8 字节：用 0xFF 补齐。
             *
             * 【为什么补 0xFF 是安全的】
             * Flash 擦除后本来就是 0xFF。写入 0xFF 到已擦除的单元，
             * 相当于"什么都不改"（1 与 1 相与还是 1）。
             * 所以多写的这几个字节不会破坏任何东西 ——
             * 前提是那几字节确实处于擦除态（调用方负责先擦）。
             */
            (void)memset(padBuf, (int)FLS_ERASED_VALUE, (size_t)xfer);
            (void)memcpy(padBuf, &SourceAddressPtr[offset], (size_t)chunk);

            status = C40_Ip_MainInterfaceWrite(curAddr,
                                               xfer,
                                               padBuf,
                                               (uint8)FLS_DOMAIN_ID);
        }
        else
        {
            /* 长度正好是 8 的倍数：直接从源缓冲写，省一次拷贝 */
            status = C40_Ip_MainInterfaceWrite(curAddr,
                                               chunk,
                                               &SourceAddressPtr[offset],
                                               (uint8)FLS_DOMAIN_ID);
        }

        g_FlsLastSector = Fls_GetSectorNumber(curAddr);

        if (status == C40_IP_STATUS_SUCCESS)
        {
            /* 发起成功，转到等待阶段。
             * g_FlsLastStatus 保留发起结果（0x00），
             * 等待结果由 Fls_WaitWriteDone 写进 g_FlsWaitStatus。 */
            status = Fls_WaitWriteDone();
        }

        if (status != C40_IP_STATUS_SUCCESS)
        {
            g_FlsLastStatus = (uint8)status;

            if (status == C40_IP_STATUS_ERROR_TIMEOUT)
            {
                FLS_DET_ERROR(FLS_API_WRITE, FLS_E_TIMEOUT);
            }
            else if (status == C40_IP_STATUS_ERROR_BLANK_CHECK)
            {
                /* 目标区没擦干净 —— 开启擦除校验时才能报出这个 */
                FLS_DET_ERROR(FLS_API_WRITE, FLS_E_NOT_ERASED);
            }
            else if (status == C40_IP_STATUS_ERROR_PROGRAM_VERIFY)
            {
                /* 写进去了但读回不一致 —— 开启写入校验时才能报出这个 */
                FLS_DET_ERROR(FLS_API_WRITE, FLS_E_WRITE_FAILED);
            }
            else
            {
                FLS_DET_ERROR(FLS_API_WRITE, FLS_E_WRITE_FAILED);
            }
            return E_NOT_OK;
        }

        chunkIndex++;
        offset += chunk;
    }

#if (FLS_VERIFY_AFTER_WRITE == STD_ON)
    /*--------------------------------------------------------------------------
     * 4. 写后校验（可选，默认关闭）
     *
     * ⚠️ 重要限制：读缓冲问题没有解决，所以这个分支不可靠。
     *
     * 编程刚结束时 C40 的读缓冲不会自动更新，直接读回可能拿到旧数据。
     * 驱动里的 C40_Ip_InvalidPrefetchBuff() 被包在
     *     #if (STD_ON == C40_IP_ECC_CHECK)
     * 之下，即 NXP 只在 Mem ECC Enable Check 打开时才认为它安全。
     *
     * 我曾手工复制那个序列，实测**不稳定**（同一段代码有时通过有时不通过），
     * 而且它夹在两次编程之间会让后一块编程失效。已整体移除。
     *
     * 所以默认关闭本校验；编程是否成功以驱动的 waitStatus 为准
     * （MCRS 的 PEG 位，硬件结论）。整片读回校验放到上层 OTA 流程末尾做。
     *
     * 若将来要重新启用，请先在 CT 里打开 Mem ECC Enable Check，
     * 让驱动的读路径自带预取失效，再打开本开关。
     *------------------------------------------------------------------------*/
    {
        uint32 vOffset = 0U;

        chunkIndex = 0U;

        while (vOffset < Length)
        {
            uint32 vAddr  = TargetAddress + vOffset;
            uint32 remain = Length - vOffset;
            uint32 vChunk = ((vAddr & ~FLS_BOUNDARY_MASK) + (uint32)FLS_WRITE_MAX) - vAddr;
            uint32 k;

            if (vChunk > (uint32)FLS_WRITE_MAX)
            {
                vChunk = (uint32)FLS_WRITE_MAX;
            }
            if (vChunk > remain)
            {
                vChunk = remain;
            }


            if (C40_Ip_Read(vAddr, vChunk, verifyBuf) != C40_IP_STATUS_SUCCESS)
            {
                FLS_DET_ERROR(FLS_API_WRITE, FLS_E_WRITE_FAILED);
                return E_NOT_OK;
            }

            for (k = 0U; k < vChunk; k++)
            {
                if (verifyBuf[k] != SourceAddressPtr[vOffset + k])
                {
                    /* 读回不一致 —— 记录完整现场，方便定位 */
                    g_FlsVerifyChunk    = chunkIndex;
                    g_FlsVerifyAddr     = vAddr;
                    g_FlsVerifyLen      = vChunk;
                    g_FlsVerifyOffset   = k;
                    g_FlsVerifyExpected = SourceAddressPtr[vOffset + k];
                    g_FlsVerifyActual   = verifyBuf[k];

                    g_FlsLastSector = Fls_GetSectorNumber(vAddr + k);
                    g_FlsLastStatus = 0x77U;    /* 自定义：本模块写后校验失败 */
                    FLS_DET_ERROR(FLS_API_WRITE, FLS_E_WRITE_FAILED);
                    return E_NOT_OK;
                }
            }

            chunkIndex++;
            vOffset += vChunk;
        }
    }
#endif /* FLS_VERIFY_AFTER_WRITE */

    return E_OK;
}

/*==================================================================================================
 *                                    Fls_Read
 *================================================================================================*/
Std_ReturnType Fls_Read(Fls_AddressType SourceAddress,
                        uint8* TargetAddressPtr,
                        Fls_LengthType Length)
{
    C40_Ip_StatusType status;

    if (Fls_Initialized == FALSE)
    {
        g_FlsLastStatus = (uint8)FLS_STATUS_NOT_INIT;
        FLS_DET_ERROR(FLS_API_READ, FLS_E_UNINIT);
        return E_NOT_OK;
    }
    if (TargetAddressPtr == NULL_PTR)
    {
        FLS_DET_ERROR(FLS_API_READ, FLS_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    if (Length == 0U)
    {
        FLS_DET_ERROR(FLS_API_READ, FLS_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    if (Fls_IsRangeValid(SourceAddress, Length) == FALSE)
    {
        FLS_DET_ERROR(FLS_API_READ, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    /* Flash 读取没有对齐和粒度限制，直接按字节读 */
    status = C40_Ip_Read(SourceAddress, Length, TargetAddressPtr);

    if (status != C40_IP_STATUS_SUCCESS)
    {
        FLS_DET_ERROR(FLS_API_READ, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    return E_OK;
}

/*==================================================================================================
 *                                    Fls_BlankCheck
 *================================================================================================*/
Std_ReturnType Fls_BlankCheck(Fls_AddressType TargetAddress, Fls_LengthType Length)
{
    uint8  buf[FLS_WRITE_MAX];
    uint32 offset = 0U;

    if (Fls_Initialized == FALSE)
    {
        g_FlsLastStatus = (uint8)FLS_STATUS_NOT_INIT;
        FLS_DET_ERROR(FLS_API_BLANK_CHECK, FLS_E_UNINIT);
        return E_NOT_OK;
    }

    if (Length == 0U)
    {
        FLS_DET_ERROR(FLS_API_BLANK_CHECK, FLS_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    if (Fls_IsRangeValid(TargetAddress, Length) == FALSE)
    {
        FLS_DET_ERROR(FLS_API_BLANK_CHECK, FLS_E_PARAM_ADDRESS);
        return E_NOT_OK;
    }

    /*
     * 【为什么不用 C40_Ip_Compare】
     * 它的文档说 SourceAddressPtr 传 NULL 就是 blank check，
     * 但那个 API 是否被编译取决于 CT 里的 "Mem Compare Service Api" 选项
     * （当前是关闭的）。为了不依赖一个可选 API，这里自己读+比对，
     * 逻辑更透明，也少一个链接风险。
     *
     * 代价是慢一点（全部读到 RAM 再比），但 blank check 不是热路径。
     */

    while (offset < Length)
    {
        uint32 chunk = Length - offset;
        uint32 i;
        uint8* p;

        if (chunk > (uint32)FLS_WRITE_MAX)
        {
            chunk = (uint32)FLS_WRITE_MAX;
        }

        if (Fls_Read(TargetAddress + offset, buf, chunk) != E_OK)
        {
            return E_NOT_OK;
        }

        p = buf;
        for (i = 0U; i < chunk; i++)
        {
            if (p[i] != FLS_ERASED_VALUE)
            {
                /* 找到非 0xFF 的字节。
                 *
                 * ★ 记录它的**绝对地址和值** —— 这是判断"真没擦干净"
                 *   还是"读缓冲给了旧数据"的唯一依据：
                 *     地址落在每次读的头 32 字节内  → 很可能是读缓冲陈旧（假报）
                 *     地址在别处、值还是我们写过的数据 → 真的没擦掉
                 */
                g_FlsBlankFailAddr  = TargetAddress + offset + i;
                g_FlsBlankFailValue = p[i];
                return E_NOT_OK;
            }
        }

        offset += chunk;
    }

    return E_OK;
}

/*==================================================================================================
 *                                    Fls_GetSectorNumber / Fls_GetBlockNumber
 *================================================================================================*/
uint32 Fls_GetSectorNumber(Fls_AddressType Address)
{
    return (uint32)C40_Ip_GetSectorNumberFromAddress(Address);
}

uint8 Fls_GetBlockNumber(Fls_AddressType Address)
{
    if ((Address < FLS_PFLASH_START) || (Address > FLS_PFLASH_USABLE_END))
    {
        return 0xFFU;
    }

    /* 每个 Block 1 MB */
    return (uint8)((Address - FLS_PFLASH_START) / 0x00100000UL);
}

/*==================================================================================================
 *                                    Fls_GetBuildStamp
 *================================================================================================*/
const char* Fls_GetBuildStamp(void)
{
    /* __DATE__ / __TIME__ 是编译期宏，展开成字符串常量。
     * 它们反映的是**本文件（fls.c）**的编译时刻，
     * 和 main.c 里那个 banner 时间互相独立 —— 这样就能看出
     * 两部分代码是不是同一次编译产出的。 */
    return __DATE__ " " __TIME__;
}

/*==================================================================================================
 *                                    Fls_GetDiag
 *
 *  【这个函数为什么存在】
 *  C40_Ip 的 PreCheck 有 4 个拒绝条件，但它们都返回同一个
 *  C40_IP_STATUS_ERROR (0x02)。只看返回码分不出是哪一条。
 *
 *  所以把 4 个条件对应的寄存器都读出来，一次跑完就能定位：
 *
 *      条件                          对应输出              正常值
 *      VirtualSector 越界            g_FlsLastSector       273（我们的测试扇区）
 *      MCR 里 ERS/PGM 残留           *MCR bit3 / bit4      都为 0
 *      UT0 的 UTE 被置位             *UT0 bit0             为 0
 *      扇区被锁                      *LockState            0x09 (SECTOR_UNPROTECTED)
 *
 *  【只读，不修改任何寄存器】
 *================================================================================================*/
void Fls_GetDiag(uint32* MCR, uint32* UT0, uint32* MCRS, uint8* LockState)
{
    if (MCR != NULL_PTR)
    {
        *MCR = IP_FLASH->MCR;
    }

    if (UT0 != NULL_PTR)
    {
        *UT0 = IP_FLASH->UT0;
    }

    if (MCRS != NULL_PTR)
    {
        *MCRS = IP_FLASH->MCRS;
    }

    if (LockState != NULL_PTR)
    {
        /* 返回值：0x09 = 未锁，0x0A = 已锁，0x04 = 扇区号非法 */
        *LockState = (uint8)C40_Ip_GetLock((C40_Ip_VirtualSectorsType)g_FlsLastSector);
    }
}
