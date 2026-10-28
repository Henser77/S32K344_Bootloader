/*==================================================================================================
 *  Mcal/fls.h —— Flash 驱动 MCAL（内部 Flash，基于 C40_Ip）
 *
 *  【层次位置】
 *
 *      AUTOSAR 标准接口     Fls_Erase / Fls_Write / Fls_Read / Fls_BlankCheck
 *                              ↑ 本文件实现的就是这一层（但简化成同步）
 *      NXP 的 AUTOSAR Fls   Mem_43_INFLS（SDK 里有，我们不用）
 *                              ↑
 *      NXP 的 IP 层         C40_Ip（寄存器操作 + 异步状态机）  ← 我们用这个
 *                              ↑
 *      硬件                 C40 Flash 控制器
 *
 *  【和 CAN / UART 同构的选型理由】
 *  和 Mcal/can.c 用 FlexCAN_Ip、Mcal/uart.c 用 Lpuart_Uart_Ip 完全一样：
 *  用 NXP 的 IP 层驱动，自己写 MCAL 封装，而不是用生成器产出的 AUTOSAR 驱动。
 *  理由：Mem_43_INFLS 需要完整的 MCAL 配置体系，代码 90 KB，
 *  而 Bootloader 只需要"擦、写、读"三件事。
 *
 *  【和 AUTOSAR 标准 Fls 的差异：同步 vs 异步】
 *
 *      AUTOSAR Fls（异步）：
 *          Fls_Erase()  → 受理，立刻返回（返回 E_OK 只代表"收下了"）
 *          Fls_MainFunction()  → 周期性调用，推进任务
 *          FlsJobEndNotification()  → 完成回调（从 MainFunction 上下文调）
 *
 *      本模块（同步）：
 *          Fls_Erase()  → 内部"发起 + 轮询" → **返回时已彻底擦完**
 *
 *      为什么改成同步：
 *      1. Bootloader 刷写时没有别的事要并行做
 *      2. 调用方不用维护状态机、不用记"上个任务完了没"
 *      3. 失败时直接返回错误码，不用查状态
 *
 *      ⚠️ 注意：Flash 的"异步"**不需要中断**。
 *         它是"发起后硬件自己干，CPU 偶尔问一句完了没"的模型，
 *         和 UART 那种"每个字节都要 CPU 及时补上"完全不同。
 *         （C40_Ip 的 API 里也没有任何完成回调/中断注册函数。）
 *
 *  【本模块消化掉的三个 C40_Ip 硬约束】
 *  ① 擦除按 8 KB 扇区：上层说"擦这段地址"，本模块翻译成"擦这几个扇区"
 *  ② 写入地址和长度必须 8 字节对齐：不足的尾部用 0xFF 补齐
 *  ③ 一次最多 128 字节，且**不能跨 128 字节边界**：自动拆包
 *
 *  【安全保护】
 *  Fls_Erase / Fls_Write 会拒绝任何与"代码所在 Block"（BLOCK_0）重叠的请求。
 *  理由见 fls_cfg.h 的说明 —— Bootloader 绝不能擦自己。
 *================================================================================================*/
#ifndef FLS_H
#define FLS_H

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "Std_Types.h"
#include "fls_cfg.h"

/*==================================================================================================
 *                                    DEFINES AND MACROS
 *================================================================================================*/

/**
 * @brief 模块号
 * @details 91（0x5B）—— 就是 AUTOSAR 的 Fls 模块号。
 *          已从 SDK 核实：Mem_43_INFLS_Types.h:111
 *              #define MEM_43_INFLS_MODULE_ID  91U
 *          所以我们用同一个号，Det 记录里能和其他 Fls 实现对齐。
 */
#define FLS_MODULE_ID               (91U)

/** @brief 单实例，实例号固定 0 */
#define FLS_INSTANCE_ID             (0U)

/**
 * @brief 开发错误检测开关
 * @details 优先级：本模块单独定义 > 全局 BSW_DEV_ERROR_DETECT > 默认 STD_ON
 */
