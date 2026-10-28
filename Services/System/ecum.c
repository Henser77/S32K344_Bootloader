/*==================================================================================================
 *  Services/System/ecum.c —— EcuM 实现（只做启动 + 裸机调度）
 *
 *  【初始化顺序为什么是这个顺序】
 *
 *      Clock      所有外设的时钟源，必须最先
 *        ↓
 *      Port       引脚复用（UART/CAN 的引脚）
 *        ↓
 *      IntCtrl    中断向量使能
 *        ↓
 *      Det        错误上报通道（后面所有模块都可能用它）
 *        ↓
 *      Uart       调试输出（越早越好，后面每一步都要打印结果）
 *        ↓
 *      Fls        Flash 驱动（内部只做 C40_Ip_Init）
 *        ↓
 *      Can        CAN 控制器（此时控制器还是 STOPPED 状态）
 *        ↓
 *      CanIf      CAN 接口层 —— 在这里把 Can 的收发回调接起来
 *        ↓
 *      PduR       路由层
 *        ↓
 *      CanTp      传输层（用 CanIf 发流控帧，所以要在 CanIf 之后）
 *        ↓
 *      Dcm        诊断层（响应最终经 PduR→CanTp 发出去，所以最后）
 *        ↓
 *      Can 控制器 STARTED    —— 最后才真正上总线，避免一起床就收帧
 *
 *  【最后一步为什么单独提出来】
 *  如果先 STARTED 再初始化 CanTp/Dcm，那么初始化期间收到的帧会进来，
 *  而上面的模块还没准备好 —— 会出现"收到帧但没人处理"的诡异现象。
 * ==================================================================================================*/
#include "S32K344_SCB.h"
#include "ecum.h"

/* 板级 / RTD */
#include "Clock_Ip.h"
#include "Siul2_Port_Ip.h"
#include "Siul2_Port_Ip_Cfg.h"
#include "IntCtrl_Ip.h"
#include "IntCtrl_Ip_Cfg.h"
#include "Det.h"

/* MCAL */
#include "uart.h"
#include "fls.h"
#include "can.h"
#include "can_cfg.h"

/* ECU 抽象层 */
#include "canif.h"

/* 服务层 */
#include "pdur.h"
#include "cantp.h"
#include "dcm.h"

/* 调试 */
#include "boot_trace.h"
#include "boot_param.h"
#include "boot_app.h"
#include "boot_version.h"

/*==================================================================================================
 *                                      本地常量
 *================================================================================================*/

/** @brief 使用的 CAN 控制器编号（0 = CAN0） */
#define ECUM_CAN_CONTROLLER     (0U)

/*==================================================================================================
 *                                      内部辅助
 *================================================================================================*/

/**
 * @brief 致命错误：打印原因并停机
 *
 * @details Bootloader 的关键外设（时钟/Flash/CAN）坏了，程序没有意义继续跑。
 *          停机比"带着坏掉的外设继续跑"更安全 —— 因为后者可能
 *          在刷写到一半时停下来，把 App 区写坏。
 *
 * @note 用死循环而不是复位：复位会无限重启刷屏，反而看不到原因。
 */
static void EcuM_Fatal(const char* Reason)
{
#if (UART_PRINTING_DISABLED == STD_OFF)
    Uart_Printf("\n[致命错误] %s\n", Reason);
    Uart_Printf("            Bootloader 已停止，请检查硬件/时钟配置。\n");
#else
    /* 发布版没有串口。产品级做法通常是：
     *   ① 关掉 CAN 控制器，别在总线上发垃圾帧
     *   ② 等看门狗复位（本工程没启用 SWT，所以只能停住）
     * 停住比"带着坏掉的外设继续跑"安全 —— 后者可能在刷写到一半时停下，
     * 把 App 区写坏。这里和开发版唯一的区别是：不打印、不刷屏。 */
    (void)Reason;
#endif

    for (;;)
    {
        /* 停在这里，不要继续 */
    }
}

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

/*--------------------------------------------------------------------------------------------------
 *                              启动窗口（BOOT_HOLD_WINDOW_MS）
 *
 * 【它解决什么问题】
 *  App 有效时，Bootloader 上电就跳走。而 App 通常不认 UDS，
 *  于是"想再刷一次"只能靠调试器擦参数区 —— 非常别扭。
 *
 *  有了这个窗口：
 *      上电 -> CAN 起来 -> 等 1.5 秒
 *          |- 期间收到任意 UDS 请求 -> 留在 Bootloader（可以刷写）
 *          \- 一直没人来 -> 跳 App
 *
 *  上位机的配合方式：先开始循环发 3E 00，再给板子断电重上电。
 *------------------------------------------------------------------------------------------------*/

