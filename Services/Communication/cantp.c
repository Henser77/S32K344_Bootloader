/*==================================================================================================
 *  Mcal/cantp.c —— CanTp（ISO 15765-2 传输层）实现
 *
 *  【职责边界 —— 记住这一条就不会写乱】
 *  本模块**不懂 UDS**（那是 Dcm 的事），也**不懂 CAN 寄存器**（那是 can.c 的事）。
 *  它只做一件事：在"一条长消息"和"一堆 8 字节帧"之间来回转换。
 *
 *  【代码分三段，按执行上下文区分】
 *
 *    ① 中断上下文（由 CAN 驱动回调）
 *         CanTp_RxIndication    收帧 → 拼装
 *         CanTp_TxConfirmation  发完一帧 → 清忙标志
 *
 *    ② 主循环上下文
 *         CanTp_MainFunction    超时检查、回流控帧、逐帧发连续帧
 *         CanTp_Transmit        上层请求发一条消息
 *
 *    ③ 纯函数（无状态）
 *         STmin 解码、帧组装
 *
 *  【为什么拼装放在中断里】
 *  拼装只是把最多 7 字节拷贝进缓冲，几微秒的事。放在中断里做的最大好处是
 *  **无论主循环被什么阻塞（比如擦 Flash 几十毫秒），帧都不会丢**。
 *  而"回流控帧"要操作 CAN 发送邮箱，就留给主循环，避免中断里做重活。
 *
 *  这也和 AUTOSAR 的实际行为一致：CanIf 收到帧后是在 CAN 中断里
 *  直接调 CanTp_RxIndication 的。
 *================================================================================================*/

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "cantp.h"
#include "pdur.h"                /* 发帧的唯一出口（不再直接碰 Can 驱动） */
#include "Det.h"

#include <string.h>                 /* memcpy / memset */

/*==================================================================================================
 *                          Cortex-M7 调试组件（DWT）寄存器地址
 *
 *  【为什么不用 CMSIS 的 DWT / CoreDebug 符号】
 *  本工程的 include 路径里没有 core_cm7.h（工程用的是 NXP 自己的 S32_SCB 宏），
 *  所以 DWT / CoreDebug 这些 CMSIS 符号不可见。
 *
 *  但这几个寄存器地址是 ARMv7-M 架构固定值，与芯片无关，直接按地址访问即可：
 *      DEMCR      = 0xE000EDFC   bit24 = TRCENA（打开跟踪/调试组件时钟）
 *      DWT_CTRL   = 0xE0001000   bit0  = CYCCNTENA（启动周期计数器）
 *      DWT_CYCCNT = 0xE0001004   32 位自由运行周期计数
 *
 *  这样做的好处：零依赖、不担心 CMSIS 版本差异、代码一目了然。
 *================================================================================================*/
#define CANTP_REG_DEMCR             (*(volatile uint32*)0xE000EDFCUL)
#define CANTP_REG_DWT_CTRL          (*(volatile uint32*)0xE0001000UL)
#define CANTP_REG_DWT_CYCCNT        (*(volatile uint32*)0xE0001004UL)

#define CANTP_DEMCR_TRCENA          (0x01000000UL)      /**< DEMCR bit24 */
#define CANTP_DWT_CTRL_CYCCNTENA    (0x00000001UL)      /**< DWT_CTRL bit0 */

/*==================================================================================================
 *                                   LOCAL DEFINES
 *================================================================================================*/
#if (CANTP_DEV_ERROR_DETECT == STD_ON)
    #define CANTP_DET_ERROR(api, err)                                   \
        ((void)Det_ReportError((uint16)CANTP_MODULE_ID,                 \
                               (uint8)CANTP_INSTANCE_ID,                \
                               (uint8)(api),                            \
                               (uint8)(err)))
#else
    #define CANTP_DET_ERROR(api, err)   ((void)0)
#endif

/** @brief API 编号（Det 上报用，取值参考 AUTOSAR SWS_CanTp） */
#define CANTP_API_INIT                  (0x01U)
#define CANTP_API_TRANSMIT              (0x02U)
#define CANTP_API_RX_INDICATION         (0x03U)
#define CANTP_API_TX_CONFIRMATION       (0x04U)
#define CANTP_API_GET_RX_MESSAGE        (0x05U)
#define CANTP_API_MAIN_FUNCTION         (0x06U)
#define CANTP_API_CANCEL_TRANSMIT       (0x07U)
#define CANTP_API_CANCEL_RECEIVE        (0x08U)
#define CANTP_API_CHANGE_PARAMETER      (0x09U)
#define CANTP_API_READ_PARAMETER        (0x0AU)
#define CANTP_API_SHUTDOWN              (0x0BU)
#define CANTP_API_GET_VERSION_INFO      (0x0CU)

/** @brief 错误编号 */
#define CANTP_E_PARAM_POINTER           (0x01U)
#define CANTP_E_PARAM_LENGTH            (0x02U)
#define CANTP_E_BUSY                    (0x03U)
#define CANTP_E_BUFFER_TOO_SMALL        (0x04U)

/** @brief 超时换算成微秒 */
#define CANTP_N_BS_US                   ((uint32)CANTP_N_BS_MS * 1000UL)
#define CANTP_N_CR_US                   ((uint32)CANTP_N_CR_MS * 1000UL)

/** @brief 流控帧里我们要求的 STmin（毫秒 → 原始编码，直接就是毫秒值） */
#define CANTP_TX_FC_STMIN_RAW           ((uint8)CANTP_TX_FC_STMIN_MS)

/**
 * @brief 帧不足 8 字节时用于填充的字节
 * @details 值来自 cantp_cfg.h 的 CANTP_PADDING_BYTE（默认 0xCC），
 *          对应 AUTOSAR 的 CanTpPaddingByte 配置参数。
 *          接收方按 PCI 里的长度忽略这些字节，所以内容不影响协议。
 */
#define CANTP_CF_PADDING                ((uint8)CANTP_PADDING_BYTE)

/*==================================================================================================
 *                                   LOCAL VARIABLES
 *================================================================================================*/

/*--------------------------- 接收方向 ---------------------------*/

/** @brief 接收重组缓冲 */
static uint8    CanTp_RxBuffer[CANTP_RX_BUF_SIZE];

