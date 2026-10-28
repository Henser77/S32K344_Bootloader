/*==================================================================================================
 *  src/boot_trace.c —— 调试上报实现
 *
 *  【两条数据来源】
 *  ① CanIf 的接收观察回调 —— 在**中断上下文**被调用，所以这里只做拷贝，
 *     把帧塞进环形缓冲，真正的打印留给主循环 BootTrace_MainFunction()。
 *  ② PduR 的请求通知 —— 在主循环里被调用，可以安全阻塞打印。
 *
 *  【全部输出受 BOOT_TRACE_ENABLE 控制】
 *  关掉后本文件编译出来几乎是空的，调用点也什么都不做。
 *================================================================================================*/

#include "boot_trace.h"

#if (BOOT_TRACE_ENABLE == STD_ON)

#include "uart.h"
#include "canif.h"
#include "cantp.h"
#include "dcm.h"
#include "fls.h"

/*==================================================================================================
 *                                      本地常量与类型
 *================================================================================================*/

/** @brief 原始帧环形缓冲深度（够看一屏就够了，满了就丢并计数） */
#define TRACE_RX_RING_SIZE      (8U)

/** @brief 一帧 CAN 报文（从 ISR 传到主循环的最小信息集） */
typedef struct
{
    uint32 id;
    uint8  length;
    uint8  data[8];
} Trace_RxFrameType;

/*==================================================================================================
 *                                      本地变量
 *================================================================================================*/

static Trace_RxFrameType g_rxRing[TRACE_RX_RING_SIZE];

/* 这两个变量跨中断/主循环访问，必须 volatile ——
 * 否则编译器可能把主循环里"反复读 g_rxCount"优化成只读一次，导致死循环。 */
static volatile uint32 g_rxHead;
static volatile uint32 g_rxTail;
static volatile uint32 g_rxCount;
static volatile uint32 g_rxDropped;

/** @brief 本轮传输已经收了多少个 0x36 块（0x34 时清零，用于每 N 块打一行进度） */
static uint32 s_xferBlocks = 0U;

/*==================================================================================================
 *                                      内部辅助
 *================================================================================================*/

#if (BOOT_TRACE_SHOW_FRAMES == STD_ON)
/** @brief 打印一帧 CAN 报文 */
static void Trace_PrintFrame(const Trace_RxFrameType* Frame)
{
    uint32 i;

    Uart_Printf("接收 0x%03X [%u 字节]", Frame->id & 0x1FFFFFFFU, (uint32)Frame->length);

    for (i = 0U; i < Frame->length; i++)
    {
        Uart_Printf(" %02X", (uint32)Frame->data[i]);
    }

    Uart_Printf("\n");
}
#else
/* 帧打印被关掉时，这个函数不参与编译 —— 省 Flash，也避免"定义了没用"的警告 */
#endif

/** @brief 从环形缓冲取一帧 */
static boolean Trace_RxPop(Trace_RxFrameType* Out)
{
    uint32 idx;
    uint32 i;

    if (g_rxCount == 0U)
    {
        return FALSE;
    }

    idx = g_rxTail;

    Out->id     = g_rxRing[idx].id;
    Out->length = g_rxRing[idx].length;
    for (i = 0U; i < 8U; i++)
    {
        Out->data[i] = g_rxRing[idx].data[i];
    }

    g_rxTail = (idx + 1U) % TRACE_RX_RING_SIZE;
    g_rxCount--;

    return TRUE;
}

/**
 * @brief CanIf 的接收观察回调 —— 运行在**中断上下文**
 *
 * @details 【三条硬约束】
 *          ① 不能打印：Uart_Printf 会阻塞几毫秒，在 ISR 里会把系统拖死
 *          ② 不能长时间占用：只拷 8 字节，几十个指令周期就返回
 *          ③ SduDataPtr 指向的缓冲本次调用后会被下一帧覆盖，必须立刻拷走
 */