/** @brief DWT 周期计数器（和 Dcm/CanTp 用的是同一个硬件计数器） */
#define ECUM_REG_DWT_CYCCNT         (*(volatile uint32*)0xE0001004UL)

/** @brief 160 MHz 内核 -> 每毫秒 160000 个周期 */
#define ECUM_CYCLES_PER_MS          (160000UL)

/**
 * @brief 在启动窗口内等诊断仪；返回 TRUE 表示"有人来找我，应该留下"
 *
 * @details 等待期间必须继续跑主函数，否则收不到帧
 *          （CanTp 负责拼装、PduR 负责交付、Dcm 负责解析）。
 */
static boolean EcuM_WaitBootHoldWindow(void)
{
#if (BOOT_HOLD_WINDOW_MS == 0U)
    /* 【发布版走这里】窗口关闭：上电直接跳 App，启动最快。
     * 变量声明也放进 #else，否则发布版会报"未使用变量"。 */
    return FALSE;
#else
    uint32 start;
    uint32 reqAtEntry;

    reqAtEntry = Dcm_GetRequestCount();
    start      = ECUM_REG_DWT_CYCCNT;

    Uart_Printf("[启动] 启动窗口 %u ms —— 现在开始循环发 3E 00 就能留下刷写\n",
                (uint32)BOOT_HOLD_WINDOW_MS);

    while ((uint32)(ECUM_REG_DWT_CYCCNT - start) <
           (uint32)((uint32)BOOT_HOLD_WINDOW_MS * ECUM_CYCLES_PER_MS))
    {
        CanTp_MainFunction();
        PduR_MainFunction();
        Dcm_MainFunction();
        BootTrace_MainFunction();

        if (Dcm_GetRequestCount() != reqAtEntry)
        {
            Uart_Printf("[启动] 窗口内收到诊断请求 -> 留在 Bootloader\n");
            return TRUE;
        }
    }

    Uart_Printf("[启动] 窗口结束，没有诊断请求\n");
    return FALSE;
#endif
}

