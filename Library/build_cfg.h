/*==================================================================================================
 *  Library/build_cfg.h —— 开发版 / 发布版的唯一开关
 *
 *  【为什么需要这个文件】
 *
 *  同一个工程要同时服务两种场景：
 *      开发期：要 DET、要串口打印、要宽松超时，方便定位问题
 *      发布期：这些统统不能留 —— 占 Flash、拖慢时序、泄露信息
 *
 *  以前这些开关散在 5 个文件里共 8 个，发布时手工逐个改，**已经漏过一次**
 *  （boot_version.h 的原话："差点漏掉"）。
 *
 *  现在改成：只改下面 BOOT_BUILD_MODE 一行（或者用 IDE 的构建配置传 -D），
 *  其余全部在这里统一派生。别的文件不许再单独定义这些宏。
 *
 *  【放在 Library/ 层的原因】
 *  MCAL（can/fls/uart）和服务层（cantp/dcm）都要读这些开关，
 *  而 MCAL 不能反向依赖 Application 层，所以放在最底层的 Library/。
 *
 *  【怎么切到发布版】
 *  方法一（推荐）：在 S32DS 的 Release_FLASH 配置里加一个宏定义
 *       工程右键 → Properties → C/C++ Build → Settings
 *         → Tool Settings → Cross ARM C Compiler → Preprocessor
 *         → Defined symbols (-D) 里加：  BOOT_BUILD_MODE=1
 *      这样 Debug_FLASH 配置编译出来是开发版，Release_FLASH 是发布版，
 *      不用改任何源码。
 *
 *  方法二：直接改下面的默认值（简单，但容易忘改回来）
 *
 *  【发布版到底关掉了什么】
 *      BOOT_LOG_LEVEL          = NONE   -> Uart_Printf/Uart_SendString 整体编译掉
 *      BSW_DEV_ERROR_DETECT    = OFF    -> Can/Fls/Uart/CanTp/Dcm 的 DET 一起关
 *      BOOT_TRACE_ENABLE       = OFF    -> 调试追踪模块（约 400 行）整体编译掉
 *      BOOT_TRACE_SHOW_KEY     = OFF    -> 不打印"应回密钥"（等于不公开解锁方法）
 *      DCM_ENABLE_TRUST_APP    = OFF    -> 关掉 0xFF02 这个"不验货就签收"的后门
 *      CANTP_MANUAL_TEST_MODE  = OFF    -> N_Bs/N_Cr 从 60 秒回到标准 1 秒
 *      DCM_MANUAL_TEST_MODE    = OFF    -> S3 从 120 秒回到标准 5 秒
 *      BOOT_HOLD_WINDOW_MS     = 0      -> 上电直接跳 App，不等诊断仪
 *================================================================================================*/
#ifndef BUILD_CFG_H
#define BUILD_CFG_H

#include "Std_Types.h"

/*--------------------------------------------------------------------------------------------------
 *                                    构建模式
 *------------------------------------------------------------------------------------------------*/
#define BOOT_BUILD_DEBUG            (0)
#define BOOT_BUILD_RELEASE          (1)

/**
 * @brief 构建模式 —— **唯一需要改的一行**
 * @note  可以被编译选项 -DBOOT_BUILD_MODE=1 覆盖（推荐做法，见文件头说明）
 */
#ifndef BOOT_BUILD_MODE
    #define BOOT_BUILD_MODE         BOOT_BUILD_DEBUG
#endif

/*--------------------------------------------------------------------------------------------------
 *                                    日志级别
 *------------------------------------------------------------------------------------------------*/
#define BOOT_LOG_NONE               (0)     /**< 什么都不打印（发布版）           */
#define BOOT_LOG_ERROR              (1)     /**< 只打印致命错误                   */
#define BOOT_LOG_INFO               (2)     /**< + 关键流程（启动/跳转/擦除结果）  */
#define BOOT_LOG_VERBOSE            (3)     /**< + 每条 UDS 请求、CAN 统计（开发） */

/*--------------------------------------------------------------------------------------------------
 *                        派生所有子开关（唯一的派生点，别处不许再定义）
 *------------------------------------------------------------------------------------------------*/