static void Trace_OnCanIfRx(PduIdType RxPduId, const PduInfoType* PduInfoPtr)
{
    uint32 idx;
    uint32 i;
    uint8  len;

    (void)RxPduId;

    if ((PduInfoPtr == NULL_PTR) || (PduInfoPtr->SduDataPtr == NULL_PTR))
    {
        return;
    }

    /* 缓冲满了就丢弃并计数。这里**不报 Det** ——
     * "缓冲满"属于高负载下的正常现象，不是程序员的错误。 */
    if (g_rxCount >= TRACE_RX_RING_SIZE)
    {
        g_rxDropped++;
        return;
    }

    idx = g_rxHead;

    len = (PduInfoPtr->SduLength > 8U) ? 8U : (uint8)PduInfoPtr->SduLength;

    /* CanIf 给的是 PduInfoType（不带 CAN ID），而打印时想看到 ID，
     * 所以这里填上配置里的诊断请求 ID。 */
    g_rxRing[idx].id     = CANIF_RX_CAN_ID_UDS;
    g_rxRing[idx].length = len;

    for (i = 0U; i < 8U; i++)
    {
        g_rxRing[idx].data[i] = (i < len) ? PduInfoPtr->SduDataPtr[i] : 0U;
    }

    g_rxHead = (idx + 1U) % TRACE_RX_RING_SIZE;
    g_rxCount++;
}

/** @brief 把 CanTp 的中止原因翻译成人话 */
static const char* BootTrace_CanTpAbortName(CanTp_AbortReasonType Reason)
{
    switch (Reason)
    {
        case CANTP_ABORT_RX_TIMEOUT:
            return "接收超时 N_Cr（对端不再发连续帧了）";

        case CANTP_ABORT_TX_TIMEOUT:
            return "发送超时 N_Bs（对端一直没回应首帧）";

        case CANTP_ABORT_RX_SEQ:
            return "序号错误（连续帧丢了，或顺序乱了）";

        case CANTP_ABORT_TX_OVFLW:
            return "发送溢出（对端说它的缓冲太小）";

        case CANTP_ABORT_RX_OVFLW:
            return "接收溢出（消息比我们的缓冲大，或丢了消息）";

        default:
            return "无";
    }
}

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

void BootTrace_Init(void)
{
    uint32 i;

    g_rxHead    = 0U;
    g_rxTail    = 0U;
    g_rxCount   = 0U;
    g_rxDropped = 0U;

    for (i = 0U; i < TRACE_RX_RING_SIZE; i++)
    {
        g_rxRing[i].id     = 0U;
        g_rxRing[i].length = 0U;
    }

    /* 注册接收观察回调 —— CanIf 在把帧交给 PduR 之后会再通知一次 */
    CanIf_RegisterRxIndicationHook(Trace_OnCanIfRx);
}

