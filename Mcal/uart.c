/*==================================================================================================
 *  Mcal/uart.c —— 调试串口封装实现
 *
 *  【本文件的三个关键实现选择，都在注释里说明了理由】
 *  1. 初始化用 RTD 的 Lpuart_Uart_Ip_Init（波特率/引脚/时钟配置由 CT 生成）
 *  2. **发送自己轮询寄存器**（TDRE → 写 → 等 TC），不用 Lpuart_Uart_Ip_SyncSend
 *     —— 因为它在本项目配置下会截断数据、且吃掉每行末尾的 '\n'
 *     （详细分析见 Uart_SendBytes 上方）
 *  3. 自己写精简 printf，不用 newlib 的 vsnprintf（省约 10 KB Flash）
 *================================================================================================*/

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "uart.h"
#include "Det.h"

#include <stdarg.h>             /* va_list / va_start / va_arg / va_end */

/* RTD 的 LPUART IP 层驱动 */
#include "Lpuart_Uart_Ip.h"

/* CT 生成的配置对象：Lpuart_Uart_Ip_xHwConfigPB_6 */
#include "Lpuart_Uart_Ip_Sa_PBcfg.h"

/*==================================================================================================
 *                                   LOCAL DEFINES
 *================================================================================================*/
#if (UART_DEV_ERROR_DETECT == STD_ON)
    /**
     * @brief Det 上报的便捷宏
     * @details ① 少打字
     *          ② DEV_ERROR_DETECT = STD_OFF 时整个调用被预处理器删成 ((void)0)
     *          ③ 统一 InstanceId
     */
    #define UART_DET_ERROR(api, err)                                    \
        ((void)Det_ReportError((uint16)UART_MODULE_ID,                  \
                               (uint8)UART_INSTANCE_ID,                 \
                               (uint8)(api),                            \
                               (uint8)(err)))
#else
    #define UART_DET_ERROR(api, err)    ((void)0)
#endif

/*==================================================================================================
 *                                   LOCAL VARIABLES
 *================================================================================================*/
/**
 * @brief 初始化标志
 * @details 用来挡住"未初始化就发送"和"重复初始化"两种情况。
 *          重复初始化会触发 RTD 内部的 DEV_ASSERT 死循环（见 Lpuart_Uart_Ip.c:270）。
 */
static boolean Uart_Initialized = FALSE;

/*==================================================================================================
 *                             强制提供的 LPUART6 回调（空实现）
 *
 *  【为什么必须有这个函数】
 *  CT 生成的配置里写死了回调名：
 *      const Lpuart_Uart_Ip_UserConfigType Lpuart_Uart_Ip_xHwConfigPB_6 =
 *      {
 *          ...
 *          Lpuart6_Callback,      ← 这个符号必须存在，否则**链接失败**
 *          NULL_PTR,
 *          ...
 *      };
 *  所以即使我们不用异步，也必须提供这个符号。
 *
 *  【为什么实现成空的】
 *  1. 我们只做轮询发送（直接操作寄存器），异步收发路径完全不走
 *  2. 驱动在 IRQ 里调用它之前会判空（Lpuart_Uart_Ip.c:1253 / 1266），
 *     所以即使是 NULL_PTR 也安全 —— 但提供空函数更明确
 *  3. IntCtrl 里根本没配 LPUART 中断，中断不会来
 *
 *  【一个重要的副作用要记住】
 *  这个函数运行在**中断上下文**（如果真有中断）。所以里面**绝对不能**打印、
 *  不能阻塞、不能调 Uart_SendBytes —— 否则会死锁或破坏时序。
 *  现在它是空的，没有风险。
 *================================================================================================*/
void Lpuart6_Callback(const uint8 HwInstance,
                      const Lpuart_Uart_Ip_EventType Event,
                      const void* UserData)
{
    (void)HwInstance;
    (void)Event;
    (void)UserData;
    /* 调试用：可以在这里放一个 g_uartEventCount++ 计数器，
     * 用来确认"异步事件到底有没有发生"。现在不需要。 */
}
#if (UART_PRINTING_DISABLED == STD_OFF)

/*==================================================================================================
 *                                   LOCAL HELPERS
 *================================================================================================*/

/*==================================================================================================
 *  说明：原来这里有一个 Uart_CalcTimeout()，用来按字节数算 SyncSend 的超时。
 *  改用"直接轮询寄存器"后不再需要它 —— 见 Uart_SendBytes 上方对两个 RTD bug 的分析。
 *================================================================================================*/

/**
 * @brief 往缓冲区追加一个字符（带边界检查，超出就丢弃）
 * @return 新的写入位置
 */
static uint32 Uart_Emit(char* Buf, uint32 Pos, uint32 Cap, char Ch)
{
    if (Pos < Cap)
    {
        Buf[Pos] = Ch;
        return (Pos + 1U);
    }
    return Pos;     /* 截断：满了就不再写，绝不越界 */
}
#endif /* UART_PRINTING_DISABLED */