#ifndef FLS_DEV_ERROR_DETECT
    #ifdef BSW_DEV_ERROR_DETECT
        #define FLS_DEV_ERROR_DETECT    (BSW_DEV_ERROR_DETECT)
    #else
        #define FLS_DEV_ERROR_DETECT    (STD_ON)
    #endif
#endif

/**
 * @brief 传给 C40_Ip 的 DomainId
 * @details C40_Ip 的擦除/写入函数都有一个 `uint8 DomainIdValue` 参数，
 *          用于多核场景下的扇区锁归属判断：
 *              if ((LockDomainIDValue != 0xFF) && (LockDomainIDValue != DomainIdValue)) → 报错
 *          本工程是单核，而且 CT 里 Mem Sector Set Lock Api 是关闭的，
 *          所以传 0 即可。
 */
#define FLS_DOMAIN_ID               (0U)

/*==================================================================================================
 *                                 STRUCTURES AND OTHER TYPEDEFS
 *================================================================================================*/

/** @brief Flash 地址类型（32 位地址空间） */
typedef uint32 Fls_AddressType;

/** @brief Flash 长度类型 */
typedef uint32 Fls_LengthType;

/** @brief API 编号（Det 上报时用） */
typedef enum
{
    FLS_API_INIT                    = 0U,
    FLS_API_ERASE                   = 1U,
    FLS_API_WRITE                   = 2U,
    FLS_API_READ                    = 3U,
    FLS_API_BLANK_CHECK             = 4U
} Fls_ApiIdType;

/** @brief 错误编号（Det 上报时用） */
typedef enum
{
    FLS_E_PARAM_POINTER         = 0x01U,    /**< 传了 NULL 指针 */
    FLS_E_UNINIT                = 0x02U,    /**< 未初始化就调用 */
    FLS_E_PARAM_ADDRESS         = 0x03U,    /**< 地址越界或未对齐 */
    FLS_E_PARAM_LENGTH          = 0x04U,    /**< 长度为 0 */
    FLS_E_ERASE_FAILED          = 0x05U,    /**< 擦除失败（含校验失败） */
    FLS_E_WRITE_FAILED          = 0x06U,    /**< 写入失败（含校验失败） */
    FLS_E_TIMEOUT               = 0x07U,    /**< 等待硬件完成超时 */
    FLS_E_PROTECTED_REGION      = 0x08U,    /**< 试图擦写代码所在的 Block（禁止） */
    FLS_E_NOT_ERASED            = 0x09U     /**< 目标区不是擦除态 */
} Fls_ErrorIdType;

/*==================================================================================================
 *                              调试可见变量
 *================================================================================================*/

/**
 * @brief 擦除等待期间的状态轮询次数
 * @details 【这个变量的真正用途：验证 RWW】
 *
 *          擦除期间 CPU 一直在执行"读状态寄存器 → 判断 → 循环"这段代码。
 *          如果 RWW 在这个场景下无效（代码和被擦扇区在同一个 Block），
 *          那么取指会被硬件阻塞 —— 这段循环根本跑不起来，函数不会返回。
 *
 *          所以：**Fls_Erase 能正常返回，且本变量 > 0**
 *          → 证明"擦除期间 CPU 确实在取指执行"
 *          → 证明 RWW 在该场景下有效
 */
extern volatile uint32 g_FlsEraseSpinCount;

/** @brief 写入等待期间的轮询次数（同上，用于验证写入期间的取指） */
extern volatile uint32 g_FlsWriteSpinCount;

/**
 * @brief 最后一次 C40_Ip 操作返回的原始状态码
 * @details 取值是 C40_Ip_StatusType：
 *              0x00 SUCCESS          0x01 BUSY            0x02 ERROR
 *              0x03 ERROR_TIMEOUT    0x04 ERROR_INPUT_PARAM
 *              0x05 ERROR_BLANK_CHECK  0x06 ERROR_PROGRAM_VERIFY
 *              0x09 SECTOR_UNPROTECTED 0x0A SECTOR_PROTECTED
 *          0xEE 是我们自己写的"还没开始"标记。
 */