void BootTrace_OnUdsRequest(const uint8* Req, uint32 ReqLen)
{
    uint32 i;

    if ((Req == NULL_PTR) || (ReqLen == 0U))
    {
        return;
    }

    /*------------------------------------------------------------------
     * 0x36 传输数据：单独处理，不能逐块刷屏
     *
     * 【为什么】
     *  一次刷写 125 块。如果每块都走下面的完整打印（请求行 + 会话/安全行），
     *  光这一阶段就 375 行 / 约 35 KB。再加上帧打印，串口会被打爆；
     *  而 Uart_Printf 是阻塞的 —— 会直接拖慢 CanTp，导致丢帧。
     *
     *  现在只打印：第 1 块（确认传输开始）+ 每 16 块一行进度。
     *  125 块一共 8 行。
     *------------------------------------------------------------------*/
    if (Req[0] == DCM_SID_TRANSFER_DATA)
    {
        s_xferBlocks++;

        if ((s_xferBlocks == 1U) ||
            ((s_xferBlocks % BOOT_TRACE_XFER_PROGRESS_EVERY) == 0U))
        {
            Uart_Printf("[刷写] 第 %u 块（每块 %u 字节）\n", s_xferBlocks, ReqLen);
        }

        return;         /* 不走下面的完整打印 */
    }

    /* 0x34 请求下载：新一轮传输开始，块计数清零 */
    if (Req[0] == DCM_SID_REQUEST_DOWNLOAD)
    {
        s_xferBlocks = 0U;
    }

    Uart_Printf("\n[UDS] 收到请求：服务 0x%02X，长度 %u\n", (uint32)Req[0], ReqLen);

    /* 请求都很短（不超过 20 字节），完整打出来便于对照 */
    if (ReqLen <= 32U)
    {
        Uart_Printf("      ");
        for (i = 0U; i < ReqLen; i++)
        {
            Uart_Printf("%02X ", (uint32)Req[i]);
        }
        Uart_Printf("\n");
    }

    /* ⚠️ 这里的标签必须和 dcm.h 里的枚举严格对应：
     *      session : 0=DEFAULT  1=PROGRAMMING  2=EXTENDED
     *      security: 0=LOCKED   1=SEED_SENT    2=UNLOCKED
     *    早期版本把 security 的 1 写成了"已解锁"，是错的 ——
     *    1 只是"种子已发出、还在等密钥"，2 才是真正解锁。 */
    Uart_Printf("      会话=%u 安全=%u（会话: 0=默认 1=编程 2=扩展 | 安全: 0=锁定 1=已发种子 2=已解锁）\n",
                (uint32)Dcm_GetSession(), (uint32)Dcm_GetSecurity());

    /*------------------------------------------------------------------
     * 擦除例程（0x31 01 xx）之后的耗时与扇区统计
     * 这是"真的擦了 1MB"的客观证据
     *------------------------------------------------------------------*/
    if ((Req[0] == DCM_SID_ROUTINE_CONTROL) && (ReqLen >= 2U) && (Req[1] == 0x01U))
    {
        Uart_Printf("      [调试] 0x31 用时 %u 毫秒，擦除 %u 个扇区（%u..%u），轮询 %u 次，重擦 %u 次，最慢单扇区等待 %u 毫秒\n",
                    Dcm_GetLastEraseMs(),
                    g_FlsLastEraseCount,
                    g_FlsLastEraseStartSector,
                    g_FlsLastEraseEndSector,
                    g_FlsTotalEraseSpin,
                    g_FlsEraseRetryCount,
                    g_FlsMaxWaitMs);
    }

    /*------------------------------------------------------------------
     * Fls 层失败细节（诊断仪的响应里只有一个成功/失败字节，
     * 真正的原因要在这里看）
     *------------------------------------------------------------------*/
    if ((Req[0] == DCM_SID_ROUTINE_CONTROL) &&
        (Dcm_GetLastFlsStatus() != 0U) &&
        (Dcm_GetLastFlsStatus() != 0xEEU))
    {
        Uart_Printf("      [调试] Flash 失败：状态码 0x%02X，卡在扇区 %u\n",
                    (uint32)Dcm_GetLastFlsStatus(),
                    Dcm_GetLastFlsSector());
    }

    /*------------------------------------------------------------------
     * 0x27 请求种子之后，把"该回什么密钥"打出来
     *
     * ⚠️ 正式产品必须删掉这一段（等于把解锁方法公开），
     *    所以它也在 BOOT_TRACE_ENABLE 开关的管辖范围内。
     *------------------------------------------------------------------*/
#if (BOOT_TRACE_SHOW_KEY == STD_ON)
    if ((Req[0] == DCM_SID_SECURITY_ACCESS) &&
        (ReqLen >= 2U) &&
        ((Req[1] & 0x7FU) == DCM_SEC_LEVEL1_SEED_SUB))
    {
        uint32 key = Dcm_GetDebugExpectedKey();

        Uart_Printf("      [调试] 种子=0x%08X，应回密钥=0x%08X\n", Dcm_GetLastSeed(), key);

        /* 直接把"下一帧该发什么"打成一条完整 8 字节帧，
         * 手工用 USB-CAN 工具时照抄即可，不用自己算字节序。 */
        Uart_Printf("      [调试] >> 请发送这一帧: 06 27 02 %02X %02X %02X %02X CC\n",
                    (uint32)((key >> 24) & 0xFFU),
                    (uint32)((key >> 16) & 0xFFU),
                    (uint32)((key >> 8)  & 0xFFU),
                    (uint32)(key & 0xFFU));
    }
#endif /* BOOT_TRACE_SHOW_KEY */
}