#if (UART_PRINTING_DISABLED == STD_OFF)
/**
 * @brief 把无符号整数按指定进制追加到缓冲区
 * @param PadChar 补位字符（'0' 用于 %02x）
 * @param IsUpper 十六进制字母用大写还是小写
 *
 * 【为什么大小写要单独一个参数】
 * 补位字符和字母大小写是两件**正交**的事：
 *      %02x  → 补 '0'，字母小写
 *      %02X  → 补 '0'，字母大写
 *      %2X   → 补空格，字母大写
 * 如果把两者绑在一起（"PadChar=='0' 就大写"），%02x 会被错误地输出成大写。
 */
static uint32 Uart_EmitUnsigned(char* Buf, uint32 Pos, uint32 Cap,
                                uint32 Value, uint32 Base,
                                uint32 MinWidth, char PadChar,
                                boolean IsUpper)
{
    char     tmp[12];       /* 足够放下 32 位二进制的 32 位 + 余量 */
    uint32   n = 0U;
    uint32   i;

    if (Value == 0U)
    {
        tmp[n] = '0';
        n++;
    }
    else
    {
        while ((Value > 0U) && (n < (uint32)sizeof(tmp)))
        {
            uint32 digit = Value % Base;

            if (digit < 10U)
            {
                tmp[n] = (char)('0' + digit);
            }
            else
            {
                char baseChar = (IsUpper != FALSE) ? 'A' : 'a';
                tmp[n] = (char)(baseChar + (digit - 10U));
            }
            n++;
            Value /= Base;
        }
    }

    /* 前导补位 */
    for (i = n; i < MinWidth; i++)
    {
        Pos = Uart_Emit(Buf, Pos, Cap, PadChar);
    }

    /* 逆序输出（数字是从低位算起的） */
    while (n > 0U)
    {
        n--;
        Pos = Uart_Emit(Buf, Pos, Cap, tmp[n]);
    }

    return Pos;
}
#endif /* UART_PRINTING_DISABLED */

/*==================================================================================================
 *                                    Uart_Init
 *================================================================================================*/
void Uart_Init(void)
{
    if (Uart_Initialized != FALSE)
    {
        /* 已经初始化过，直接返回。
         * 不能重复调 Lpuart_Uart_Ip_Init —— 它里面有 DEV_ASSERT 会死循环。 */
        return;
    }

    /* 初始化 LPUART6 硬件。
     *
     * 前提条件（调用方保证）：
     *    Clock_Ip_Init()          → 提供 LPUART6 时钟（80 MHz）
     *    Siul2_Port_Ip_Init()     → 把 PTA15/PTA16 复用成 LPUART6 的 RX/TX
     *
     * 参数：
     *    UART_INSTANCE   = 6    （**物理 LPUART 编号**，见 uart.h 的详细说明）
     *    UART_HW_CONFIG  = &Lpuart_Uart_Ip_xHwConfigPB_6
     *
     * ⚠️ 这两个参数必须指向同一个外设（都是 LPUART6）：
     *       Instance = 6      → apBases[6] = IP_LPUART_6
     *       ...PB_6           → CT 给 LPUART6 生成的配置
     *    如果写成 Instance = 0，驱动会去操作 LPUART_0，
     *    而时钟和引脚都是配给 LPUART6 的 —— 结果串口毫无输出，且不报任何错。
     */
    Lpuart_Uart_Ip_Init((uint8)UART_INSTANCE, UART_HW_CONFIG);

    Uart_Initialized = TRUE;
}

