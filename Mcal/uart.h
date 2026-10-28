/*==================================================================================================
 *  Mcal/uart.h —— 调试串口封装（Bootloader 专用）
 *
 *  【这不是完整的 AUTOSAR Uart MCAL，请先看清它的定位】
 *
 *  AUTOSAR 标准里确实定义了 Uart MCAL（SWS_Uart），但 NXP 的 S32K3 RTD
 *  **没有实现标准 Uart MCAL**，而是提供了一个 CDD（Complex Device Driver）：
 *
 *      Uart_TS_T40D34M70I1R0/
 *          include/CDD_Uart.h      ← 模块号 255（厂商自定义，不是标准模块号）
 *          src/CDD_Uart.c
 *          src/Uart_Ipw.c
 *          src/Lpuart_Uart_Ip.c    ← IP 层（本工程用它做初始化）
 *
 *  层次关系（和 CAN / Flash 同构）：
 *      CDD_Uart  →  Uart_Ipw  →  Lpuart_Uart_Ip  →  LPUART6 硬件
 *      我们不写完整的 Uart，只写一个调试输出封装。
 *
 *  【为什么值得写这一层】
 *  1. 隔离 RTD 类型：Boot/ 和其他 Mcal 模块不必出现任何 Lpuart_Uart_Ip_* 类型
 *  2. 提供 Uart_Printf，让"打印调试"变成一行代码
 *  3. 如果将来换 MCU（LPUART → 别家的 UART），只改本文件
 *
 *  【为什么用轮询，而不是中断（AsyncSend）】
 *  1. Bootloader 要最小化：轮询不需要 ISR、不需要缓冲状态机
 *  2. 打印量很小（只在关键节点），阻塞几毫秒可以接受
 *  3. **IntCtrl 里本来就没配 LPUART 中断**（只配了 CAN0），用异步也来不了中断
 *
 *  【★ 但不能用 RTD 的 Lpuart_Uart_Ip_SyncSend —— 实测有两个坑】
 *
 *  坑 1：超时机制是"假的"，发送中途被截断
 *      CT 把 LPUART_UART_IP_TIMEOUT_TYPE 配成 OSIF_COUNTER_DUMMY，
 *      而 DUMMY 计数器的 GetElapsed() 恒返回 1（RTD/src/OsIf_Timer.c:252），
 *      于是"超时"实际是"检查次数上限"，不是微秒。
 *      等 1 个字节要空转约 700 次，而按 µs 算出的预算远小于此
 *      → 每次发送只能发出前一部分字节。
 *      实测：40 个 '=' 只出 18 个。
 *
 *  坑 2：发完立刻关发射器，吃掉最后一个字节
 *      SyncSend 结尾执行 SetTransmitterCmd(Base, FALSE)，
 *      而它前面只等到 TDRE（数据寄存器空），最后一个字节还在移位寄存器里。
 *      → 每次发送丢最后 1 字节。因为每行都以 "\r\n" 结尾，丢的正好是 '\n'，
 *        结果只剩 '\r'：光标回到行首但不换行，**所有行挤在一起**。
 *
 *  【所以本模块：Init 用 RTD，发送自己轮询寄存器】
 *      Uart_Init      → 仍调用 Lpuart_Uart_Ip_Init（波特率/引脚/时钟由 CT 生成）
 *      Uart_SendBytes → 自己按 "TDRE → 写 → 等 TC" 操作寄存器，行为完全确定
 *                       （实现与详细分析见 uart.c）
 *
 *  【阻塞时间参考】115200 baud / 8N1 = 11520 字节/秒，约 87 µs/字节
 *      4 字节  ≈ 0.35 ms
 *      22 字节 ≈ 1.9 ms
 *      50 字节 ≈ 4.3 ms
 *  所以**不要每收一帧就打印** —— 刷写时上万帧会把流程拖慢几十秒。
 *================================================================================================*/
#ifndef UART_H
#define UART_H

/* 构建模式 / 日志总闸 / DET 总开关 */
#include "build_cfg.h"

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "Std_Types.h"

/*==================================================================================================
 *                                    DEFINES AND MACROS
 *================================================================================================*/

/**
 * @brief 模块号
 * @details 用 255，与 NXP 的 CDD_Uart.h 保持一致。
 *          255 是"厂商自定义"区间，因为 UART 在 AUTOSAR 里没有标准 MCAL 实现。
 *          这样 Det 记录里的模块号不会和标准模块（Can=0x50、Com=0x32 等）撞车。
 */