void EcuM_Init(void)
{
    boolean JumpToApp = FALSE;   /* 上电校验结果：TRUE 表示 App 可以跳 */

    /*--------------------------------------------------------------------------
     * 第 1 步：板级初始化
     *------------------------------------------------------------------------*/
    Clock_Ip_Init(&Clock_Ip_aClockConfig[0]);
    (void)Siul2_Port_Ip_Init(NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
                             g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);
    IntCtrl_Ip_Init(&IntCtrlConfig_0);

    /*--------------------------------------------------------------------------
     * 第 2 步：Det（开发错误上报）
     *
     * Det_ReportError 内部在未初始化时会自动 Det_Init()，
     * 这里显式调用是为了"每次上电从干净状态开始"。
     *------------------------------------------------------------------------*/
    Det_Init();

    /*--------------------------------------------------------------------------
     * 第 3 步：串口（后面每一步都要打印结果，所以尽早）
     *------------------------------------------------------------------------*/
    Uart_Init();

    Uart_Printf("\n");
    Uart_Printf("========================================\n");
    Uart_Printf("  S32K344 Bootloader  v" BOOT_FW_VERSION "\n");
    Uart_Printf("  构建时间: " __DATE__ " " __TIME__ "\n");
    Uart_Printf("  功能: " BOOT_FW_FEATURES "\n");
    Uart_Printf("  构建: " BOOT_BUILD_TAG "（日志级别 %u）\n",
              (uint32)BOOT_LOG_LEVEL);
    Uart_Printf("========================================\n");

    /*--------------------------------------------------------------------------
     * 第 4 步：Flash 驱动
     *
     * Fls_Init() 内部只做一件事：C40_Ip_Init(&C40_Ip_InitCfg)。
     * 后面的 Fls 封装和 Dcm 擦除服务都依赖它。
     *
     * 【教训】早期版本把它藏在调试自检函数里，后来自检被关掉，
     * Fls_Init 就再也没被调用过 —— 所有擦/写/读都在第一行返回 E_NOT_OK，
     * 表现为"擦除瞬间失败"却查不出原因。模块初始化绝不能依赖调试代码。
     *------------------------------------------------------------------------*/
    if (Fls_Init() != E_OK)
    {
        EcuM_Fatal("Flash 驱动初始化失败，无法刷写");
    }
    Uart_Printf("Flash 驱动 : 初始化成功（构建于 %s）\n", Fls_GetBuildStamp());
    /*--------------------------------------------------------------------------
     * 第 4.5 步：上电校验 App 是否完整（只判断，不跳）
     *
     * 【为什么这里只判断、不跳】
     *  因为"启动窗口"需要 CAN 先工作起来：
     *      上电 -> 校验通过 -> 初始化 CAN 栈 -> 等 1.5 秒看有没有诊断仪来
     *          -> 来了就留下刷写，没来才跳 App
     *  所以跳转动作挪到 CAN 启动之后（见函数末尾）。
     *
     * 判断链（任何一步失败都留在 Bootloader，绝不冒险跳）：
     *     账本结构有效？ -> 账本说 App 有效？ -> App 地址/长度合法？
     *     -> 向量表 SP 在 RAM 内？ -> 复位向量落在 App 区？
     *     -> App 区 CRC32 与账本一致？ -> 全部通过才算"可以跳"
     *------------------------------------------------------------------------*/
    BootParam_Load();

    JumpToApp = (BootApp_Decide() == BOOT_DECIDE_JUMP) ? TRUE : FALSE;

    if (JumpToApp == FALSE)
    {
        Uart_Printf("[启动] 留在 Bootloader（App 不可跳），等待诊断仪刷写\n");
    }

    /*--------------------------------------------------------------------------
     * 第 4.6 步：账本里有没有"请求进 Bootloader"的标志？
     *
     * 【为什么需要这一条】
     *  发布版的启动窗口是 0 毫秒（上电直接跳 App，启动最快）。
     *  如果 App 校验通过、而我们又想重刷固件，就完全没有软件入口了 ——
     *  这是 2026-10-25 实测踩到的坑（表现为"看起来 Bootloader 没了"）。
     *
     *  BootRequest 就是第二条入口：App 或诊断仪先在账本里按下门铃，
     *  下次上电 Bootloader 看到它就留下来。
     *
     *  【注意"只消费一次"】
     *  看到请求后立刻清掉。否则每次上电都会留在 Bootloader，
     *  用户会以为 App 坏了。
     *------------------------------------------------------------------------*/
    if (BootParam_IsBootRequested() != FALSE)
    {
        if (BootParam_ClearBootRequest() == E_OK)
        {
            Uart_Printf("[启动] 账本里有 BootRequest -> 已清掉，留在 Bootloader 等刷写\n");
        }
        else
        {
            /* 清不掉（Flash 写失败）也要留在 Bootloader —— 刷写比启动更重要 */
            Uart_Printf("[启动] 清 BootRequest 失败，但仍留在 Bootloader\n");
        }

        JumpToApp = FALSE;      /* 覆盖上面的校验结论 */
    }


    /*--------------------------------------------------------------------------
     * 第 5 步：CAN 控制器（此时仍是 STOPPED，真正上总线放到最后）
     *------------------------------------------------------------------------*/
    Can_Init(&Can_Config);

    /*--------------------------------------------------------------------------
     * 第 6 步：CanIf —— 在 Can_Init 之后，因为要注册收发回调
     *------------------------------------------------------------------------*/
    CanIf_Init();

    /*--------------------------------------------------------------------------
     * 第 7 步：PduR 路由层
     *------------------------------------------------------------------------*/
    PduR_Init();

    /*--------------------------------------------------------------------------
     * 第 8 步：CanTp 传输层（用 CanIf 发流控帧，所以必须在 CanIf 之后）
     *------------------------------------------------------------------------*/
    CanTp_Init(NULL_PTR);

    Uart_Printf("传输层 CanTp : 构建于 %s，时间基准 %s\n",
                CanTp_GetBuildStamp(),
                (CanTp_IsTimeBaseOk() != FALSE) ? "正常" : "异常");

    /* 把实际生效的超时值打出来 —— 一眼就能确认烧进去的是哪个版本的配置。
     * 曾经因为"以为烧了新的、其实还是旧的"浪费过一次排查时间。 */
    Uart_Printf("传输层 CanTp : N_Bs/N_Cr = %u/%u 毫秒（手工测试模式: %s）\n",
                (uint32)CANTP_N_BS_MS, (uint32)CANTP_N_CR_MS,
                (CANTP_MANUAL_TEST_MODE != 0) ? "开" : "关");

    /*--------------------------------------------------------------------------
     * 第 9 步：Dcm 诊断层（响应最终经 PduR → CanTp 发出去，所以放在最后）
     *------------------------------------------------------------------------*/
    Dcm_Init(NULL_PTR);

    Uart_Printf("诊断层 Dcm  : 构建于 %s\n", Dcm_GetBuildStamp());
    Uart_Printf("诊断层 Dcm  : S3 = %u 毫秒（手工测试模式: %s）\n",
                (uint32)DCM_S3_MS,
                (DCM_MANUAL_TEST_MODE != 0) ? "开" : "关");

    /*--------------------------------------------------------------------------
     * 第 10 步：调试上报初始化（注册 CanIf 的接收观察回调）
     *------------------------------------------------------------------------*/
    BootTrace_Init();

    /*--------------------------------------------------------------------------
     * 第 11 步：最后才让 CAN 控制器上总线
     *
     * 前面所有模块都准备好了，此刻开始收帧就不会出现"收到帧但没人处理"。
     *------------------------------------------------------------------------*/
    if (Can_SetControllerMode((uint8)ECUM_CAN_CONTROLLER, CAN_CS_STARTED) != E_OK)
    {
        EcuM_Fatal("CAN0 启动失败");
    }

    Uart_Printf("CAN0 已启动：500 kbps，接收 ID 0x%03X，响应 ID 0x%03X\n",
                (uint32)CANIF_RX_CAN_ID_UDS, (uint32)CANIF_TX_CAN_ID_UDS);
    Uart_Printf("Det 错误计数 = %u\n", (uint32)Det_numEventErrors[0]);
    Uart_Printf("当前状态：默认会话 / 安全未解锁\n");
    Uart_Printf("等待 UDS 诊断请求...\n\n");
    /*--------------------------------------------------------------------------
     * 第 12 步：启动窗口 —— 决定"跳 App"还是"留下刷写"
     *
     * 只有 App 校验通过才需要这一步；App 不可跳的场合应该一直留着。
     *------------------------------------------------------------------------*/
    if (JumpToApp != FALSE)
    {
        if (EcuM_WaitBootHoldWindow() == FALSE)
        {
            Uart_Printf("[启动] 准备跳转到 0x%08X\n", BOOT_APP_START);

            /* 跳之前停掉 CAN：App 会重新初始化它，
             * 留着旧配置可能让 App 收到残留中断。 */
            (void)Can_SetControllerMode((uint8)ECUM_CAN_CONTROLLER, CAN_CS_STOPPED);
            Uart_Printf("[启动] 已停止 CAN 控制器，即将跳转...\n");

            BootApp_Jump(BOOT_APP_START);      /* 不返回 */
        }
    }
}
/*--------------------------------------------------------------------------------------------------
 *                        诊断仪请求的复位（服务 0x11 01）
 *
 * 【为什么复位动作必须放在这里】
 *  Dcm_HandleEcuReset() 只把 Dcm_ResetRequested 置起来就返回 ——
 *  它不能当场复位，因为 51 01 这条正响应还在发送队列里，
 *  提前复位会让诊断仪收不到响应，误判成"复位失败"。
 *
 * 【曾经漏掉这个】
 *  2026-10-24 实测踩到：0x11 01 只置标志、没有代码执行复位，
 *  表现成"刷写成功、CRC 一致、但按完复位还回 Bootloader"。
 *  参数区其实已经写好了 AppValid，只是没人重启 MCU。
 *------------------------------------------------------------------------------------------------*/