extern volatile uint8 g_FlsLastStatus;

/**
 * @brief 等待阶段返回的状态码
 * @details 和 g_FlsLastStatus 配合区分"发起被拒"和"执行失败"：
 *              g_FlsLastStatus = 发起结果（如 0x02 表示 PreCheck 拒绝）
 *              g_FlsWaitStatus = 等待结果（如 0x06 表示写入校验失败）
 *          两者都是 0x00 才算真正成功。
 */
extern volatile uint8 g_FlsWaitStatus;

/**
 * @brief 上一次 Fls_Erase 实际擦了多少个扇区
 * @details 【为什么需要这个】
 *          擦 1 MB 应该是 128 个扇区、耗时 2~10 秒。
 *          实测"9 ms 就返回成功"，唯一能证明循环是否真的跑完的
 *          就是这个计数 —— 光看返回值和耗时都可能被"假成功"骗过。
 */
extern volatile uint32 g_FlsLastEraseCount;

/**
 * @brief 上一次擦除里累计的"等待忙"轮询次数
 * @details 128 个扇区若都真的等过，应该是百万级；接近 0 说明等待没生效。
 */
extern volatile uint32 g_FlsTotalEraseSpin;

/**
 * @brief 因擦除后验证不通过而重擦的次数
 * @details 正常应该是 0。大于 0 说明 C40_Ip 报了"成功"但实际没擦干净
 *          （原因是 MCRS.PEG 位不会被清除，导致连续擦除时后续扇区被误报成功）。
 *          有了"擦后读回验证 + 重擦"，这个计数就是兜底机制的运行记录。
 */
extern volatile uint32 g_FlsEraseRetryCount;

/** @brief 本次 Fls_Erase/Fls_Write 里"最慢的一次等待"（毫秒，诊断用） */
extern volatile uint32 g_FlsMaxWaitMs;

/** @brief 上一次 BlankCheck 失败时，第一个非 0xFF 字节的绝对地址 */
extern volatile uint32 g_FlsBlankFailAddr;

/** @brief 该字节的实际值 */
extern volatile uint8  g_FlsBlankFailValue;

/** @brief 上一次擦除的起始扇区号 */
extern volatile uint32 g_FlsLastEraseStartSector;

/** @brief 上一次擦除的结束扇区号 */
extern volatile uint32 g_FlsLastEraseEndSector;

/**
 * @brief 写后校验失败的详细信息（诊断用）
 * @details 当 Fls_Write 的逐块校验发现不一致时，这几个变量会记录：
 *              g_FlsVerifyChunk     第几块（0 起）
 *              g_FlsVerifyAddr      该块的起始地址
 *              g_FlsVerifyLen       该块的长度
 *              g_FlsVerifyOffset    块内第一个不一致字节的偏移
 *              g_FlsVerifyExpected  期望值（源数据）
 *              g_FlsVerifyActual    实际读回值
 *          有了这些就能判断是"完全没写进去"、"部分写错"还是"读到了旧数据"。
 */
extern volatile uint32 g_FlsVerifyChunk;
extern volatile uint32 g_FlsVerifyAddr;
extern volatile uint32 g_FlsVerifyLen;
extern volatile uint32 g_FlsVerifyOffset;
extern volatile uint8  g_FlsVerifyExpected;
extern volatile uint8  g_FlsVerifyActual;

/** @brief 最后一次操作的扇区号（诊断用） */
extern volatile uint32 g_FlsLastSector;

/*==================================================================================================
 *                               诊断接口（只读，不修改任何状态）
 *================================================================================================*/