void BootTrace_MainFunction(void)
{
    Trace_RxFrameType frame;

    /*------------------------------------------------------------------
     * ① S3 会话超时
     *
     * 超过 S3 时间没收到任何请求，会话会自动退回默认并锁定安全访问 ——
     * 这是标准要求（防止诊断仪拔线后 ECU 一直停在编程会话）。
     * 但如果日志里悄悄发生，看图的人会纳闷"我刚才明明切过扩展会话"。
     *------------------------------------------------------------------*/
    {
        static uint32 lastS3Count = 0U;
        uint32 nowS3 = Dcm_GetS3TimeoutCount();

        if (nowS3 != lastS3Count)
        {
            lastS3Count = nowS3;
            Uart_Printf("[Dcm] S3 超时：会话已退回默认，安全访问已重新锁定\n");
            Uart_Printf("      （定时发 01 3E 00 00 00 00 00 00 可以保持会话）\n");
        }
    }

    /*------------------------------------------------------------------
     * ② 传输被中止
     *
     * 必须打印得**能自己说明问题**：用文字说清是哪种中止，
     * 并附上"拼到多少字节 / 本来总共多少字节"。
     *------------------------------------------------------------------*/
    {
        static uint32 lastAbortCount = 0U;
        uint32 nowCount = CanTp_GetAbortCount();

        if (nowCount != lastAbortCount)
        {
            lastAbortCount = nowCount;

            Uart_Printf("[CanTp] 传输中止：%s\n", BootTrace_CanTpAbortName(CanTp_GetAbortReason()));

            if (CanTp_GetLastAbortRxTotal() != 0U)
            {
                Uart_Printf("        已收到 %u / %u 字节\n",
                            CanTp_GetLastAbortRxLen(),
                            CanTp_GetLastAbortRxTotal());
            }
        }
    }

    /*------------------------------------------------------------------
     * ③ 把原始帧打出来（确认 CanIf 到底收到了什么）
     *------------------------------------------------------------------*/
#if (BOOT_TRACE_SHOW_FRAMES == STD_ON)
    /* 每帧原始数据：只有在追单条命令时才打开（刷写时会打爆串口） */
    while (Trace_RxPop(&frame) != FALSE)
    {
        Trace_PrintFrame(&frame);
    }
#else
    /* 不打印，但**必须**把环形缓冲抽空 ——
     * 否则 ISR 那边会一直判"缓冲满"，g_rxDropped 越涨越多。 */
    while (Trace_RxPop(&frame) != FALSE)
    {
        /* 只丢弃 */
    }
#endif

    /*------------------------------------------------------------------
     * ④ 接收缓冲溢出提示（只在第一次丢弃时提示，避免刷屏）
     *------------------------------------------------------------------*/
    if (g_rxDropped > 0U)
    {
        Uart_Printf("[警告] 接收缓冲已满，丢弃了 %u 帧\n", g_rxDropped);
        g_rxDropped = 0U;
    }

    /*------------------------------------------------------------------
     * ⑤ CAN 收发计数（诊断"上位机发了但 ECU 不回"这类故障）
     *
     * 【怎么用它定位】
     *   Rx 不动、Tx 不动 → 板子根本没收到请求
     *                       问题在 PC/适配器/接线，不在固件
     *   Rx 在涨、Tx 不涨 → 板子收到了但发不出去
     *                       问题在发送路径（CanTp→PduR→CanIf→Can）
     *   Tx 在涨、上位机收不到 → 板子发了但对方没收到
     *                       问题在适配器接收侧或 CAN ID 不匹配
     *
     * 【为什么按调用次数分频而不是用定时器】
     *   主循环没有定时器依赖，按次数分频最简单。
     *   主循环约每秒几十万次，取 200000 大约 1 秒。
     *   数字没变化就不打印，避免刷屏。
     *------------------------------------------------------------------*/
    {
        static uint32 statDiv     = 0U;
        static uint32 lastRx      = 0U;
        static uint32 lastTx      = 0U;
        static uint32 lastBusy    = 0U;
        uint32 rx = 0U, rxF = 0U, tx = 0U, busy = 0U;

        statDiv++;
        if (statDiv >= 200000U)
        {
            statDiv = 0U;
            CanIf_GetStats(&rx, &rxF, &tx, &busy);

            if ((rx != lastRx) || (tx != lastTx) || (busy != lastBusy))
            {
                lastRx   = rx;
                lastTx   = tx;
                lastBusy = busy;
                Uart_Printf("[CAN] 收 %u 帧（过滤掉 %u）/ 发 %u 帧（失败 %u）\n",
                            rx, rxF, tx, busy);
            }
        }
    }
}

#else /* BOOT_TRACE_ENABLE == STD_OFF */

/*---------------- 关掉时编译成空函数，调用点什么都不做 ----------------*/

void BootTrace_Init(void)
{
}

void BootTrace_MainFunction(void)
{
}

void BootTrace_OnUdsRequest(const uint8* Req, uint32 ReqLen)
{
    (void)Req;
    (void)ReqLen;
}

#endif /* BOOT_TRACE_ENABLE */