/**
 * @brief MC_ME（Mode Entry）寄存器 —— S32K3 上"软件复位"真正的入口
 *
 * @details 地址和位定义全部来自 NXP 的设备头文件，不是猜的：
 *      S32K344_MC_ME.h:146   #define IP_MC_ME_BASE            (0x402DC000u)
 *      S32K344_MC_ME.h:175   MC_ME_MODE_CONF_DEST_RST_MASK  (0x1U)  <- 破坏性复位
 *      S32K344_MC_ME.h:180   MC_ME_MODE_CONF_FUNC_RST_MASK  (0x2U)  <- 功能复位
 *      S32K344_MC_ME.h:194   MC_ME_MODE_UPD_MODE_UPD_MASK   (0x1U)  <- 触发更新
 *      Clock_Ip_Specific.c   CTL_KEY = 0x5AF0 / 0xA50F（解锁序列）
 *
 *  偏移量：CTL_KEY = +0x00，MODE_CONF = +0x04，MODE_UPD = +0x08
 */
#define ECUM_REG_MC_ME_CTL_KEY      (*(volatile uint32*)0x402DC000UL)
#define ECUM_REG_MC_ME_MODE_CONF    (*(volatile uint32*)0x402DC004UL)
#define ECUM_REG_MC_ME_MODE_UPD     (*(volatile uint32*)0x402DC008UL)