#define UART_MODULE_ID              (255U)

/** @brief 单实例，实例号固定 0 */
#define UART_INSTANCE_ID            (0U)

/**
 * @brief 开发错误检测开关
 * @details 优先级：本模块单独定义 > 全局 BSW_DEV_ERROR_DETECT > 默认 STD_ON
 */
#ifndef UART_DEV_ERROR_DETECT
    #ifdef BSW_DEV_ERROR_DETECT
        #define UART_DEV_ERROR_DETECT   (BSW_DEV_ERROR_DETECT)
    #else
        #define UART_DEV_ERROR_DETECT   (STD_ON)
    #endif
#endif

/**
 * @brief LPUART 实例号 —— **物理编号**，不是"第几个被配置的"
 *
 * 【这里有两个不同的索引空间，很容易混，本人已踩过一次坑】
 *
 *  ① Instance —— 传给 Lpuart_Uart_Ip_Init 的那个参数
 *     ───────────────────────────────────────────────────────────
 *     它索引这个数组（Lpuart_Uart_Ip.c:169）：
 *         static LPUART_Type* const Lpuart_Uart_Ip_apBases[LPUART_INSTANCE_COUNT]
 *                                                 = IP_LPUART_BASE_PTRS;
 *     其中 LPUART_INSTANCE_COUNT = 16（S32K344 有 16 个 LPUART）
 *     IP_LPUART_BASE_PTRS = { IP_LPUART_0, IP_LPUART_1, ..., IP_LPUART_15 }
 *
 *     → **必须填物理 LPUART 编号：用 LPUART6 就填 6。**
 *
 *  ② Lpuart_Uart_Ip_apStateStructure[] 的下标
 *     ───────────────────────────────────────────────────────────
 *     这个数组大小是"**配置了几个**"：
 *         #define LPUART_UART_IP_NUMBER_OF_INSTANCES  (1U)    ← CT 只配了 1 个
 *     → 只有 [0] 一个元素，所以只有下标 0 合法。
 *
 *     CT 生成的配置对象里写死了：
 *         &Lpuart_Uart_Ip_apStateStructure[0U]        ← 这是②空间的下标
 *     它**不代表**①空间也该填 0。两个空间互不相干。
 *
 * 【★ 把 Instance 误写成 0 会发生什么 —— 本文件最值得记住的一课】
 *
 *     apBases[0] = IP_LPUART_0  →  驱动去配置 **LPUART_0 的寄存器**，
 *     而时钟（80 MHz 给 LPUART6）和引脚（PTA15/PTA16 复用成 LPUART6）
 *     都是给 **LPUART6** 配的。
 *
 *     结果：
 *         Lpuart_Uart_Ip_Init      返回"成功"
 *         发送函数                 返回"成功"
 *         串口上一个字节都没有
 *         Det 一条错误都不报
 *
 *     因为 0 在数组范围内（不越界），驱动只是默默地操作了另一个外设。
 *     **这就是典型的"静默失败"** —— 和本项目一直在对抗的那类问题同源：
 *     两份事实（实例号 vs 引脚/时钟配置）对不上，却没有任何机制检查它们。
 *
 *     （实测：填 0 串口无输出，填 6 正常打印。）
 */
#define UART_INSTANCE               (6U)

/** @brief 配置对象名（CT 生成，见 generate/src/Lpuart_Uart_Ip_Sa_PBcfg.c） */
#define UART_HW_CONFIG              (&Lpuart_Uart_Ip_xHwConfigPB_6)

/**
 * @brief 发送时的空转上限（每个字节 / 等 TC 各用一次）
 *
 * @details 我们不依赖任何定时器做超时（见文件头对 DUMMY 计数器的分析），
 *          而是用纯循环计数防止死等。
 *
 *          量级估算：160 MHz 下每次循环约 5 个周期 → 2,000,000 次 ≈ 62 ms。
 *          而 115200 baud 发 1 个字节只需 87 µs，等 TC 也只需 87 µs。
 *          所以这个上限有约 700 倍余量 —— 正常绝不会触发，
 *          只有在硬件真的卡死（比如引脚被占、时钟没开）时才会超时。
 */
#define UART_TX_SPIN_LIMIT          (2000000UL)

/** @brief Uart_Printf 的栈上缓冲大小（超出会截断，不越界） */
#define UART_PRINTF_BUF_SIZE        (128U)