/**
 * @brief 读取 Flash 控制器的关键寄存器 + 目标扇区的锁定状态
 *
 * @details 这个函数存在的唯一目的：**当擦除失败时，一次就定位到原因**。
 *          因为 C40_Ip 的 PreCheck 有 4 个拒绝条件，只看返回的
 *          "笼统的 C40_IP_STATUS_ERROR" 分不出是哪一个。
 *
 *          4 个条件与下面的输出对应关系：
 *              VirtualSector 越界     → 看 g_FlsLastSector
 *              MCR 里有 ERS/PGM 残留  → 看 *MCR 的 bit[3](ERS) 和 bit[4](PGM)
 *              UT0 的 UTE 被置位      → 看 *UT0 的 bit[0]
 *              扇区被锁               → 看 *LockState（0x09=未锁，0x0A=已锁）
 *
 * @param[out] MCR       Flash 主控制寄存器（MCR）
 * @param[out] UT0       Flash UTest 控制寄存器（UT0）
 * @param[out] MCRS      Flash 主控制状态寄存器（MCRS）
 * @param[out] LockState C40_Ip_GetLock() 的结果（0x09=未锁 / 0x0A=已锁 / 0x04=扇区非法）
 *
 * 任何一个指针传 NULL_PTR 都表示"这一项不取"。
 */
void Fls_GetDiag(uint32* MCR, uint32* UT0, uint32* MCRS, uint8* LockState);

/*==================================================================================================
 *                                    FUNCTION PROTOTYPES
 *================================================================================================*/

/**
 * @brief 初始化 Flash 驱动
 *
 * @details 调用 C40_Ip_Init，并登记"已初始化"状态。
 *          必须在 Clock_Ip_Init 之后调用（Flash 时序依赖正确的时钟）。
 *
 * @return E_OK 成功；E_NOT_OK 失败
 */
Std_ReturnType Fls_Init(void);

/**
 * @brief 擦除一段地址所覆盖的**所有扇区**
 *
 * @details 上层给的是"字节范围"，本函数内部：
 *          ① 算出起始扇区与结束扇区
 *          ② 逐个扇区调用 C40_Ip_MainInterfaceSectorErase + 等待完成
 *
 *          ⚠️ 擦除粒度是 8 KB：即使只要擦 1 个字节，也会擦掉整个扇区。
 *
 * @param[in] TargetAddress 起始地址（不要求对齐，内部会按扇区向下取整）
 * @param[in] Length        字节数（必须 > 0）
 *
 * @return E_OK        整段擦除完成（且通过校验，若校验已开启）
 *         E_NOT_OK    参数错误 / 落入保护区 / 硬件失败 / 超时
 */
Std_ReturnType Fls_Erase(Fls_AddressType TargetAddress, Fls_LengthType Length);

/**
 * @brief 写入一段数据
 *
 * @details 内部自动处理三个硬约束：
 *          ① 地址按 8 字节向下对齐（对齐前的少量字节需调用方自己保证已擦除）
 *          ② 单次不超过 128 字节，且不跨 128 字节边界 → 自动拆包
 *          ③ 尾部不足 8 字节倍数时，用 0xFF 补齐（写 0xFF 到已擦除区无害）
 *
 *          ⚠️ 写入前该区域必须已经擦除，否则数据会被"与"运算破坏。
 *
 * @param[in] TargetAddress 目标地址（必须 8 字节对齐）
 * @param[in] SourceAddressPtr 源数据指针
 * @param[in] Length        字节数（必须 > 0）
 *
 * @return E_OK 写入完成（且通过校验，若校验已开启）
 */
Std_ReturnType Fls_Write(Fls_AddressType TargetAddress,
                         const uint8* SourceAddressPtr,
                         Fls_LengthType Length);