#if (BOOT_BUILD_MODE == BOOT_BUILD_RELEASE)

    #define BOOT_RELEASE_MODE               (1)
    #define BOOT_BUILD_TAG                  "RELEASE"

    /* 日志：全部编译掉 */
    #define BOOT_LOG_LEVEL                  BOOT_LOG_NONE

    /* DET：5 个模块一起关（各模块的 xxx_DEV_ERROR_DETECT 会继承这个值） */
    #define BSW_DEV_ERROR_DETECT            STD_OFF

    /* 调试追踪模块整体编译掉 */
    #define BOOT_TRACE_ENABLE               STD_OFF
    #define BOOT_TRACE_SHOW_KEY             STD_OFF
    #define BOOT_TRACE_SHOW_FRAMES          STD_OFF

    /* 调试用后门 / 宽松超时 */
    #define DCM_ENABLE_TRUST_APP            STD_OFF
    #define CANTP_MANUAL_TEST_MODE          STD_OFF
    #define DCM_MANUAL_TEST_MODE            STD_OFF

    /* 上电不等诊断仪，直接跳 App（启动最快） */
    #define BOOT_HOLD_WINDOW_MS             (1000U)  /* 见下方"为什么不能设成 0" */

    /*------------------------------------------------------------------------------------------
     * 【为什么发布版的启动窗口不能设成 0 —— 2026-10-25 实测踩到】
     *
     *  曾经把发布版的 BOOT_HOLD_WINDOW_MS 设成 0，理由是"上电直接跳 App，启动最快"。
     *  烧进去之后现象是：
     *      - 上位机 --wait 连不上
     *      - 断电重上电还是连不上
     *      - 看起来"Bootloader 没了"
     *
     *  原因：App 校验通过时，EcuM_Init 会调 EcuM_WaitBootHoldWindow()，
     *        窗口为 0 时它立刻返回 FALSE -> 直接 BootApp_Jump()。
     *        结果开机 0 毫秒就跳走，**Bootloader 的 UDS 从来没机会响应**。
     *
     *  也就是说：这个窗口是"软件进入 Bootloader"的唯一入口。
     *  设成 0 = 一旦 App 刷坏或需要重刷，只能上 J-Link 救砖。
     *
     *  【正确做法（产品级）】
     *  让 App 能主动请求进 Bootloader：
     *      ① App 收到 10 02（编程会话）或某个自定义例程时，
     *         往参数区的 BootRequest 字段写 1，然后触发复位；
     *      ② Bootloader 启动时先看这个字段：置位 -> 清掉并留在 Bootloader；
     *         未置位 -> 直接跳 App（完全不需要窗口）。
     *  这样既能"上电秒跳"，又永远刷得回来。
     *  本工程目前只有窗口这一条路，所以必须留 1 秒。
     *  等 App 侧支持了请求标志，再把这里改成 0。
     *----------------------------------------------------------------------------------------*/

#else  /* 开发版 */

    #define BOOT_RELEASE_MODE               (0)
    #define BOOT_BUILD_TAG                  "DEBUG"

    #define BOOT_LOG_LEVEL                  BOOT_LOG_VERBOSE
    #define BSW_DEV_ERROR_DETECT            STD_ON

    #define BOOT_TRACE_ENABLE               STD_ON
    #define BOOT_TRACE_SHOW_KEY             STD_ON
    /* 每一帧原始数据默认关：刷 31KB 会打 5000 行 / 350KB，串口软件直接崩 */
    #define BOOT_TRACE_SHOW_FRAMES          STD_OFF

    #define DCM_ENABLE_TRUST_APP            STD_ON
    #define CANTP_MANUAL_TEST_MODE          STD_ON
    #define DCM_MANUAL_TEST_MODE            STD_ON

    /* 1500 ms 启动窗口：配上位机的 --wait，想刷就刷 */
    #define BOOT_HOLD_WINDOW_MS             (1500U)

#endif

/*--------------------------------------------------------------------------------------------------
 *                        日志总闸（让打印代码从固件里彻底消失）
 *
 *  【为什么要单独一个宏】
 *  光把调用点用 #if 包起来只能省掉"字符串常量"，
 *  Uart_Printf 的函数体（格式化引擎，约 2~3 KB）还会留在固件里 ——
 *  因为本工程没开 -ffunction-sections/--gc-sections。
 *  所以 uart.c 里那些函数的**定义**也要用这个宏包起来。
 *------------------------------------------------------------------------------------------------*/
#if (BOOT_LOG_LEVEL == BOOT_LOG_NONE)
    #define UART_PRINTING_DISABLED          STD_ON
#else
    #define UART_PRINTING_DISABLED          STD_OFF
#endif

#endif /* BUILD_CFG_H */