/*==================================================================================================
 *                                 STRUCTURES AND OTHER TYPEDEFS
 *================================================================================================*/

/** @brief API 编号（Det 上报时用） */
typedef enum
{
    UART_API_INIT                   = 0U,
    UART_API_SEND_BYTES             = 1U,
    UART_API_PRINTF                 = 2U
} Uart_ApiIdType;

/** @brief 错误编号（Det 上报时用） */
typedef enum
{
    UART_E_PARAM_POINTER        = 0x01U,    /**< 传了 NULL 指针 */
    UART_E_UNINIT               = 0x02U,    /**< 未初始化就调用发送 */
    UART_E_PARAM_LENGTH         = 0x03U,    /**< 长度为 0 */
    UART_E_TIMEOUT              = 0x04U     /**< 发送超时 */
} Uart_ErrorIdType;

/*==================================================================================================
 *                                    FUNCTION PROTOTYPES
 *================================================================================================*/

/**
 * @brief 初始化调试串口
 * @details 内部完成两件事：
 *          1. 调用 Lpuart_Uart_Ip_Init 配置硬件（115200 / 8N1）
 *          2. 置内部"已初始化"标志
 *
 *          ⚠️ 只能调用一次。RTD 的 Init 里有 DEV_ASSERT 检查"之前是否已初始化"，
 *             重复调用会触发断言死循环。
 *
 *          ⚠️ 调用前必须已经完成 Clock_Ip_Init 和 Siul2_Port_Ip_Init ——
 *             这两个分别提供 LPUART6 的时钟和引脚复用。
 */
void Uart_Init(void);

/**
 * @brief 发送任意字节（阻塞，直接轮询 LPUART 寄存器）
 *
 * @details 内部流程：确保 TE=1 → 逐字节等 TDRE 后写入 → 最后等 TC 再返回。
 *          最后那一步是为了让"最后一个字节真的出线"，否则调用方紧接着
 *          关外设或改配置会把它吃掉（RTD 的 SyncSend 就栽在这里）。
 *
 * @param[in] Data   数据指针（不能为 NULL）
 * @param[in] Length 字节数（不能为 0）
 * @return E_OK 发送完成；E_NOT_OK 参数错误 / 未初始化 / 超时
 */
Std_ReturnType Uart_SendBytes(const uint8* Data, uint32 Length);

/**
 * @brief 发送以 '\0' 结尾的字符串（方便调试）
 * @param[in] Str 字符串指针（不能为 NULL）
 */
void Uart_SendString(const char* Str);

/**
 * @brief 格式化输出（精简版，不依赖 newlib 的 printf）
 *
 * @details 支持的格式符：
 *      %s          字符串
 *      %c          字符
 *      %d          有符号十进制
 *      %u          无符号十进制
 *      %x / %X     十六进制（小写 / 大写）
 *      %02x 等     带最小宽度的十六进制（不足补 0）
 *      %%          输出一个 '%'
 *
 *      另外：格式串里的 '\n' 会自动转成 "\r\n"，调用方不用手写 '\r'。
 *
 *  【为什么不用标准 vsnprintf】
 *  newlib 的 vsnprintf 会带进来约 10 KB 代码（格式化引擎 + 字符分类表）。
 *  对一个 64 KB 的 Bootloader 来说，为了打印调试信息占 15% 的 Flash 不划算。
 *  这个精简版只支持上面几种格式符，足够了。
 *
 * @param[in] Fmt 格式串
 */
void Uart_Printf(const char* Fmt, ...);

/*==================================================================================================
 *                        日志总闸：发布版让打印代码彻底消失
 *
 *  BOOT_LOG_LEVEL == BOOT_LOG_NONE 时，所有调用点被替换成 ((void)0)：
 *      ① 字符串常量不再进 Flash（现在这些打印语句约 8~10 KB）
 *      ② 参数表达式不再求值，连函数调用开销都没有
 *  uart.c 里 Uart_Printf / Uart_SendString 的**定义**也用同一个宏包起来，
 *  所以格式化引擎（约 2~3 KB）也不会被链接进来。
 *
 *  注意：Uart_Init / Uart_SendBytes 不受影响 ——
 *        将来如果 UART 要承载真正的通信协议，那两个仍然可用。
 *================================================================================================*/
#if (UART_PRINTING_DISABLED == STD_ON)
    #define Uart_Printf(...)                ((void)0)
    #define Uart_SendString(Str)            ((void)(Str))
#endif

#endif /* UART_H */