/** @brief 请求"功能复位"（FUNC_RST = bit1）。比破坏性复位温和，SRAM 内容保留 */
#define ECUM_MC_ME_MODE_CONF_FUNC_RST   (0x2UL)

/** @brief 请求"破坏性复位"（DEST_RST = bit0）。更彻底，SRAM 会被清 */
#define ECUM_MC_ME_MODE_CONF_DEST_RST   (0x1UL)

/** @brief 触发模式更新（MODE_UPD = bit0）—— 写这个位硬件才会真正动作 */
#define ECUM_MC_ME_MODE_UPD_TRIGGER     (0x1UL)

/** @brief MC_ME 的解锁序列（0x5AF0 = "SAFE"，0xA50F = 它的反码） */
#define ECUM_MC_ME_KEY_SAFE             (0x5AF0UL)
#define ECUM_MC_ME_KEY_SAFE_INV         (0xA50FUL)

/** @brief 置标志后等多久才复位（毫秒）—— 要让 51 01 真的发上总线 */
#define ECUM_RESET_DELAY_MS         (100U)

/** @brief 请求复位后等多久判断"没成功"（毫秒） */
#define ECUM_RESET_SETTLE_MS        (50U)

static boolean EcuM_ResetArmed      = FALSE;
static uint32  EcuM_ResetStartCycle = 0U;

/**
 * @brief 触发一次"复位事件"（严格照 NXP Power_Ip_MC_ME_SocTriggerResetEvent 的顺序）
 *
 * @param[in] ModeConfBits  ECUM_MC_ME_MODE_CONF_FUNC_RST 或 ..._DEST_RST
 *
 * @details 【顺序为什么是这个 —— 这是第一次失败的原因】
 *
 *  NXP 的实现是三步：
 *      Power_Ip_MC_ME_SocTriggerResetEvent():
 *          MC_ME->MODE_CONF = FUNC_RST       ① 告诉硬件"要做什么"
 *          Power_Ip_MC_ME_TriggerModeUpdate():
 *              MC_ME->MODE_UPD = 1           ② 提交
 *              Power_Ip_MC_ME_WriteControlKeys():
 *                  CTL_KEY = 0x5AF0          ③ 写钥匙 —— "启动硬件动作"
 *                  CTL_KEY = 0xA50F
 *
 *  注意第 ③ 步的注释原文就是 "Starting the hardware processes"：
 *  **钥匙是把前面配置"启动"起来的那一下**，必须放在最后。
 *
 *  之前我把钥匙写在最前面（先解锁再配置），结果：
 *  "启动"信号发出时配置还是空的 → 硬件没动；
 *  之后写的 MODE_CONF/MODE_UPD 只是躺在寄存器里，没人启动它们。
 *  这就是 v1.9/v1.12 两次都"没复位"的原因。
 */
static void EcuM_TriggerResetEvent(uint32 ModeConfBits)
{
    ECUM_REG_MC_ME_MODE_CONF = ModeConfBits;                    /* ① 要做什么 */
    ECUM_REG_MC_ME_MODE_UPD  = ECUM_MC_ME_MODE_UPD_TRIGGER;     /* ② 提交 */
    ECUM_REG_MC_ME_CTL_KEY   = ECUM_MC_ME_KEY_SAFE;             /* ③ 启动 */
    ECUM_REG_MC_ME_CTL_KEY   = ECUM_MC_ME_KEY_SAFE_INV;

    __asm volatile ("dsb");
}

/**
 * @brief 等一小会儿看复位有没有发生
 * @return TRUE = 还能返回，说明复位没成功
 */
static boolean EcuM_WaitResetSettled(void)
{
    uint32 start = ECUM_REG_DWT_CYCCNT;

    while ((uint32)(ECUM_REG_DWT_CYCCNT - start) <
           (uint32)(ECUM_RESET_SETTLE_MS * ECUM_CYCLES_PER_MS))
    {
        /* 空转等复位；真复位了就走不到 return */
    }

    return TRUE;
}