/** @brief 当前消息已拼装的字节数 */
static uint32   CanTp_RxLen = 0U;

/** @brief 当前消息的总长度（首帧里带过来的） */
static uint32   CanTp_RxTotal = 0U;

/** @brief 下一个期望的连续帧序号 */
static uint8    CanTp_RxSN = 0U;

/** @brief 已收到一条完整消息，等待上层取走 */
static boolean  CanTp_RxReady = FALSE;

/** @brief 接收状态 */
static CanTp_StateType CanTp_RxState = CANTP_STATE_IDLE;

/** @brief 接收侧计时起点（等连续帧的 N_Cr） */
static uint32   CanTp_RxTimerUs = 0U;

/** @brief 有一帧流控帧等着发 */
static boolean  CanTp_FcPending = FALSE;

/** @brief 该流控帧的状态（CTS / WAIT / OVFLW） */
static uint8    CanTp_FcStatus = CANTP_FS_CTS;

/* 可以通过 CanTp_ChangeParameter() 在运行期修改的流控参数。
 * 初值来自 cantp_cfg.h 的宏；AUTOSAR 允许 TP_BS / TP_STMIN 在运行期动态设置。 */
static uint8    CanTp_FcBlockSize = (uint8)CANTP_TX_FC_BLOCK_SIZE;
static uint8    CanTp_FcStminRaw  = (uint8)CANTP_TX_FC_STMIN_RAW;

/*--------------------------- 发送方向 ---------------------------*/

/** @brief 发送分段缓冲（CanTp_Transmit 时拷贝进来） */
static uint8    CanTp_TxBuffer[CANTP_TX_BUF_SIZE];

/** @brief 待发消息总长度 */
static uint32   CanTp_TxLen = 0U;

/** @brief 已发出的字节数 */
static uint32   CanTp_TxSent = 0U;

/** @brief 下一个连续帧序号 */
static uint8    CanTp_TxSN = 0U;

/** @brief 发送状态 */
static CanTp_StateType CanTp_TxState = CANTP_STATE_IDLE;

/** @brief 发送侧计时起点（等流控的 N_Bs / STmin 间隔） */
static uint32   CanTp_TxTimerUs = 0U;

/** @brief 对端要求的块大小（0 = 一次发完） */
static uint8    CanTp_TxBlockSize = 0U;

/** @brief 本块内已发出的帧数 */
static uint8    CanTp_TxSentInBlock = 0U;

/** @brief 对端要求的最小间隔（微秒） */
static uint32   CanTp_TxStminUs = 0U;

/** @brief 发送邮箱正忙（等 TxConfirmation） */
static boolean  CanTp_TxConfPending = FALSE;

/*--------------------------- 统计与诊断 ---------------------------*/

static CanTp_AbortReasonType CanTp_LastAbort   = CANTP_ABORT_NONE;
static uint32   CanTp_RxFrameCount = 0U;
static uint32   CanTp_RxMsgCount   = 0U;
static uint32   CanTp_TxFrameCount = 0U;
static uint32   CanTp_AbortCount   = 0U;
static boolean  CanTp_TimeBaseOk   = FALSE;

/* 软件扩展的微秒计数器（见 CanTp_GetTimeUs 的说明） */
static uint32   CanTp_LastCycles   = 0U;   /**< 上次读到的原始 CYCCNT */
static uint32   CanTp_UsRemainder  = 0U;   /**< 不足 1 微秒的余数（周期数） */
static uint32   CanTp_UsCounter    = 0U;   /**< 32 位微秒计数，71.6 分钟回绕 */
static boolean  CanTp_Initialized  = FALSE;

/* 中止瞬间的接收进度，用来解释"是拼到一半断掉的"还是"根本没开始" */
static uint32   CanTp_LastAbortRxLen   = 0U;
static uint32   CanTp_LastAbortRxTotal = 0U;

/*==================================================================================================
 *                                   时间基准（DWT 周期计数器）
 *================================================================================================*/

/**
 * @brief 初始化时间基准
 *
 * @details 【为什么用 DWT->CYCCNT】
 *  本工程没有配定时器中断（也不该为一个超时去占用一个定时器）。
 *  Cortex-M7 内核自带一个 32 位自由运行周期计数器 DWT->CYCCNT，
 *  以 CPU 主频递增：
 *      160 MHz → 1 微秒 = 160 个周期
 *      32 位 → 约 26.8 秒回绕一次
 *  我们的超时最长 1 秒，回绕完全不影响（用无符号差值比较即可）。
 *
 *  【启用步骤（ARMv7-M 标准做法）】
 *      ① DEMCR.TRCENA = 1   打开跟踪/调试组件的时钟
 *      ② CYCCNT = 0         清零
 *      ③ CTRL.CYCCNTENA = 1 启动计数
 *
 *  【为什么还要自检】
 *  个别芯片/调试配置下 DWT 可能不可用。所以初始化后读两次计数器、
 *  中间空转一小段，确认它真的在涨 —— 结果存进 CanTp_TimeBaseOk，
 *  上层可以先查一下再决定是否信任超时功能。
 */
static void CanTp_InitTimeBase(void)
{
    uint32 first;
    uint32 second;
    uint32 i;

    /* ① 打开跟踪组件时钟 */
    CANTP_REG_DEMCR |= CANTP_DEMCR_TRCENA;

    /* ② 清零 */
    CANTP_REG_DWT_CYCCNT = 0U;

    /* ③ 启动 */
    CANTP_REG_DWT_CTRL |= CANTP_DWT_CTRL_CYCCNTENA;

    /* 自检：空转一小段，看计数器是否递增 */
    first = CANTP_REG_DWT_CYCCNT;
    for (i = 0U; i < 1000U; i++)
    {
        /* 故意空转，让计数器走过至少几千个周期 */
    }
    second = CANTP_REG_DWT_CYCCNT;

    CanTp_TimeBaseOk = (second != first) ? TRUE : FALSE;

    /* 记录起点并清零软件扩展计数器 */
    CanTp_LastCycles    = CANTP_REG_DWT_CYCCNT;
    CanTp_UsRemainder   = 0U;
    CanTp_UsCounter     = 0U;
}