/*==================================================================================================
 *                                  Uart_SendBytes
 *
 *  【★ 为什么不用 RTD 的 Lpuart_Uart_Ip_SyncSend —— 实测踩到两个坑】
 *
 *  坑 1：超时是"假的"，导致每次发送中途被截断
 *  ─────────────────────────────────────────────────────────────
 *    CT 里把 LPUART_UART_IP_TIMEOUT_TYPE 配成了 OSIF_COUNTER_DUMMY，
 *    而 DUMMY 计数器的实现是（RTD/src/OsIf_Timer.c:252-278）：
 *        OsIf_Timer_Dummy_GetCounter()          → return 0U;
 *        OsIf_Timer_Dummy_GetElapsed(...)       → return 1U;    ← 每次检查只加 1
 *        OsIf_Timer_Dummy_MicrosToTicks(Micros) → return Micros; ← 把 µs 当"次数"
 *
 *    SyncSendData 的内层循环是空转等 TDRE：
 *        while (!TDRE && !CheckTimeout(...)) {}
 *    160 MHz 下等 1 个字节（87 µs）要空转约 700 次，而 ElapsedTicks 每次只 +1。
 *    于是"按微秒算出的超时预算"实际只够发几个字节，循环提前退出 → 数据被截断。
 *    实测现象：40 个 '=' 只发出 18 个。
 *
 *  坑 2：发完立刻关发射器，吃掉最后一个字节
 *  ─────────────────────────────────────────────────────────────
 *        Lpuart_Uart_Ip_SyncSendData(...);              ← 只等到 TDRE（数据寄存器空）
 *        Lpuart_Uart_Ip_SetTransmitterCmd(Base, FALSE); ← 立刻关 TE
 *    此时最后一个字节还在**移位寄存器**里，会随 TE=0 一起被丢弃。
 *    而每个 Uart_Printf 的结尾都是 "\r\n" —— 丢的正好是 '\n'，
 *    结果只剩 '\r'：光标回到行首但不换行，**所有行挤在一起**。
 *
 *  【解法】自己按"TDRE → 写 → 最后等 TC"的顺序轮询寄存器
 *  ① 逐字节等 TDRE 再写，不会覆盖未发出的数据
 *  ② 发完等 TC（Transmission Complete，移位寄存器真空），保证最后一个字节真的出线
 *  ③ 发射器全程保持打开（TE=1），不来回切换
 *  ④ 超时保护用纯循环计数，不依赖任何定时器/OsIf
 *================================================================================================*/
Std_ReturnType Uart_SendBytes(const uint8* Data, uint32 Length)
{
    LPUART_Type*   base = IP_LPUART_6;
    uint32         i;
    uint32         spin;
    Std_ReturnType result = E_OK;

    /*--------------------------------------------------------------------------
     * 参数检查
     *------------------------------------------------------------------------*/
    if (Data == NULL_PTR)
    {
        UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    if (Length == 0U)
    {
        UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    if (Uart_Initialized == FALSE)
    {
        UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_UNINIT);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 确保发射器是开着的（TE = 1）
     *
     * RTD 的 Init 不会打开 TE，而 SyncSend 会在结束时把 TE 关掉。
     * 我们自己管理，避免来回切换导致时序问题。
     *------------------------------------------------------------------------*/
    base->CTRL = (base->CTRL & ~LPUART_CTRL_TE_MASK) | LPUART_CTRL_TE_MASK;

    /*--------------------------------------------------------------------------
     * 逐字节发送
     *
     * 每写一个字节前先等 TDRE（数据寄存器空），
     * 表示上一个字节已经从 DATA 搬进移位寄存器，可以写下一个了。
     *------------------------------------------------------------------------*/
    for (i = 0U; i < Length; i++)
    {
        spin = 0U;
        while (0U == (base->STAT & LPUART_STAT_TDRE_MASK))
        {
            spin++;
            if (spin > UART_TX_SPIN_LIMIT)
            {
                UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_TIMEOUT);
                return E_NOT_OK;
            }
        }

        /* 写数据寄存器（8 位模式，直接写低字节） */
        *(volatile uint8*)(&(base->DATA)) = Data[i];
    }

    /*--------------------------------------------------------------------------
     * 等移位寄存器也发完（TC）—— 这一步是整个函数的重点
     *
     * 不等 TC 就返回，调用方紧接着操作外设（或像 SyncSend 那样关 TE），
     * 会把最后一个字节吃掉。实测丢的就是每行结尾的 '\n'。
     *------------------------------------------------------------------------*/
    spin = 0U;
    while (0U == (base->STAT & LPUART_STAT_TC_MASK))
    {
        spin++;
        if (spin > UART_TX_SPIN_LIMIT)
        {
            UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_TIMEOUT);
            result = E_NOT_OK;
            break;
        }
    }

    return result;
}
#if (UART_PRINTING_DISABLED == STD_OFF)

/*==================================================================================================
 *                                  Uart_SendString
 *================================================================================================*/
void Uart_SendString(const char* Str)
{
    uint32 len = 0U;

    if (Str == NULL_PTR)
    {
        UART_DET_ERROR(UART_API_SEND_BYTES, UART_E_PARAM_POINTER);
        return;
    }

    /* 自己数长度，避免依赖 libc 的 strlen（省几个字节，也少一个外部依赖） */
    while (Str[len] != '\0')
    {
        len++;
    }

    if (len > 0U)
    {
        (void)Uart_SendBytes((const uint8*)Str, len);
    }
}
#endif /* UART_PRINTING_DISABLED */
#if (UART_PRINTING_DISABLED == STD_OFF)

/*==================================================================================================
 *                                   Uart_Printf
 *================================================================================================*/