/**
 * @brief 读取一段数据
 * @details Flash 可以直接按字节读，没有对齐和粒度限制。
 *
 *          【⚠️ 这里藏着本项目最隐蔽的一个坑：CPU 的 D-Cache 会缓存 Flash】
 *
 *          本工程有两件事同时成立：
 *              ① system.c 使能了 D-Cache（编译参数带 -DD_CACHE_ENABLE）
 *              ② system.c 的 MPU 表把 Program Flash 配成 Write-Back/Allocate（可缓存）
 *
 *          于是：
 *              CPU 读 Flash      → 数据进入 D-Cache
 *              Flash 控制器擦/写 → **CPU 缓存不会自动失效**
 *              再读同一地址      → 命中缓存 → 拿到擦写之前的旧数据
 *
 *          【实测症状 —— 极易被误判成"擦除没生效"】
 *              擦除 → 读回仍是写入时的数据 0x11 0x12 ...
 *              直接指针读同一地址 → 也是 0x11（同样经过 CPU 缓存，
 *                  所以"两条独立的读法"会一起给出错误答案）
 *              但立刻再擦一次只用 0.26 ms（和擦空扇区一样快）
 *                  → 说明硬件认为扇区已经空了 → **第一次擦除其实成功了**
 *
 *          【解法：用厂商机制，本模块不写任何缓存维护代码】
 *          C40_Ip 自带 C40_Ip_SynchronizeCache()，它会在合适的位置调用
 *          Cache_Ip_InvalidateByAddr() 失效 CPU 的 D-Cache：
 *              擦除完成 → C40_Ip_Compare（内建查空）→ 先失效缓存再比对
 *              每次读   → C40_Ip_Read 开头 → 先失效缓存再读
 *          这一切由下面四个 CT 开关控制，都在 CT 里打开即可：
 *
 *              C40_Ip → MemGeneral：
 *                  ☑ Mem ECC Enable Check     （失效 Flash 控制器的预取缓冲）
 *                  ☑ Erase Verification       （擦完自动查空，判据可信）
 *                  ☑ Program Verification     （写完自动校验）
 *                  ☑ Synchronize Cache        （失效 CPU 的 D-Cache）
 *              Cache Ip 组件：
 *                  ☑ 启用                     （CACHE_IP_IS_AVAILABLE = STD_ON）
 *
 *          **少任何一个，擦写后的读都可能不可靠。**
 *          四个全开之后实测：连续 3 轮"擦除→查空→写入→读回"全部 MATCH。
 *
 *          【历史】在发现这些开关之前，本文件曾手写 DCIMVAC 循环做同样的事，
 *          以及手写"擦后读回校验"。现已全部删除 —— 用厂商机制代码更少、
 *          出处更清楚、也更容易向别人解释。
 */
Std_ReturnType Fls_Read(Fls_AddressType SourceAddress,
                        uint8* TargetAddressPtr,
                        Fls_LengthType Length);

/**
 * @brief 检查一段区域是否处于擦除态（全 0xFF）
 *
 * @details 用途：写完一帧后确认"这块确实擦干净了"，或者校验操作。
 *          实现上分块读取（每块 128 字节）逐字节比对 0xFF。
 *
 * @return E_OK 全为 0xFF（已擦除）；E_NOT_OK 有非 0xFF 内容
 */
Std_ReturnType Fls_BlankCheck(Fls_AddressType TargetAddress, Fls_LengthType Length);

/**
 * @brief 查询某个地址落在哪个扇区号
 * @details 只是对 C40_Ip_GetSectorNumberFromAddress 的包装，
 *          方便上层调试打印。返回值是 C40_Ip 的虚拟扇区号（16 起）。
 */
uint32 Fls_GetSectorNumber(Fls_AddressType Address);

/**
 * @brief 查询某个地址落在哪个 Block（0 ~ 3）
 * @details 用于调试和验证"擦除目标是否和代码在同一个 Block"。
 *          返回值 0xFF 表示地址不在 P-Flash 范围内。
 */
uint8 Fls_GetBlockNumber(Fls_AddressType Address);

/**
 * @brief 返回 fls.c 自己的编译日期时间字符串
 *
 * @details 【为什么需要这个】
 *          工程 banner 上那个 `__DATE__ __TIME__` 是写在 main.c 里的，
 *          它只反映 **main.c** 的编译时间。如果只改了 fls.c 而 main.c 没变，
 *          Eclipse 增量编译不会重编 main.c，于是 banner 时间看起来"没更新"，
 *          很容易让人误以为烧进去的是旧固件。
 *
 *          所以这里把 fls.c 的编译时间也导出来，让 banner 能同时显示
 *          "main.c 什么时候编的"和"fls.c 什么时候编的"。
 *
 * @return 形如 "Oct 21 2026 22:35:10" 的常量字符串
 */
const char* Fls_GetBuildStamp(void);

#endif /* FLS_H */