/**
 * @brief 请求一次系统复位（先功能复位，不行再破坏性复位）
 *
 * @details 【一次烧录试两种强度】
 *  第 1 次：FUNC_RST（功能复位，SRAM 保留，温和）
 *  第 2 次：DEST_RST（破坏性复位，SRAM 清空，更彻底）
 *
 *  如果两次都没复位，说明 MC_ME 这条路在我们工程里还缺东西
 *  （NXP 的 Mcu_PerformReset 依赖 Mcu_Init 先初始化 MC_ME，
 *    而本工程没链接 Power/Mcu 模块）。
 *
 * @return TRUE  = 两次都没复位（调用方要降级为"直接跳转"）
 *         FALSE = 不会发生（真复位就走不到 return）
 */
static boolean EcuM_RequestSystemReset(void)
{
    /* 第 1 次：功能复位 */
    EcuM_TriggerResetEvent(ECUM_MC_ME_MODE_CONF_FUNC_RST);

    if (EcuM_WaitResetSettled() == FALSE)
    {
        return FALSE;
    }

    /* 第 2 次：破坏性复位 */
    Uart_Printf("[启动] 功能复位未生效，再试破坏性复位\n");
    EcuM_TriggerResetEvent(ECUM_MC_ME_MODE_CONF_DEST_RST);

    if (EcuM_WaitResetSettled() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

void EcuM_MainFunction(void)
{
    /* CanTp 必须每圈调用：它负责 N_Bs/N_Cr 超时检查、回流控帧、逐帧发连续帧。
     * 调用越快，时序越准 —— 主循环里不要再塞阻塞操作。 */
    CanTp_MainFunction();

    /* Dcm 每圈调用：负责 S3 会话超时检查，以及把排队的响应发出去 */
    Dcm_MainFunction();
    /*--------------------------------------------------------------------------
     * 诊断仪请求复位？（服务 0x11 01）
     *
     * Dcm 收到 11 01 → 置标志 → 这里看到标志 → 等 100 ms（让 51 01 发完）
     * → 写 AIRCR 复位。
     *------------------------------------------------------------------------*/
    if (Dcm_IsResetRequested() != FALSE)
    {
        if (EcuM_ResetArmed == FALSE)
        {
            EcuM_ResetArmed      = TRUE;
            EcuM_ResetStartCycle = ECUM_REG_DWT_CYCCNT;
            Uart_Printf("[启动] 收到 11 01 复位请求，100 ms 后重启 MCU\n");
        }

        if ((uint32)(ECUM_REG_DWT_CYCCNT - EcuM_ResetStartCycle) >=
            (uint32)(ECUM_RESET_DELAY_MS * ECUM_CYCLES_PER_MS))
        {
            Uart_Printf("[启动] 正在请求复位...\n");

            /* 复位没成功的话，这个函数会返回 —— 必须清标志，
             * 否则主循环每圈都重试，板子等于死机。 */
            if (EcuM_RequestSystemReset() != FALSE)
            {
                /*--------------------------------------------------------------------
                 * 复位没生效 —— 退而求其次：直接跳转到 App
                 *
                 * 【为什么这样可以】
                 *  跳转本来就是 Bootloader 支持的动作（上电校验通过时用的同一套）：
                 *  关总中断 -> 设 VTOR -> 换 MSP -> 跳 PC。
                 *  跳之前把 CAN 停掉，App 会自己重新初始化外设。
                 *
                 * 【为什么重新校验一次】
                 *  刚刷写完的那份 App 必须真的完整才敢跳，不能凭"刚才写过"就跳。
                 *  BootApp_Decide() 会重算整个 App 区的 CRC（约 400 ms），
                 *  不通过就老老实实留在 Bootloader。
                 *--------------------------------------------------------------------*/
                Uart_Printf("[启动] 复位未生效，改为直接跳转 App\n");

                Dcm_ClearResetRequested();
                EcuM_ResetArmed      = FALSE;
                EcuM_ResetStartCycle = 0U;

                if (BootApp_Decide() == BOOT_DECIDE_JUMP)
                {
                    (void)Can_SetControllerMode((uint8)ECUM_CAN_CONTROLLER, CAN_CS_STOPPED);
                    Uart_Printf("[启动] App 校验通过，跳转到 0x%08X\n", BOOT_APP_START);
                    BootApp_Jump(BOOT_APP_START);       /* 不返回 */
                }

                Uart_Printf("[启动] App 校验不通过，留在 Bootloader 等待重刷\n");
            }
        }
    }
}