void Uart_Printf(const char* Fmt, ...)
{
    char        buf[UART_PRINTF_BUF_SIZE];
    uint32      pos = 0U;
    const char* p   = Fmt;
    va_list     ap;

    if (Fmt == NULL_PTR)
    {
        UART_DET_ERROR(UART_API_PRINTF, UART_E_PARAM_POINTER);
        return;
    }

    va_start(ap, Fmt);

    while ((*p != '\0') && (pos < (UART_PRINTF_BUF_SIZE - 1U)))
    {
        if (*p != '%')
        {
            /* 普通字符直接搬。
             * 顺带处理 '\n' → "\r\n"：
             * Windows 串口助手普遍需要回车+换行，只发 '\n' 会只换行不回车，
             * 表现为"文字全部从同一列末尾开始"。
             * 这样调用方写 "\n" 就够了，不用到处写 "\r\n"。 */
            if (*p == '\n')
            {
                pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, '\r');
            }
            pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, *p);
            p++;
            continue;
        }

        p++;    /* 跳过 '%' */

        /*--------------------------------------------------------------------------
         * 解析可选的补位与宽度：%[-][0][width][duxXsc]
         *
         * 【为什么要"吃掉"一个 '-'】
         * 本函数是精简版，只支持 '0' 补位 + 十进制宽度，**不支持左对齐**。
         * 但如果调用方误写了 "%-4s"，以前会被当成普通字符继续解析，
         * 导致后续的 %u / %02X 全部错位 —— 打印出一堆乱数字，
         * 而且不看源码根本发现不了（实测就被坑过一次，浪费了一轮上板）。
         *
         * 现在允许并忽略左对齐标记（-）：格式串不再错位，
         * 输出变成"不补位"，虽然不对齐但数字是**对的**。
         *------------------------------------------------------------------------*/
        {
            char     padChar  = ' ';
            uint32   minWidth = 0U;

            /* 忽略左对齐标记（不支持，但必须吃掉，否则整串错位） */
            if (*p == '-')
            {
                p++;
            }

            if (*p == '0')
            {
                padChar = '0';
                p++;
            }

            while ((*p >= '0') && (*p <= '9'))
            {
                minWidth = (minWidth * 10U) + (uint32)(*p - '0');
                p++;
            }

            /*--------------------------------------------------------------------------
             * 按格式符分派
             *------------------------------------------------------------------------*/
            switch (*p)
            {
                case 's':
                {
                    const char* s = va_arg(ap, const char*);
                    if (s == NULL_PTR)
                    {
                        s = "(null)";
                    }
                    while ((*s != '\0') && (pos < (UART_PRINTF_BUF_SIZE - 1U)))
                    {
                        pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, *s);
                        s++;
                    }
                    p++;
                    break;
                }

                case 'c':
                {
                    char c = (char)va_arg(ap, int);     /* char 会被提升成 int */
                    pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, c);
                    p++;
                    break;
                }

                case 'd':
                {
                    sint32 v = va_arg(ap, sint32);
                    if (v < 0)
                    {
                        pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, '-');
                        /* 这样写避开 INT32_MIN 取负溢出的边界情况 */
                        pos = Uart_EmitUnsigned(buf, pos, UART_PRINTF_BUF_SIZE - 1U,
                                                (uint32)(-(v + 1)) + 1U,
                                                10U, minWidth, '0', FALSE);
                    }
                    else
                    {
                        pos = Uart_EmitUnsigned(buf, pos, UART_PRINTF_BUF_SIZE - 1U,
                                                (uint32)v, 10U, minWidth, '0', FALSE);
                    }
                    p++;
                    break;
                }

                case 'u':
                {
                    uint32 v = va_arg(ap, uint32);
                    pos = Uart_EmitUnsigned(buf, pos, UART_PRINTF_BUF_SIZE - 1U,
                                            v, 10U, minWidth, '0', FALSE);
                    p++;
                    break;
                }

                case 'x':
                case 'X':
                {
                    uint32 v = va_arg(ap, uint32);
                    /* 补位字符与字母大小写是两件正交的事，分别传：
                     *   %02x → 补 '0' + 小写
                     *   %02X → 补 '0' + 大写
                     *   %2X  → 补空格 + 大写 */
                    pos = Uart_EmitUnsigned(buf, pos, UART_PRINTF_BUF_SIZE - 1U,
                                            v, 16U, minWidth, padChar,
                                            (boolean)((*p == 'X') ? TRUE : FALSE));
                    p++;
                    break;
                }

                case '%':
                {
                    pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, '%');
                    p++;
                    break;
                }

                default:
                {
                    /* 不认识的格式符：原样输出 '%' 和该字符，方便发现写错 */
                    pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, '%');
                    if (*p != '\0')
                    {
                        pos = Uart_Emit(buf, pos, UART_PRINTF_BUF_SIZE - 1U, *p);
                        p++;
                    }
                    break;
                }
            }
        }
    }

    va_end(ap);

    if (pos > 0U)
    {
        (void)Uart_SendBytes((const uint8*)buf, pos);
    }
}
#endif /* UART_PRINTING_DISABLED */