/*--------------------------------------------------------------------------------------------------
 *                          临界区 + 超时判定（2026-10-25 修并发 bug）
 *
 * 【这个 bug 怎么被发现的】
 *  把日志瘦身之后（不再逐帧打印），刷写开始在第 4 个连续帧处中止，原因是 N_Cr 超时。
 *  但串口的 CAN 计数显示"收 312 帧"—— 帧全都到了，是 CanTp 判错了。
 *
 * 【根因】
 *  CanTp_GetTimeUs() 同时被 CAN 中断（CanTp_RxIndication）和主循环
 *  （CanTp_MainFunction）调用，而它内部是三组"读-改-写"：
 *      CanTp_LastCycles / CanTp_UsCounter / CanTp_UsRemainder
 *  中断一旦插在中间，主循环会用"过期的旧值"覆盖中断刚写进去的新值，
 *  于是 UsCounter 可能**回退**一点点。
 *
 *  回退的后果是灾难性的：
 *      (uint32)(now - CanTp_RxTimerUs)  当 now 比 RxTimerUs 小 1，
 *      无符号减法得到 0xFFFFFFFF ≈ 71 分钟 → 远大于 N_Cr(60 秒)
 *      → 每一次都会"立刻超时" → 接收状态被清成 IDLE
 *      → 之后所有连续帧都走"不在等连续帧"分支被静默丢弃
 *      → 只收到 FF + 4 帧就停住，60 秒后报 N_Cr 超时。
 *
 * 【为什么以前没暴露】
 *  v1.8 及以前每帧都打印串口（每帧约 9 次阻塞式 Uart_Printf），
 *  主循环一圈要 1 毫秒左右，CanTp_GetTimeUs 每秒只被调约 1000 次，
 *  撞上中断的概率极低。日志瘦身之后主循环快了约 500 倍，
 *  每秒调用 50 万次 —— 竞态几乎必然发生。
 *  **也就是说：以前的"慢"一直在掩护这个 bug。**
 *------------------------------------------------------------------------------------------------*/

/**
 * @brief 进临界区（关中断），返回进入前的 PRIMASK
 * @note  用 mrs/cpsid 而不是 CMSIS，避免依赖头文件是否被包含
 */
static uint32 CanTp_EnterCritical(void)
{
    uint32 primask;

    __asm volatile ("mrs %0, primask" : "=r" (primask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");

    return primask;
}

/**
 * @brief 出临界区：把 PRIMASK 恢复成进入前的值
 * @note  不用 cpsie i —— 如果调用者本来就在关中断环境里（比如 ISR），
 *        直接开中断是错的。恢复原值才安全。
 */
static void CanTp_ExitCritical(uint32 Primask)
{
    __asm volatile ("msr primask, %0" :: "r" (Primask) : "memory");
}

/**
 * @brief 判断"从 Start 到现在是否真的超过 Limit 微秒"
 *
 * @details 为什么不直接写 (Now - Start) > Limit：
 *  只要 Now 比 Start 小一点点（时间基准抖一下、或变量被别处改写），
 *  无符号减法就下溢成 40 多亿，等于"已经过了 71 分钟"，
 *  任何超时都会瞬间触发。这就是上面那个 bug 的直接表现。
 *
 *  这里额外要求差值落在合理区间（< 2^31 微秒 ≈ 35 分钟）才认。
 *  即使将来还有别的并发问题，也不会再"误判成超时"。
 */
static boolean CanTp_ElapsedExceeds(uint32 Now, uint32 Start, uint32 Limit)
{
    uint32 elapsed = (uint32)(Now - Start);

    if (elapsed >= 0x80000000UL)
    {
        return FALSE;       /* 看起来像"负数" → 时间没往前走，不算超时 */
    }

    return (elapsed > Limit) ? TRUE : FALSE;
}

/**
 * @brief 取当前时间（微秒，软件扩展成 32 位）
 *
 * @details 【为什么不能直接用 CYCCNT / 160 —— 这是一个很隐蔽的坑】
 *
 *  直接除出来的"微秒值"，它的**模是 26.84 秒**（因为 CYCCNT 每 26.84 秒回绕），
 *  但下面这种写法是按 **2^32** 取模的：
 *          (uint32)(now - start) > timeout
 *  两者模数不一致，后果是：
 *          只要"现在的绝对时间"跨过了 26.84 秒那道坎，
 *          差值就会突然变成一个接近 42 亿的巨值 → **立刻误判超时**。
 *
 *  实测症状：S3 超时（配置 24 秒）会在运行约 26.8 秒后随机触发一次，
 *  看起来像"莫名其妙掉会话"，而且时有时无 —— 取决于发帧时刻落在周期的哪一段。
 *
 *  【正确做法：把周期数累加成一个真正的 32 位微秒计数器】
 *      每次调用取 CYCCNT，算"距上次的周期差"（这个减法模 2^32，回绕安全），
 *      再按 160 周期/微秒折算成微秒累加。
 *  这样 usCounter 的模就是 2^32 微秒 = **71.6 分钟**，
 *  与 uint32 减法一致，差值比较天然正确。
 *
 *  【前提】必须**定期调用**（至少每 20 秒一次），否则两次调用之间可能跨过
 *  CYCCNT 的 26.84 秒回绕而丢数。主循环每圈都会调，远快于这个要求。
 *
 * @note 如果 DWT 不可用，返回 0 —— 此时超时判断会失效（永远不超时），
 *       但基本收发仍然工作。这是刻意选择"宁可不停机"而不是"乱超时"。
 */
static uint32 CanTp_GetTimeUs(void)
{
    uint32 cyc;
    uint32 deltaCyc;
    uint32 primask;
    uint32 value;

    if (CanTp_TimeBaseOk == FALSE)
    {
        return 0U;
    }

    /* ★ 关中断保护这三组"读-改-写"。
     *   不保护的话，CAN 中断插在中间会让主循环用旧值覆盖新值，
     *   UsCounter 回退 → 所有超时比较下溢 → 立刻误判超时。
     *   详见文件上方 CanTp_EnterCritical 的说明。 */
    primask = CanTp_EnterCritical();

    cyc      = CANTP_REG_DWT_CYCCNT;
    deltaCyc = cyc - CanTp_LastCycles;      /* 模 2^32，回绕安全 */
    CanTp_LastCycles = cyc;

    /* 用"整除 + 余数"避免 deltaCyc + 余数 溢出 */
    CanTp_UsCounter   += deltaCyc / CANTP_CYCLES_PER_US;
    CanTp_UsRemainder += deltaCyc % CANTP_CYCLES_PER_US;

    if (CanTp_UsRemainder >= CANTP_CYCLES_PER_US)
    {
        CanTp_UsCounter++;
        CanTp_UsRemainder -= CANTP_CYCLES_PER_US;
    }

    value = CanTp_UsCounter;

    CanTp_ExitCritical(primask);

    return value;
}

/*==================================================================================================
 *                                    发送辅助
 *================================================================================================*/

/**
 * @brief 把一帧交给 CAN 驱动发出去
 *
 * @param[in] Frame 帧内容（长度含 PCI 字节）
 * @param[in] Len   长度（1 ~ 8）
 *
 * @return TRUE  已成功交给驱动
 *         FALSE 邮箱忙或驱动拒绝 —— 调用方应当在下一轮 MainFunction 重试
 *
 * @note `Can_Write` 内部是把数据**拷贝**进邮箱寄存器的，所以 Frame 可以是
 *       局部数组，函数返回后失效也没关系。
 */
static boolean CanTp_SendFrame(const uint8* Frame, uint8 Len)
{
    PduInfoType    info;
    Std_ReturnType ret;

    info.SduDataPtr  = (uint8*)Frame;       /* 下游只读，不会改 */
    info.SduLength   = (PduLengthType)Len;
    info.MetaDataPtr = NULL_PTR;

    /*------------------------------------------------------------------
     * 交给 PduR，由它路由到 CanIf → Can_Write。
     *
     * 【为什么不直接调 Can_Write】
     * CanTp 处理的是 N-PDU（8 字节数据块），它**不应该知道**：
     *      - 这帧挂在哪个 CAN ID 上      （CanIf 的知识）
     *      - 用哪个发送邮箱               （MCAL 的知识）
     * 以前这里写 Can_Write(CanConf_TxObject_Hth4, &pdu)，等于把本文件
     * 焊死在 Can 驱动上；换 CAN FD 或换 MCU 就得改它。
     * 现在它只依赖 PduInfoType，硬件无关。
     *------------------------------------------------------------------*/
    ret = PduR_CanTpTransmit((PduIdType)CANTP_NPDU_TX_ID, &info);

    if (ret == E_OK)
    {
        CanTp_TxFrameCount++;
        CanTp_TxConfPending = TRUE;         /* 等 TxConfirmation 才能发下一帧 */
        return TRUE;
    }

    /* CAN_BUSY（邮箱还占着）或真失败 —— 都留到下一次 MainFunction 重试 */
    return FALSE;
}

/**
 * @brief 发一帧流控帧
 *
 * @details 格式：PCI(1) + BS(1) + STmin(1) + 填充(5)
 *              首字节 = 0x30 | FS
 *
 * @return TRUE 已交给驱动；FALSE 邮箱忙，需要下次重试
 */
static boolean CanTp_SendFlowControl(uint8 Fs)
{
    uint8 frame[CANTP_CAN_DL];

    frame[0] = (uint8)((CANTP_PCI_FC << 4) | (Fs & 0x0FU));
    frame[1] = CanTp_FcBlockSize;
    frame[2] = CanTp_FcStminRaw;

    /* 经典 CAN 帧固定 8 字节，多余部分按 ISO 15765-2 建议填 0xCC */
    (void)memset(&frame[3], (int)CANTP_CF_PADDING, 5U);

    return CanTp_SendFrame(frame, (uint8)CANTP_CAN_DL);
}

/**
 * @brief 把 STmin 原始编码解码成微秒
 *
 * @details ISO 15765-2 规定的编码：
 *              0x00 ~ 0x7F   → 0 ~ 127 毫秒
 *              0xF1 ~ 0xF9   → 100 ~ 900 微秒
 *              其它（0x80~0xF0、0xFA~0xFF）→ 保留值，按 0 处理（尽量快）
 */
static uint32 CanTp_DecodeStmin(uint8 Raw)
{
    if (Raw <= 0x7FU)
    {
        return (uint32)Raw * 1000UL;
    }

    if ((Raw >= 0xF1U) && (Raw <= 0xF9U))
    {
        return (uint32)(Raw - 0xF0U) * 100UL;
    }

    return 0UL;
}

/**
 * @brief 中止当前传输（两个方向都回到空闲）
 * @param[in] Reason 中止原因，记录到 CanTp_LastAbort
 */
static void CanTp_Abort(CanTp_AbortReasonType Reason)
{
    CanTp_LastAbort     = Reason;
    CanTp_AbortCount++;

    /* 先记录"断在哪" —— 必须在下面清零之前保存，否则信息就丢了。
     *
     * 这两个数的用途：
     *    RxLen = 0                 → 根本没收到首帧，问题在更早的地方
     *    0 < RxLen < RxTotal       → 拼到一半断掉（这是最常见的情况）
     *    RxTotal = 0               → 不是接收方向的中止
     */
    CanTp_LastAbortRxLen   = CanTp_RxLen;
    CanTp_LastAbortRxTotal = CanTp_RxTotal;

    CanTp_RxState       = CANTP_STATE_IDLE;
    CanTp_RxLen         = 0U;
    CanTp_RxTotal       = 0U;
    CanTp_RxReady       = FALSE;
    CanTp_FcPending     = FALSE;

    CanTp_TxState       = CANTP_STATE_IDLE;
    CanTp_TxLen         = 0U;
    CanTp_TxSent        = 0U;
    CanTp_TxSentInBlock = 0U;
}

/*==================================================================================================
 *                                    接收处理（ISR 上下文）
 *================================================================================================*/

/**
 * @brief 处理单帧（SF）—— 整条消息一帧装完
 */
static void CanTp_HandleSingleFrame(const uint8* Data, uint8 Len)
{
    uint8 sfLen = (uint8)(Data[0] & 0x0FU);

    /* 长度 0 表示"转义帧"（更长 PCI），本工程不支持 */
    if (sfLen == 0U)
    {
        return;
    }

    if (sfLen > CANTP_SF_MAX_DATA)
    {
        CanTp_Abort(CANTP_ABORT_RX_OVFLW);
        return;
    }

    /* 报文实际长度不够，属于对端违规 */
    if ((uint32)(sfLen + 1U) > (uint32)Len)
    {
        CanTp_Abort(CANTP_ABORT_RX_OVFLW);
        return;
    }

    /* 上一条还没被取走 —— 丢弃新的，并计一次溢出 */
    if (CanTp_RxReady != FALSE)
    {
        CanTp_Abort(CANTP_ABORT_RX_OVFLW);
        return;
    }

    (void)memcpy(CanTp_RxBuffer, &Data[1], (size_t)sfLen);

    CanTp_RxLen     = (uint32)sfLen;
    CanTp_RxTotal   = (uint32)sfLen;
    CanTp_RxReady   = TRUE;
    CanTp_RxState   = CANTP_STATE_IDLE;
    CanTp_RxMsgCount++;
}

/**
 * @brief 处理首帧（FF）—— 长消息的第 1 帧
 */
static void CanTp_HandleFirstFrame(const uint8* Data, uint8 Len)
{
    uint32 total;

    if (Len < 3U)
    {
        return;
    }

    /* 12 位总长度：首字节低 4 位是高 4 位，第 2 字节是低 8 位 */
    total = ((uint32)(Data[0] & 0x0FU) << 8) | (uint32)Data[1];

    if (total > (uint32)CANTP_RX_BUF_SIZE)
    {
        /* 装不下 —— 先中止本条消息，再安排回一个"溢出"流控帧。
         *
         * ⚠️ 顺序很重要：CanTp_Abort() 会把 CanTp_FcPending 清掉，
         *    所以必须"先 abort，再请求发 FC"，否则溢出通知永远发不出去。 */
        CanTp_Abort(CANTP_ABORT_RX_OVFLW);

        CanTp_FcStatus  = CANTP_FS_OVFLW;
        CanTp_FcPending = TRUE;
        return;
    }

    if (total <= (uint32)CANTP_SF_MAX_DATA)
    {
        /* 这个长度本该用单帧，对端违规 —— 忽略 */
        return;
    }

    if (CanTp_RxReady != FALSE)
    {
        CanTp_Abort(CANTP_ABORT_RX_OVFLW);
        return;
    }

    CanTp_RxTotal = total;
    CanTp_RxLen   = (uint32)CANTP_FF_DATA_LEN;

    /* 首帧带 6 字节数据（8 - 2 字节 PCI） */
    (void)memcpy(CanTp_RxBuffer, &Data[2], (size_t)CANTP_FF_DATA_LEN);

    CanTp_RxSN      = 1U;                     /* 下一个连续帧序号从 1 开始 */
    CanTp_RxState   = CANTP_STATE_RX_WAIT_CF;
    CanTp_RxTimerUs = CanTp_GetTimeUs();

    /* 回送流控帧，告诉对端"可以继续" —— 实际发送留给 MainFunction */
    CanTp_FcStatus  = CANTP_FS_CTS;
    CanTp_FcPending = TRUE;
}

/**
 * @brief 处理连续帧（CF）—— 长消息的后续帧
 */
static void CanTp_HandleConsecutiveFrame(const uint8* Data, uint8 Len)
{
    uint8  sn;
    uint32 n;
    uint32 i;
    uint32 now;

    /* 不在等连续帧 → 孤立的 CF，直接忽略（可能是上一次传输的残留） */
    if (CanTp_RxState != CANTP_STATE_RX_WAIT_CF)
    {
        return;
    }

    sn = (uint8)(Data[0] & 0x0FU);

    /* 序号错 = 中间丢帧了。绝不能"尽力拼接"，否则数据整体错位 */
    if (sn != CanTp_RxSN)
    {
        CanTp_Abort(CANTP_ABORT_RX_SEQ);
        return;
    }

    /* 本帧还能提供多少字节 */
    n = CanTp_RxTotal - CanTp_RxLen;
    if (n > (uint32)CANTP_CF_DATA_LEN)
    {
        n = (uint32)CANTP_CF_DATA_LEN;
    }
    if (n > (uint32)(Len - 1U))
    {
        n = (uint32)(Len - 1U);
    }

    for (i = 0U; i < n; i++)
    {
        CanTp_RxBuffer[CanTp_RxLen + i] = Data[1U + i];
    }

    CanTp_RxLen += n;
    CanTp_RxSN   = (uint8)((CanTp_RxSN + 1U) & 0x0FU);

    now = CanTp_GetTimeUs();
    CanTp_RxTimerUs = now;

    if (CanTp_RxLen >= CanTp_RxTotal)
    {
        /* 拼满了 */
        CanTp_RxReady = TRUE;
        CanTp_RxState = CANTP_STATE_IDLE;
        CanTp_RxMsgCount++;
    }
}

/**
 * @brief 处理流控帧（FC）—— 对端回应我们的首帧
 */
static void CanTp_HandleFlowControl(const uint8* Data, uint8 Len)
{
    uint8 fs;
    uint32 now;

    /* 只有我们自己发了首帧、在等流控的时候才处理 */
    if (CanTp_TxState != CANTP_STATE_TX_WAIT_FC)
    {
        return;
    }

    if (Len < 3U)
    {
        return;
    }

    fs  = (uint8)(Data[0] & 0x0FU);
    now = CanTp_GetTimeUs();

    switch (fs)
    {
        case CANTP_FS_CTS:
            /* 可以继续 —— 记下对端要求的节奏 */
            CanTp_TxBlockSize     = Data[1];
            CanTp_TxStminUs       = CanTp_DecodeStmin(Data[2]);
            CanTp_TxSentInBlock   = 0U;
            CanTp_TxTimerUs       = now;
            CanTp_TxState         = CANTP_STATE_TX_SENDING;
            break;

        case CANTP_FS_WAIT:
            /* 对端还没准备好 —— 重新计 N_Bs */
            CanTp_TxTimerUs = now;
            break;

        case CANTP_FS_OVFLW:
            CanTp_Abort(CANTP_ABORT_TX_OVFLW);
            break;

        default:
            /* 保留状态值，忽略 */
            break;
    }
}

/*==================================================================================================
 *                                   CanTp_RxIndication（ISR）
 *================================================================================================*/
void CanTp_RxIndication(PduIdType RxPduId, const PduInfoType* PduInfoPtr)
{
    const uint8* data;
    uint32       len;
    uint8        pci;

    /* 单通道工程：只认 PDU 0。
     * 【注意】CAN ID 的过滤不在这里 —— 按 AUTOSAR 分层，那是 CanIf 的职责，
     *         所以本函数根本不接触 CAN ID，收到的就是"已经过滤好的数据"。 */
    if (RxPduId != (PduIdType)CANTP_RX_PDU_ID)
    {
        return;
    }

    if ((PduInfoPtr == NULL_PTR) || (PduInfoPtr->SduDataPtr == NULL_PTR) ||
        (PduInfoPtr->SduLength == 0U))
    {
        CANTP_DET_ERROR(CANTP_API_RX_INDICATION, CANTP_E_PARAM_POINTER);
        return;
    }

    data = PduInfoPtr->SduDataPtr;
    len  = (uint32)PduInfoPtr->SduLength;

    /* 超过经典 CAN 一帧长度属于下层错误，直接拒绝 */
    if (len > (uint32)CANTP_CAN_DL)
    {
        return;
    }

    CanTp_RxFrameCount++;

    pci = (uint8)((data[0] >> 4) & 0x0FU);

    switch (pci)
    {
        case CANTP_PCI_SF:
            CanTp_HandleSingleFrame(data, (uint8)len);
            break;

        case CANTP_PCI_FF:
            CanTp_HandleFirstFrame(data, (uint8)len);
            break;

        case CANTP_PCI_CF:
            CanTp_HandleConsecutiveFrame(data, (uint8)len);
            break;

        case CANTP_PCI_FC:
            CanTp_HandleFlowControl(data, (uint8)len);
            break;

        default:
            /* 高 4 位 0x4~0xF 未定义，忽略 */
            break;
    }
}

/*==================================================================================================
 *                                   CanTp_TxConfirmation（ISR）
 *================================================================================================*/
void CanTp_TxConfirmation(PduIdType TxPduId, Std_ReturnType result)
{
    /* 单通道工程只有一路发送 PDU */
    if (TxPduId != (PduIdType)CANTP_TX_PDU_ID)
    {
        return;
    }

    /* 发送失败：不能再等下一帧了，直接中止，避免永久卡在"等确认" */
    if (result != E_OK)
    {
        CanTp_Abort(CANTP_ABORT_TX_FAILED);
        return;
    }

    CanTp_TxConfPending = FALSE;
}

/*==================================================================================================
 *                                   CanTp_Transmit（主循环）
 *================================================================================================*/
Std_ReturnType CanTp_Transmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr)
{
    const uint8* data;
    uint32       length;
    uint8        frame[CANTP_CAN_DL];
    uint32       i;

    if (TxPduId != (PduIdType)CANTP_TX_PDU_ID)
    {
        return E_NOT_OK;
    }

    if ((PduInfoPtr == NULL_PTR) || (PduInfoPtr->SduDataPtr == NULL_PTR))
    {
        CANTP_DET_ERROR(CANTP_API_TRANSMIT, CANTP_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    data   = PduInfoPtr->SduDataPtr;
    length = (uint32)PduInfoPtr->SduLength;

    if ((length == 0U) || (length > (uint32)CANTP_MAX_MSG_LEN))
    {
        CANTP_DET_ERROR(CANTP_API_TRANSMIT, CANTP_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    /* 上一次发送还没结束 → 拒绝（CanTp 同一时刻只处理一条消息） */
    if ((CanTp_TxState != CANTP_STATE_IDLE) || (CanTp_TxConfPending != FALSE))
    {
        CANTP_DET_ERROR(CANTP_API_TRANSMIT, CANTP_E_BUSY);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 短消息（≤7 字节）—— 单帧一次发完
     *------------------------------------------------------------------------*/
    if (length <= (uint32)CANTP_SF_MAX_DATA)
    {
        frame[0] = (uint8)((CANTP_PCI_SF << 4) | (uint8)length);
        for (i = 0U; i < length; i++)
        {
            frame[1U + i] = data[i];
        }

        /* 经典 CAN 固定 8 字节，多余填充 */
        for (i = length; i < (uint32)CANTP_SF_MAX_DATA; i++)
        {
            frame[1U + i] = CANTP_CF_PADDING;
        }

        if (CanTp_SendFrame(frame, (uint8)CANTP_CAN_DL) == FALSE)
        {
            /* 邮箱忙 —— 这不是故障，调用方稍后重试即可 */
            return E_NOT_OK;
        }

        CanTp_TxState = CANTP_STATE_IDLE;
        return E_OK;
    }

    /*--------------------------------------------------------------------------
     * 长消息 —— 先拷贝到内部缓冲，再发首帧
     *
     * 【为什么要拷贝】
     * 首帧发出去之后，后续连续帧要由 MainFunction 一帧一帧慢慢发，
     * 期间调用方（Dcm）的缓冲可能已经被复用或释放。
     * 所以必须自己留一份。
     *------------------------------------------------------------------------*/
    if (length > (uint32)CANTP_TX_BUF_SIZE)
    {
        CANTP_DET_ERROR(CANTP_API_TRANSMIT, CANTP_E_PARAM_LENGTH);
        return E_NOT_OK;
    }

    (void)memcpy(CanTp_TxBuffer, data, (size_t)length);

    /* 首帧：PCI(2) + 6 字节数据 */
    frame[0] = (uint8)((CANTP_PCI_FF << 4) | (uint8)((length >> 8) & 0x0FU));
    frame[1] = (uint8)(length & 0xFFU);
    for (i = 0U; i < (uint32)CANTP_FF_DATA_LEN; i++)
    {
        frame[2U + i] = CanTp_TxBuffer[i];
    }

    if (CanTp_SendFrame(frame, (uint8)CANTP_CAN_DL) == FALSE)
    {
        return E_NOT_OK;
    }

    CanTp_TxLen         = length;
    CanTp_TxSent        = (uint32)CANTP_FF_DATA_LEN;
    CanTp_TxSN          = 1U;                               /* 连续帧从 1 开始 */
    CanTp_TxSentInBlock = 0U;
    CanTp_TxState       = CANTP_STATE_TX_WAIT_FC;
    CanTp_TxTimerUs     = CanTp_GetTimeUs();

    return E_OK;
}

/*==================================================================================================
 *                                   CanTp_MainFunction（主循环）
 *================================================================================================*/
void CanTp_MainFunction(void)
{
    uint32 now = CanTp_GetTimeUs();

    /*==========================================================================
     * 1. 接收侧超时（N_Cr）
     *
     * 收到首帧或某帧连续帧之后，对端必须在 N_Cr 内继续发。
     * 超时就放弃这条消息 —— 否则会永久卡在"等连续帧"，整个刷写流程假死。
     *========================================================================*/
    if (CanTp_RxState == CANTP_STATE_RX_WAIT_CF)
    {
        if (CanTp_ElapsedExceeds(now, CanTp_RxTimerUs, CANTP_N_CR_US) != FALSE)
        {
            CanTp_Abort(CANTP_ABORT_RX_TIMEOUT);
        }
    }

    /*==========================================================================
     * 2. 有待发的流控帧就发出去
     *
     * 放在主循环而不是中断里，是为了避免在中断上下文操作 CAN 发送邮箱。
     *========================================================================*/
    if (CanTp_FcPending != FALSE)
    {
        if (CanTp_TxConfPending == FALSE)
        {
            /* ⚠️ 只有真的发出去了才清标志。
             * 如果邮箱正忙（Can_Write 返回 CAN_BUSY），必须保留待发状态，
             * 下一轮再试 —— 否则流控帧会被静默丢掉，对端就会一直等我们。 */
            if (CanTp_SendFlowControl(CanTp_FcStatus) != FALSE)
            {
                CanTp_FcPending = FALSE;
            }
        }
    }

    /*==========================================================================
     * 3. 发送侧状态机
     *========================================================================*/
    switch (CanTp_TxState)
    {
        case CANTP_STATE_TX_WAIT_FC:
            /* 等对端流控帧，超时记为 N_Bs */
            if (CanTp_ElapsedExceeds(now, CanTp_TxTimerUs, CANTP_N_BS_US) != FALSE)
            {
                CanTp_Abort(CANTP_ABORT_TX_TIMEOUT);
            }
            break;

        case CANTP_STATE_TX_SENDING:
            /* 上一帧还没发完（等 TxConfirmation）→ 不能发下一帧，
             * 因为本工程只有 1 个发送邮箱 */
            if (CanTp_TxConfPending != FALSE)
            {
                break;
            }

            /* 全部发完 */
            if (CanTp_TxSent >= CanTp_TxLen)
            {
                CanTp_TxState = CANTP_STATE_IDLE;
                break;
            }

            /* 对端指定了块大小，且本块已发满 → 回去等新的流控帧 */
            if ((CanTp_TxBlockSize != 0U) &&
                (CanTp_TxSentInBlock >= CanTp_TxBlockSize))
            {
                CanTp_TxState     = CANTP_STATE_TX_WAIT_FC;
                CanTp_TxTimerUs   = now;
                CanTp_TxSentInBlock = 0U;
                break;
            }

            /* 还没到对端要求的最小间隔 STmin → 等 */
            if ((uint32)(now - CanTp_TxTimerUs) < CanTp_TxStminUs)
            {
                break;
            }

            /* 发一帧连续帧 */
            {
                uint8  frame[CANTP_CAN_DL];
                uint32 remain = CanTp_TxLen - CanTp_TxSent;
                uint8  n = (uint8)((remain > (uint32)CANTP_CF_DATA_LEN)
                                   ? CANTP_CF_DATA_LEN : remain);
                uint32 i;

                frame[0] = (uint8)((CANTP_PCI_CF << 4) | (CanTp_TxSN & 0x0FU));

                for (i = 0U; i < (uint32)n; i++)
                {
                    frame[1U + i] = CanTp_TxBuffer[CanTp_TxSent + i];
                }
                for (i = (uint32)n; i < (uint32)CANTP_CF_DATA_LEN; i++)
                {
                    frame[1U + i] = CANTP_CF_PADDING;
                }

                if (CanTp_SendFrame(frame, (uint8)CANTP_CAN_DL) != FALSE)
                {
                    CanTp_TxSent      += n;
                    CanTp_TxSN         = (uint8)((CanTp_TxSN + 1U) & 0x0FU);
                    CanTp_TxSentInBlock++;
                    CanTp_TxTimerUs    = now;
                }
            }
            break;

        default:
            /* IDLE / RX_WAIT_CF —— 发送侧没事可做 */
            break;
    }
}

/*==================================================================================================
 *                                   CanTp_Init / 查询接口
 *================================================================================================*/
void CanTp_Init(const CanTp_ConfigType* CfgPtr)
{
    /* 本工程用编译期配置，参数不使用。
     * 保留参数是为了和 AUTOSAR SWS 的签名一致（调用时传 NULL_PTR）。 */
    (void)CfgPtr;

    /* 状态清零 */
    CanTp_RxLen         = 0U;
    CanTp_RxTotal       = 0U;
    CanTp_RxSN          = 0U;
    CanTp_RxReady       = FALSE;
    CanTp_RxState       = CANTP_STATE_IDLE;
    CanTp_RxTimerUs     = 0U;
    CanTp_FcPending     = FALSE;
    CanTp_FcStatus      = CANTP_FS_CTS;
    CanTp_FcBlockSize   = (uint8)CANTP_TX_FC_BLOCK_SIZE;
    CanTp_FcStminRaw    = (uint8)CANTP_TX_FC_STMIN_RAW;

    CanTp_TxLen         = 0U;
    CanTp_TxSent        = 0U;
    CanTp_TxSN          = 0U;
    CanTp_TxState       = CANTP_STATE_IDLE;
    CanTp_TxTimerUs     = 0U;
    CanTp_TxBlockSize   = 0U;
    CanTp_TxSentInBlock = 0U;
    CanTp_TxStminUs     = 0U;
    CanTp_TxConfPending = FALSE;

    CanTp_LastAbort     = CANTP_ABORT_NONE;
    CanTp_RxFrameCount  = 0U;
    CanTp_RxMsgCount    = 0U;
    CanTp_TxFrameCount  = 0U;
    CanTp_AbortCount    = 0U;
    CanTp_LastAbortRxLen   = 0U;
    CanTp_LastAbortRxTotal = 0U;

    /* 时间基准 */
    CanTp_InitTimeBase();

    CanTp_Initialized = TRUE;
}

/*==================================================================================================
 *                   其余 AUTOSAR SWS 标准接口
 *================================================================================================*/

Std_ReturnType CanTp_CancelTransmit(PduIdType TxPduId)
{
    if ((TxPduId != (PduIdType)CANTP_TX_PDU_ID) ||
        (CanTp_TxState == CANTP_STATE_IDLE))
    {
        return E_NOT_OK;
    }

    CanTp_TxState       = CANTP_STATE_IDLE;
    CanTp_TxLen         = 0U;
    CanTp_TxSent        = 0U;
    CanTp_TxSentInBlock = 0U;

    return E_OK;
}

Std_ReturnType CanTp_CancelReceive(PduIdType RxPduId)
{
    if ((RxPduId != (PduIdType)CANTP_RX_PDU_ID) ||
        (CanTp_RxState == CANTP_STATE_IDLE))
    {
        return E_NOT_OK;
    }

    /* 按 AUTOSAR 规定：只让本端停止接收，不主动发流控帧去中止对端 */
    CanTp_RxState   = CANTP_STATE_IDLE;
    CanTp_RxLen     = 0U;
    CanTp_RxTotal   = 0U;
    CanTp_FcPending = FALSE;

    return E_OK;
}

Std_ReturnType CanTp_ChangeParameter(PduIdType id, TPParameterType parameter, uint16 value)
{
    if (id != (PduIdType)CANTP_RX_PDU_ID)
    {
        return E_NOT_OK;
    }

    switch (parameter)
    {
        case TP_BS:
            if (value > 0xFFU)
            {
                return E_NOT_OK;
            }
            CanTp_FcBlockSize = (uint8)value;
            return E_OK;

        case TP_STMIN:
            if (value > 0xFFU)
            {
                return E_NOT_OK;
            }
            CanTp_FcStminRaw = (uint8)value;
            return E_OK;

        default:
            /* TP_BC（带宽控制）等本工程不支持 */
            return E_NOT_OK;
    }
}

Std_ReturnType CanTp_ReadParameter(PduIdType id, TPParameterType parameter, uint16* value)
{
    if ((id != (PduIdType)CANTP_RX_PDU_ID) || (value == NULL_PTR))
    {
        return E_NOT_OK;
    }

    switch (parameter)
    {
        case TP_BS:
            *value = (uint16)CanTp_FcBlockSize;
            return E_OK;

        case TP_STMIN:
            *value = (uint16)CanTp_FcStminRaw;
            return E_OK;

        default:
            return E_NOT_OK;
    }
}

void CanTp_Shutdown(void)
{
    /*
     * 【为什么这里不调 CanTp_Abort()】
     * CanTp_Abort() 会累加 CanTp_AbortCount —— 那是"传输异常中止"的统计。
     * 正常关闭模块不是异常，不该计入，否则自检输出的中止次数会失真。
     * 所以这里直接手工清状态。
     */
    CanTp_RxState       = CANTP_STATE_IDLE;
    CanTp_RxLen         = 0U;
    CanTp_RxTotal       = 0U;
    CanTp_RxReady       = FALSE;
    CanTp_FcPending     = FALSE;

    CanTp_TxState       = CANTP_STATE_IDLE;
    CanTp_TxLen         = 0U;
    CanTp_TxSent        = 0U;
    CanTp_TxSentInBlock = 0U;
    CanTp_TxConfPending = FALSE;

    CanTp_Initialized = FALSE;
}

void CanTp_GetVersionInfo(Std_VersionInfoType* versioninfo)
{
    if (versioninfo == NULL_PTR)
    {
        CANTP_DET_ERROR(CANTP_API_GET_VERSION_INFO, CANTP_E_PARAM_POINTER);
        return;
    }

    /* AR 4.9 / 本工程 1.0.0 */
    versioninfo->vendorID         = (uint16)CANTP_VENDOR_ID;
    versioninfo->moduleID         = (uint16)CANTP_MODULE_ID;
    versioninfo->sw_major_version = 1U;
    versioninfo->sw_minor_version = 0U;
    versioninfo->sw_patch_version = 0U;
}

boolean CanTp_IsRxMessageReady(void)
{
    return CanTp_RxReady;
}

Std_ReturnType CanTp_GetRxMessage(uint8* Buffer, uint32 BufferSize, uint32* OutLength)
{
    if ((Buffer == NULL_PTR) || (OutLength == NULL_PTR))
    {
        CANTP_DET_ERROR(CANTP_API_GET_RX_MESSAGE, CANTP_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    if (CanTp_RxReady == FALSE)
    {
        return E_NOT_OK;
    }

    if (BufferSize < CanTp_RxLen)
    {
        CANTP_DET_ERROR(CANTP_API_GET_RX_MESSAGE, CANTP_E_BUFFER_TOO_SMALL);
        return E_NOT_OK;
    }

    (void)memcpy(Buffer, CanTp_RxBuffer, (size_t)CanTp_RxLen);
    *OutLength = CanTp_RxLen;

    /* 取走即清除就绪标志，允许接收下一条 */
    CanTp_RxReady = FALSE;

    return E_OK;
}

CanTp_StateType CanTp_GetState(void)
{
    /* 两个方向都在忙时，优先报告接收侧（那是"正在被对端喂数据"） */
    if (CanTp_RxState != CANTP_STATE_IDLE)
    {
        return CanTp_RxState;
    }

    return CanTp_TxState;
}

CanTp_AbortReasonType CanTp_GetAbortReason(void)
{
    return CanTp_LastAbort;
}

uint32 CanTp_GetLastAbortRxLen(void)
{
    return CanTp_LastAbortRxLen;
}

uint32 CanTp_GetLastAbortRxTotal(void)
{
    return CanTp_LastAbortRxTotal;
}

boolean CanTp_IsTimeBaseOk(void)
{
    return CanTp_TimeBaseOk;
}

uint32 CanTp_GetRxFrameCount(void)
{
    return CanTp_RxFrameCount;
}

uint32 CanTp_GetRxMsgCount(void)
{
    return CanTp_RxMsgCount;
}

uint32 CanTp_GetTxFrameCount(void)
{
    return CanTp_TxFrameCount;
}

uint32 CanTp_GetAbortCount(void)
{
    return CanTp_AbortCount;
}

const char* CanTp_GetBuildStamp(void)
{
    return __DATE__ " " __TIME__;
}
