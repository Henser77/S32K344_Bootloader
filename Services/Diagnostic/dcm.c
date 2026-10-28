/*==================================================================================================
 *  Mcal/dcm.c —— Dcm（UDS 诊断服务层）实现
 *
 *  【职责】
 *  输入：一条完整的 UDS 请求（CanTp 已经拼好）
 *  动作：看懂它是哪个服务、校验会话/安全/参数、必要时去操作 Flash
 *  输出：一条完整的 UDS 响应（交给 CanTp 去分段发送）
 *
 *  【本文件的结构】
 *      ① 时间基准与响应发送
 *      ② 会话 / 安全 两个状态机
 *      ③ 八个服务的处理器（0x10 0x11 0x27 0x31 0x34 0x36 0x37 0x3E）
 *      ④ 服务分发 + 周期处理
 *
 *  【一个贯穿全程的约定：正响应 = 请求 SID + 0x40】
 *      请求 10 03  →  正响应 50 03 ...
 *      请求 34 ... →  正响应 74 ...
 *  这是 ISO 14229-1 的硬性规定，不是我们的习惯。
 *================================================================================================*/

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "dcm.h"
#include "pdur.h"
#include "pdur_cfg.h"
#include "boot_param.h"   /* 升级标志（参数区）*/
#include "crc32.h"        /* 与上电校验共用同一份 CRC32 */
#include "fls.h"
#include "Det.h"

#include <string.h>

/*==================================================================================================
 *                                   LOCAL DEFINES
 *================================================================================================*/
#if (DCM_DEV_ERROR_DETECT == STD_ON)
    #define DCM_DET_ERROR(api, err)                                     \
        ((void)Det_ReportError((uint16)DCM_MODULE_ID,                   \
                               (uint8)DCM_INSTANCE_ID,                  \
                               (uint8)(api),                            \
                               (uint8)(err)))
#else
    #define DCM_DET_ERROR(api, err)     ((void)0)
#endif

#define DCM_API_INIT                (0x01U)
#define DCM_API_MAIN_FUNCTION       (0x02U)
#define DCM_API_RX_INDICATION       (0x03U)
#define DCM_API_GET_VERSION_INFO    (0x04U)

#define DCM_E_PARAM_POINTER         (0x01U)
#define DCM_E_PARAM_LENGTH          (0x02U)
#define DCM_E_UNINIT                (0x03U)

/* Cortex-M7 DWT 周期计数器（和 cantp.c 用同一套办法，理由见那边注释） */
#define DCM_REG_DEMCR               (*(volatile uint32*)0xE000EDFCUL)
#define DCM_REG_DWT_CTRL            (*(volatile uint32*)0xE0001000UL)
#define DCM_REG_DWT_CYCCNT          (*(volatile uint32*)0xE0001004UL)
#define DCM_DEMCR_TRCENA            (0x01000000UL)
#define DCM_DWT_CTRL_CYCCNTENA      (0x00000001UL)
#define DCM_CYCLES_PER_US           (160UL)     /* 160 MHz */

/** @brief 请求里"抑制正响应"位（子功能最高位） */
#define DCM_SUPPRESS_POS_RSP        (0x80U)

/*==================================================================================================
 *                                   LOCAL VARIABLES
 *================================================================================================*/

/*--------------------------- 会话与安全 ---------------------------*/
static Dcm_SessionStateType  Dcm_Session   = DCM_SESSION_STATE_DEFAULT;
static Dcm_SecurityStateType Dcm_Security  = DCM_SECURITY_LOCKED;
static uint32                Dcm_S3TimerUs = 0U;   /**< 上次收到请求的时间（用于 S3 超时） */
static boolean               Dcm_TimeBaseOk = FALSE;

/* 软件扩展的微秒计数器（见 Dcm_GetTimeUs 的说明） */
static uint32                Dcm_LastCycles  = 0U;
static uint32                Dcm_UsRemainder = 0U;
static uint32                Dcm_UsCounter   = 0U;

/*--------------------------- 安全访问 ---------------------------*/
static uint32  Dcm_SecSeed        = 0U;      /**< 本次发出的种子 */
static uint32  Dcm_SecSeedCounter = 0U;      /**< 每次请求种子都变，防止重放 */
static uint8   Dcm_SecAttempts    = 0U;      /**< 连续错误次数 */
static uint32  Dcm_SecLockTimerUs = 0U;      /**< 超限锁定的起点 */

/*--------------------------- 下载上下文 ---------------------------*/
static boolean Dcm_DownloadActive = FALSE;   /**< 0x34 已受理，0x37 还没来 */
static uint32  Dcm_DownloadStart  = 0U;      /**< 目标起始地址 */
static uint32  Dcm_DownloadSize   = 0U;      /**< 声明要写的总长度 */
static uint32  Dcm_DownloadWritten = 0U;     /**< 已经写进去多少字节 */
static uint8   Dcm_DownloadBsc    = 0U;      /**< 下一个期望的块序号 */
static uint32  Dcm_DownloadCrc    = DCM_CRC32_INIT;  /**< 接收过程中累加的 CRC */

/*--------------------------- 响应发送 ---------------------------*/
static uint8   Dcm_TxBuf[DCM_TX_BUF_SIZE];
static uint32  Dcm_TxLen     = 0U;
static boolean Dcm_TxPending = FALSE;

/** @brief 因"上一条响应还没发走"而丢弃的响应条数（正常应恒为 0） */
static uint32  Dcm_RespOverwriteCount = 0U;

/*--------------------------- 统计与诊断 ---------------------------*/
static uint32  Dcm_ReqCount  = 0U;
static uint32  Dcm_NrcCount  = 0U;
static uint32  Dcm_S3TimeoutCount = 0U;
static boolean Dcm_ResetRequested = FALSE;

/* 最近一次 0xFF00 擦除失败时，Fls 层记录的原始状态和扇区号（诊断用） */
static uint8  Dcm_LastFlsStatus = 0U;
static uint32 Dcm_LastFlsSector = 0U;

/** @brief 最近一次擦除例程实际耗时（毫秒） */
static uint32 Dcm_LastEraseMs = 0U;

/* 大缓冲放静态区，避免撑爆栈 */
static uint8   Dcm_CrcBuf[128];

/*==================================================================================================
 *                                  时间基准
 *================================================================================================*/
static void Dcm_InitTimeBase(void)
{
    uint32 first;
    uint32 second;
    uint32 i;

    DCM_REG_DEMCR     |= DCM_DEMCR_TRCENA;
    /*--------------------------------------------------------------------------
     * ★ 不要无条件清零 CYCCNT（2026-10-25 修正）
     *
     * 以前这里直接写：DCM_REG_DWT_CYCCNT = 0U;
     * 但 CanTp_Init() 先执行、已经清过零并记下了它自己的基准 CanTp_LastCycles。
     * 这里再清一次，CanTp 下一次 GetTimeUs 会算：
     *
     *     deltaCyc =（很小的新值）-（清零前的旧基准）
     *              = 无符号减法绕回 ≈ 4.3e9 周期 ≈ 26.8 秒
     *
     * 于是 CanTp_UsCounter 被凭空加了 26.8 秒 —— 任何在途的超时都会立刻到期。
     *
     * 目前不发作：初始化期间没有在途传输，CanTp 的收发计时器都还没置。
     * 但这是地雷：哪天有人在初始化阶段启动传输，就会看到"第一条消息立刻超时"。
     *
     * 现在的做法：只有在计数器还没跑起来时才清零并启动；已经跑着就沿用。
     * 这样两个模块共用同一个时基，谁也不会打乱对方的基准。
     *------------------------------------------------------------------------*/
    if ((DCM_REG_DWT_CTRL & DCM_DWT_CTRL_CYCCNTENA) == 0U)
    {
        DCM_REG_DWT_CYCCNT = 0U;        /* 只有从零开始时才清零 */
    }

    DCM_REG_DWT_CTRL  |= DCM_DWT_CTRL_CYCCNTENA;

    first = DCM_REG_DWT_CYCCNT;
    for (i = 0U; i < 1000U; i++)
    {
        /* 空转，让计数器走过一段 */
    }
    second = DCM_REG_DWT_CYCCNT;

    Dcm_TimeBaseOk = (second != first) ? TRUE : FALSE;

    Dcm_LastCycles  = DCM_REG_DWT_CYCCNT;
    Dcm_UsRemainder = 0U;
    Dcm_UsCounter   = 0U;
}

/**
 * @brief 判断"从 Start 到现在是否真的超过 Limit 微秒"
 *
 * @details 和 cantp.c 里 CanTp_ElapsedExceeds 同一个理由：
 *  直接写 (Now - Start) > Limit 时，只要 Now 比 Start 小一点点，
 *  无符号减法就下溢成 40 多亿 ≈ 71 分钟，任何超时都会瞬间触发。
 *
 *  本工程真踩过这个坑：CanTp 的 N_Cr 超时被误触发，导致连续帧被静默丢弃、
 *  刷写停在第 4 个连续帧。详见 cantp.c 里 CanTp_EnterCritical 的说明。
 *  S3 超时和安全锁定延时用的是同一套时间基准，所以同样加保护。
 */
static boolean Dcm_ElapsedExceeds(uint32 Now, uint32 Start, uint32 Limit)
{
    uint32 elapsed = (uint32)(Now - Start);

    if (elapsed >= 0x80000000UL)
    {
        return FALSE;
    }

    return (elapsed > Limit) ? TRUE : FALSE;
}

/**
 * @brief 取当前时间（微秒，软件扩展成 32 位）
 *
 * @details 和 cantp.c 里 CanTp_GetTimeUs() 同一个道理，那边写了完整说明，
 *          这里只说结论：
 *          直接把 CYCCNT 除以 160 得到的"微秒值"模数是 26.84 秒，
 *          而 (now - start) 的减法模数是 2^32，两者不一致 →
 *          **每跨过 26.84 秒那道坎就会误报一次超时**。
 *          所以必须把周期数累加成真正的 32 位微秒计数器（模 71.6 分钟）。
 */
static uint32 Dcm_GetTimeUs(void)
{
    uint32 cyc;
    uint32 deltaCyc;

    if (Dcm_TimeBaseOk == FALSE)
    {
        return 0U;
    }

    cyc      = DCM_REG_DWT_CYCCNT;
    deltaCyc = cyc - Dcm_LastCycles;        /* 模 2^32，回绕安全 */
    Dcm_LastCycles = cyc;

    Dcm_UsCounter   += deltaCyc / DCM_CYCLES_PER_US;
    Dcm_UsRemainder += deltaCyc % DCM_CYCLES_PER_US;

    if (Dcm_UsRemainder >= DCM_CYCLES_PER_US)
    {
        Dcm_UsCounter++;
        Dcm_UsRemainder -= DCM_CYCLES_PER_US;
    }

    return Dcm_UsCounter;
}

/*==================================================================================================
 *                                  响应发送
 *================================================================================================*/

/** @brief 把一条响应放进发送缓冲（真正的发送在 Dcm_FlushTx 里做） */
static void Dcm_QueueResponse(const uint8* Data, uint32 Len)
{
    if ((Data == NULL_PTR) || (Len == 0U) || (Len > (uint32)DCM_TX_BUF_SIZE))
    {
        return;
    }

    /*--------------------------------------------------------------------------
     * 【必须有的占用检查】
     *
     * 响应缓冲只有一个。如果上一条响应还卡在缓冲里没交出去
     * （CanTp 忙、或多帧响应还在等发送确认），这条新响应会**直接覆盖**它 ——
     * 结果是诊断仪收到一条内容错乱的响应，现场没有任何提示。
     *
     * 触发条件很窄（需要 CanTp 正忙时又来新请求），但一旦发生极难排查。
     * 所以宁可丢掉新响应并计数，让上层能看到 Dcm_GetRespOverwriteCount()。
     *------------------------------------------------------------------------*/
    if (Dcm_TxPending != FALSE)
    {
        Dcm_RespOverwriteCount++;
        DCM_DET_ERROR(DCM_API_RX_INDICATION, DCM_E_PARAM_POINTER);
        return;
    }

    (void)memcpy(Dcm_TxBuf, Data, (size_t)Len);
    Dcm_TxLen     = Len;
    Dcm_TxPending = TRUE;
}

/** @brief 尝试把缓冲里的响应交给 CanTp（失败说明 CanTp 还忙，下次再试） */
static void Dcm_FlushTx(void)
{
    PduInfoType info;

    if (Dcm_TxPending == FALSE)
    {
        return;
    }

    info.SduDataPtr  = Dcm_TxBuf;
    info.SduLength   = (PduLengthType)Dcm_TxLen;
    info.MetaDataPtr = NULL_PTR;

    /* 响应经 PduR 路由到 CanTp（PduR 才知道"这条 PDU 走哪条路"） */
    if (PduR_DcmTransmit((PduIdType)PDUR_DCM_TX_PDU_ID, &info) == E_OK)
    {
        Dcm_TxPending = FALSE;
    }
}

/**
 * @brief 带等待的发送
 *
 * @details 【为什么需要这个函数】
 *  擦除 Flash 之前必须先让 NRC 0x78（请等待）**真的发出去**，
 *  否则擦除期间（几十毫秒到几秒）诊断仪的 P2 超时（50 ms）先到了，
 *  整个刷写会被判失败。
 *
 *  这里忙等一小段时间，期间 CAN 中断照常工作（发完一帧会置 TxConfirmation），
 *  所以 CanTp 很快就能接受下一条发送。
 */
static void Dcm_FlushTxBlocking(void)
{
    uint32 spin = 0U;

    while ((Dcm_TxPending != FALSE) && (spin < DCM_TX_RETRY_LIMIT))
    {
        Dcm_FlushTx();
        spin++;
    }
}

/** @brief 发正响应：buff[0] 应当是"请求 SID + 0x40" */
static void Dcm_SendPositive(const uint8* Data, uint32 Len)
{
    Dcm_QueueResponse(Data, Len);
}

/** @brief 发负响应：7F <SID> <NRC> */
static void Dcm_SendNegative(uint8 Sid, uint8 Nrc)
{
    uint8 resp[3];

    resp[0] = DCM_NEGATIVE_RESPONSE_SID;
    resp[1] = Sid;
    resp[2] = Nrc;

    Dcm_NrcCount++;
    Dcm_QueueResponse(resp, 3U);
}

/*==================================================================================================
 *                                   CRC32（0xFF01 例程用）
 *================================================================================================*/
static uint32 Dcm_Crc32Update(uint32 Crc, const uint8* Data, uint32 Len)
{
    /* 直接转调共享实现 —— 保证和上电校验用的是同一份算法（src/crc32.c）*/
    return Crc32_Update(Crc, Data, Len);
}

/** @brief 读一段 Flash 算 CRC32（用于 0xFF01 校验） */
static uint32 Dcm_Crc32ReadRegion(uint32 Address, uint32 Len, Std_ReturnType* Result)
{
    uint32 crc = DCM_CRC32_INIT;
    uint32 offset = 0U;

    *Result = E_OK;

    while (offset < Len)
    {
        uint32 chunk = Len - offset;

        if (chunk > (uint32)sizeof(Dcm_CrcBuf))
        {
            chunk = (uint32)sizeof(Dcm_CrcBuf);
        }

        if (Fls_Read(Address + offset, Dcm_CrcBuf, chunk) != E_OK)
        {
            *Result = E_NOT_OK;
            return 0U;
        }

        crc    = Dcm_Crc32Update(crc, Dcm_CrcBuf, chunk);
        offset += chunk;
    }

    return crc ^ DCM_CRC32_FINAL_XOR;
}

/*==================================================================================================
 *                                  会话与安全
 *================================================================================================*/
static void Dcm_EnterSession(Dcm_SessionStateType NewSession)
{
    Dcm_Session = NewSession;

    /* ISO 14229-1 规定：回到默认会话时必须把安全等级复位。
     * 其它会话切换保留安全状态 —— 这样"扩展会话里解锁 → 切到编程会话"
     * 这个常见流程才走得通。 */
    if (NewSession == DCM_SESSION_STATE_DEFAULT)
    {
        Dcm_Security      = DCM_SECURITY_LOCKED;
        Dcm_DownloadActive = FALSE;
    }
}

/** @brief 恢复默认会话（S3 超时、复位等场景调用） */
static void Dcm_BackToDefault(void)
{
    Dcm_EnterSession(DCM_SESSION_STATE_DEFAULT);
}

/** @brief 当前会话是否满足某个服务的要求 */
static boolean Dcm_SessionInProgramming(void)
{
    return (Dcm_Session == DCM_SESSION_STATE_PROGRAMMING) ? TRUE : FALSE;
}

/** @brief 安全访问的算法：种子 -> 密钥
 *
 *  @details 【这个算法不安全，是刻意简化的】
 *
 *  它只是一个"确定可复现的变换"，用来把 0x27 服务的**流程**走通：
 *      种子 -> (seed ^ XOR) + ADD -> 密钥
 *
 *  **它提供的不是安全性，只是"仪式"**，原因有三：
 *      1. 公式固定且公开 -> 拿到任意一组 (seed,key) 就能反推出 XOR/ADD
 *      2. 只要知道公式，任何人在没有密钥的情况下也能自己算
 *      3. 种子用计数器生成（Dcm_SecSeedCounter），每次上电序列都一样，可预测
 *
 *  真实产品应该：
 *      - 用 AES-128-CMAC 之类，密钥存在 MCU 的安全区（HSE/SHE）
 *      - 种子来自硬件随机数，用后即废
 *      - 配合 SecOC 做报文认证，防止总线上重放
 *      - 失败次数锁定 + 延时（本工程已有 NRC 0x36/0x37，这部分是对的）
 *
 *  写在这里是为了让后来的人**知道这是缺口**，而不是误以为它安全。
 */
static uint32 Dcm_CalcKey(uint32 Seed)
{
    return (uint32)(((Seed ^ DCM_SEC_KEY_XOR) + DCM_SEC_KEY_ADD) & 0xFFFFFFFFUL);
}

/*==================================================================================================
 *                                  服务 0x10：切换会话
 *================================================================================================*/
static void Dcm_HandleSessionControl(const uint8* Req, uint32 Len)
{
    uint8 sub;
    uint8 resp[6];

    if (Len < 2U)
    {
        Dcm_SendNegative(DCM_SID_DIAGNOSTIC_SESSION_CONTROL, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    sub = (uint8)(Req[1] & (uint8)(~DCM_SUPPRESS_POS_RSP));

    switch (sub)
    {
        case DCM_SESSION_DEFAULT:
            Dcm_EnterSession(DCM_SESSION_STATE_DEFAULT);
            break;

        case DCM_SESSION_PROGRAMMING:
            Dcm_EnterSession(DCM_SESSION_STATE_PROGRAMMING);
            break;

        case DCM_SESSION_EXTENDED:
            Dcm_EnterSession(DCM_SESSION_STATE_EXTENDED);
            break;

        default:
            Dcm_SendNegative(DCM_SID_DIAGNOSTIC_SESSION_CONTROL,
                             DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED);
            return;
    }

    Dcm_S3TimerUs = Dcm_GetTimeUs();

    /* 响应里必须告诉诊断仪两个定时参数：
     *   P2   占 2 字节，单位毫秒
     *   P2*  占 2 字节，单位 10 毫秒 */
    resp[0] = (uint8)(DCM_SID_DIAGNOSTIC_SESSION_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
    resp[1] = sub;
    resp[2] = (uint8)((DCM_P2_MS >> 8) & 0xFFU);
    resp[3] = (uint8)(DCM_P2_MS & 0xFFU);
    resp[4] = (uint8)(((DCM_P2_STAR_MS / 10U) >> 8) & 0xFFU);
    resp[5] = (uint8)((DCM_P2_STAR_MS / 10U) & 0xFFU);

    Dcm_SendPositive(resp, 6U);
}

/*==================================================================================================
 *                                  服务 0x11：ECU 复位
 *================================================================================================*/
static void Dcm_HandleEcuReset(const uint8* Req, uint32 Len)
{
    uint8 sub;
    uint8 resp[2];

    if (Len < 2U)
    {
        Dcm_SendNegative(DCM_SID_ECU_RESET, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    sub = (uint8)(Req[1] & (uint8)(~DCM_SUPPRESS_POS_RSP));

    if (sub != 0x01U)
    {
        /* 只支持 0x01 = 硬复位 */
        Dcm_SendNegative(DCM_SID_ECU_RESET, DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return;
    }

    resp[0] = (uint8)(DCM_SID_ECU_RESET + DCM_POSITIVE_RESPONSE_OFFSET);
    resp[1] = sub;
    Dcm_SendPositive(resp, 2U);

    /* 复位动作不在这里做 —— 要等响应真的发出去之后。
     * 主循环看到这个标志再执行复位，见 Dcm_IsResetRequested()。 */
    Dcm_ResetRequested = TRUE;
}

/*==================================================================================================
 *                                  服务 0x27：安全访问
 *================================================================================================*/
static void Dcm_HandleSecurityAccess(const uint8* Req, uint32 Len)
{
    uint8  sub;
    uint32 now = Dcm_GetTimeUs();

    if (Len < 2U)
    {
        Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    sub = (uint8)(Req[1] & (uint8)(~DCM_SUPPRESS_POS_RSP));

    /*--------------------------------------------------------------------------
     * 0. 超限锁定的处理（在检查子功能之前）
     *
     * 连续错 3 次 → 锁 10 秒，期间任何请求都回 NRC 0x37。
     * 这是防暴力猜密钥，ISO 14229-1 要求的行为。
     *------------------------------------------------------------------------*/
    if (Dcm_SecAttempts >= DCM_SEC_MAX_ATTEMPTS)
    {
        if (Dcm_ElapsedExceeds(now, Dcm_SecLockTimerUs, DCM_SEC_DELAY_US) == FALSE)
        {
            Dcm_SendNegative(DCM_SID_SECURITY_ACCESS,
                             DCM_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED);
            return;
        }

        /* 延时到期，重新给机会 */
        Dcm_SecAttempts = 0U;
    }

    /*--------------------------------------------------------------------------
     * 1. 子功能 0x01 —— 请求种子
     *------------------------------------------------------------------------*/
    if (sub == DCM_SEC_LEVEL1_SEED_SUB)
    {
        uint8 resp[6];

        if (Dcm_Security == DCM_SECURITY_UNLOCKED)
        {
            /* 已解锁时再请求种子，标准允许回全 0 表示"不用再解了" */
            resp[2] = 0U; resp[3] = 0U; resp[4] = 0U; resp[5] = 0U;
        }
        else if (Dcm_Security == DCM_SECURITY_SEED_SENT)
        {
            /*
             * 已经发过种子、还没收到密钥 —— **重复请求就返回同一个种子**。
             *
             * 【为什么这样改】
             * 原来的实现是"每次 27 01 都换新种子"。这符合"防重放"的意图，
             * 但造成了一个实际的使用陷阱：
             *     手工点两次 27 01 → 生成了两个种子 → 串口打印两行不同的密钥
             *     → 人拿着第一次的密钥去回，结果对不上（因为种子已经换了）
             * 实测就踩过这个坑：发了 0xD5C56315 却以为是"密钥错"，
             * 其实那是第二个种子的正确密钥，只是当时手上拿的是第一个的。
             *
             * 现在改成：在一次"请求种子 → 发送密钥"的窗口内，种子保持不变。
             * 安全性没有被削弱 —— 因为种子在一个解锁尝试窗口内本来就只该用一次，
             * 而真正防重放靠的是"每次解锁成功后下一次请求一定换新种子"。
             */
            resp[2] = (uint8)((Dcm_SecSeed >> 24) & 0xFFU);
            resp[3] = (uint8)((Dcm_SecSeed >> 16) & 0xFFU);
            resp[4] = (uint8)((Dcm_SecSeed >> 8)  & 0xFFU);
            resp[5] = (uint8)((Dcm_SecSeed)       & 0xFFU);
        }
        else
        {
            /* 每次都换一个值，防止重放攻击 */
            Dcm_SecSeedCounter++;
            Dcm_SecSeed = (uint32)((Dcm_SecSeedCounter * 0x9E3779B1UL) ^ 0xA5A5A5A5UL);

            /* 0 是保留值（表示"已解锁"），不能用作种子 */
            if (Dcm_SecSeed == 0U)
            {
                Dcm_SecSeed = 0x00000001UL;
            }

            resp[2] = (uint8)((Dcm_SecSeed >> 24) & 0xFFU);
            resp[3] = (uint8)((Dcm_SecSeed >> 16) & 0xFFU);
            resp[4] = (uint8)((Dcm_SecSeed >> 8)  & 0xFFU);
            resp[5] = (uint8)((Dcm_SecSeed)       & 0xFFU);

            Dcm_Security = DCM_SECURITY_SEED_SENT;
        }

        resp[0] = (uint8)(DCM_SID_SECURITY_ACCESS + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        Dcm_SendPositive(resp, 6U);
        return;
    }

    /*--------------------------------------------------------------------------
     * 2. 子功能 0x02 —— 发送密钥
     *------------------------------------------------------------------------*/
    if (sub == DCM_SEC_LEVEL1_KEY_SUB)
    {
        uint8  resp[2];
        uint32 key;

        /* 没先要种子就发密钥 → 顺序错 */
        if (Dcm_Security != DCM_SECURITY_SEED_SENT)
        {
            Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_REQUEST_SEQUENCE_ERROR);
            return;
        }

        /* 密钥必须正好 4 字节 */
        if (Len != 6U)
        {
            Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
            return;
        }

        key = ((uint32)Req[2] << 24) | ((uint32)Req[3] << 16) |
              ((uint32)Req[4] << 8)  |  (uint32)Req[5];

        if (key == Dcm_CalcKey(Dcm_SecSeed))
        {
            Dcm_Security    = DCM_SECURITY_UNLOCKED;
            Dcm_SecAttempts = 0U;

            resp[0] = (uint8)(DCM_SID_SECURITY_ACCESS + DCM_POSITIVE_RESPONSE_OFFSET);
            resp[1] = sub;
            Dcm_SendPositive(resp, 2U);
        }
        else
        {
            Dcm_SecAttempts++;
            Dcm_Security = DCM_SECURITY_LOCKED;

            if (Dcm_SecAttempts >= DCM_SEC_MAX_ATTEMPTS)
            {
                /* 刚好用完次数：开始锁 10 秒，并按标准回 0x36 */
                Dcm_SecLockTimerUs = now;
                Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_EXCEED_NUMBER_OF_ATTEMPTS);
            }
            else
            {
                Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_INVALID_KEY);
            }
        }
        return;
    }

    Dcm_SendNegative(DCM_SID_SECURITY_ACCESS, DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

/*==================================================================================================
 *                                  服务 0x3E：心跳
 *================================================================================================*/
static void Dcm_HandleTesterPresent(const uint8* Req, uint32 Len)
{
    uint8 sub;
    uint8 resp[2];

    if (Len < 2U)
    {
        Dcm_SendNegative(DCM_SID_TESTER_PRESENT, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    /* 先记下"诊断仪还活着"，S3 计时重置 */
    Dcm_S3TimerUs = Dcm_GetTimeUs();

    sub = (uint8)(Req[1] & (uint8)(~DCM_SUPPRESS_POS_RSP));

    if (sub != 0x00U)
    {
        Dcm_SendNegative(DCM_SID_TESTER_PRESENT, DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return;
    }

    /* 如果诊断仪要求"抑制正响应"，就什么都不回（这是标准允许的省流量做法） */
    if ((Req[1] & DCM_SUPPRESS_POS_RSP) != 0U)
    {
        return;
    }

    resp[0] = (uint8)(DCM_SID_TESTER_PRESENT + DCM_POSITIVE_RESPONSE_OFFSET);
    resp[1] = sub;
    Dcm_SendPositive(resp, 2U);
}

/*==================================================================================================
 *                                  服务 0x31：例程控制
 *================================================================================================*/
static void Dcm_HandleRoutineControl(const uint8* Req, uint32 Len)
{
    uint8  sub;
    uint16 rid;

    if (Len < 4U)
    {
        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    sub = (uint8)(Req[1] & (uint8)(~DCM_SUPPRESS_POS_RSP));
    rid = (uint16)(((uint16)Req[2] << 8) | (uint16)Req[3]);

    /* 只有"启动例程"（0x01）有意义，停止/查结果不支持 */
    if (sub != DCM_ROUTINE_START)
    {
        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return;
    }

    /*--------------------------------------------------------------------------
     * 擦除和校验都会改动 Flash，必须同时满足：
     *   ① 处于编程会话
     *   ② 已通过安全解锁
     * 少任何一条都回 NRC 0x33 / 0x7F，这就是"两道门"的实际作用。
     *------------------------------------------------------------------------*/
    if (Dcm_SessionInProgramming() == FALSE)
    {
        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);
        return;
    }

    if (Dcm_Security != DCM_SECURITY_UNLOCKED)
    {
        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_SECURITY_ACCESS_DENIED);
        return;
    }

    /*==========================================================================
     * 例程 0xFF00 —— 擦除 App 区
     *
     * 【这里有本模块最重要的一个细节】
     * 擦 1 MB 需要好几秒，而诊断仪的 P2 超时只有 50 ms。
     * 所以必须**先回 NRC 0x78（请等待）**，再去擦。
     * 诊断仪收到 0x78 后会把超时延长到 P2*（5 秒）。
     *
     * 顺序反了 = 诊断仪先超时 = 整个刷写失败。
     *========================================================================*/
    if (rid == DCM_RID_ERASE_MEMORY)
    {
        uint8        resp[5];
        Std_ReturnType r;

        /* ① 先回"请等待"，并且**等它真的发出去** */
        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_RESPONSE_PENDING);
        Dcm_FlushTxBlocking();

        /* ② 再执行擦除（阻塞几秒）—— 顺便计时，证明真的擦了
         *
         * 【为什么要计时】
         * 擦 1 MB（128 个 8KB 扇区）应该有明显耗时（通常 2~10 秒）。
         * 如果耗时接近 0 却返回"成功"，说明循环没真正执行 ——
         * 那种"假成功"会让后面写数据时莫名其妙失败。
         * 这个数字是唯一能证明"真的擦了 1 MB"的客观证据。 */
        /* ★ 擦除之前必须先把参数区标记成"App 无效"。
         *
         * 【为什么顺序不能反 —— 这是 Bootloader 最重要的一条纪律】
         * 擦到一半断电时：
         *   先置无效 → 上电看到"无效" → 留在 Bootloader → 可以重刷   ✅
         *   先擦后置 → 上电看到旧账本说"有效" → 跳进半擦半留的 App → 跑飞 ❌
         *
         * 参数区只占一个扇区，擦+写约 3.4ms，相对擦 1MB 可以忽略。 */
        if (BootParam_SetInvalid() != E_OK)
        {
            /* 连"记账"都失败 → 不敢往下擦，直接报编程失败 */
            Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_GENERAL_PROGRAMMING_FAILURE);
            return;
        }

        {
            uint32 eraseStartUs = Dcm_GetTimeUs();

            r = Fls_Erase(DCM_DOWNLOAD_START_ADDR, DCM_DOWNLOAD_MAX_SIZE);

            Dcm_LastEraseMs = (uint32)((Dcm_GetTimeUs() - eraseStartUs) / 1000UL);
        }

        /* 把 Fls 层的失败细节留下来 —— 否则只回一个"失败"状态字节，
         * 根本不知道是参数错、保护区、解锁失败还是硬件超时。 */
        Dcm_LastFlsStatus = g_FlsLastStatus;
        Dcm_LastFlsSector = g_FlsLastSector;

        /* ③ 最后回正响应，状态字节告诉诊断仪成功与否 */
        resp[0] = (uint8)(DCM_SID_ROUTINE_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8)((rid >> 8) & 0xFFU);
        resp[3] = (uint8)(rid & 0xFFU);
        resp[4] = (r == E_OK) ? DCM_ROUTINE_STATUS_OK : DCM_ROUTINE_STATUS_FAILED;
        Dcm_SendPositive(resp, 5U);
        return;
    }

    /*==========================================================================
     * 例程 0xFF01 —— 校验（算 CRC32）
     *
     * 把"接收过程中累加的 CRC"和"从 Flash 读回来现算的 CRC"比对。
     * 一致说明写进去的数据和收到的完全一样。
     *
     * ⚠️ 已知限制：C40 Flash 在读缓冲未更新时，刚写完立刻读回可能读到旧值
     *    （详见 fls.h 里 Fls_Read 的说明）。所以这一步可能出现"数据其实是对的
     *    但校验报错"。权威的校验放在**下次上电**做。
     *========================================================================*/
    if (rid == DCM_RID_CHECK_INTEGRITY)
    {
        uint8          resp[9];
        uint32         crcRead;
        Std_ReturnType rdResult = E_NOT_OK;
        boolean        ok = FALSE;

        if (Dcm_DownloadWritten == 0U)
        {
            Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_REQUEST_SEQUENCE_ERROR);
            return;
        }

        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_RESPONSE_PENDING);
        Dcm_FlushTxBlocking();

        crcRead = Dcm_Crc32ReadRegion(Dcm_DownloadStart, Dcm_DownloadWritten, &rdResult);

        if (rdResult == E_OK)
        {
            ok = (crcRead == (Dcm_DownloadCrc ^ DCM_CRC32_FINAL_XOR)) ? TRUE : FALSE;
        }

        /* ★ 验货通过才签收：CRC 一致时把"这份 App 有效"记进参数区
         *
         * 【为什么记账挂在这里，而不是挂在 0x37（结束传输）】
         *   0x37 只代表"我传完了"，不代表"传对了"。
         *   只有这一步的 CRC 比对才是真正的验货 —— 它验证的是
         *   "Flash 里的内容 == 诊断仪发过来的内容"。
         *   没验货就签收，等于把"刷一半"的固件当成好的。
         *
         * 【为什么要检查 Dcm_DownloadStart】
         *   参数区里记的 AppStart 固定是 0x00500000（App 区起点）。
         *   如果诊断仪把固件下到 App 区中间的某个地址，上电校验时会从
         *   0x00500000 开始算 CRC —— 那就对不上了。所以这种非常规下载
         *   不记账（保持"无效"，上电会留在 Bootloader，是安全的）。 */
        if ((ok != FALSE) &&
            (Dcm_DownloadStart == (uint32)DCM_DOWNLOAD_START_ADDR))
        {
            if (BootParam_SetValid(Dcm_DownloadWritten, crcRead) != E_OK)
            {
                /* 记账失败 → 虽然数据是对的，但不告诉上电"能跳" → 留在 Bootloader */
                ok = FALSE;
            }
        }
        else if ((ok != FALSE) &&
                 (Dcm_DownloadStart != (uint32)DCM_DOWNLOAD_START_ADDR))
        {
            /* 数据对，但下载地址不是 App 区起点 → 不记账 */
            ok = FALSE;
        }

        resp[0] = (uint8)(DCM_SID_ROUTINE_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8)((rid >> 8) & 0xFFU);
        resp[3] = (uint8)(rid & 0xFFU);
        resp[4] = (ok != FALSE) ? DCM_ROUTINE_STATUS_OK : DCM_ROUTINE_STATUS_FAILED;
        resp[5] = (uint8)((crcRead >> 24) & 0xFFU);
        resp[6] = (uint8)((crcRead >> 16) & 0xFFU);
        resp[7] = (uint8)((crcRead >> 8)  & 0xFFU);
        resp[8] = (uint8)(crcRead & 0xFFU);
        Dcm_SendPositive(resp, 9U);
        return;
    }

    /*==========================================================================
     * 例程 0xFF03 —— 请求"下次上电留在 Bootloader"
     *
     * 【解决什么问题 —— 2026-10-25 实测踩到】
     *   发布版把启动窗口设成 0（上电直接跳 App，启动最快）。
     *   结果 App 是好的时候，软件上**再也没有办法进 Bootloader** 了 ——
     *   上位机 --wait 连不上，断电重上电也连不上，只能上 J-Link 救砖。
     *
     *   这个例程就是第二条入口：
     *       31 01 FF 03   -> 往参数区账本里写 BootRequest = 1
     *       11 01         -> 复位
     *       上电          -> Bootloader 看到请求 -> 清掉 -> 留在 Bootloader
     *
     * 【为什么写在 Flash 而不是 RAM】
     *   要扛得住断电。"我要刷固件"这个意图不能因为掉电就丢了。
     *   代价是擦写一个扇区（约 3.4 ms），可以接受。
     *========================================================================*/
    if (rid == DCM_RID_REQUEST_BOOT)
    {
        uint8          resp[5];
        Std_ReturnType r;

        r = BootParam_SetBootRequest();

        resp[0] = (uint8)(DCM_SID_ROUTINE_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8)((rid >> 8) & 0xFFU);
        resp[3] = (uint8)(rid & 0xFFU);
        resp[4] = (r == E_OK) ? DCM_ROUTINE_STATUS_OK : DCM_ROUTINE_STATUS_FAILED;
        Dcm_SendPositive(resp, 5U);
        return;
    }

    /*==========================================================================
     * 例程 0xFF04 —— 取消"下次上电留在 Bootloader"的请求
     *========================================================================*/
    if (rid == DCM_RID_CLEAR_BOOT_REQUEST)
    {
        uint8          resp[5];
        Std_ReturnType r;

        r = BootParam_ClearBootRequest();

        resp[0] = (uint8)(DCM_SID_ROUTINE_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8)((rid >> 8) & 0xFFU);
        resp[3] = (uint8)(rid & 0xFFU);
        resp[4] = (r == E_OK) ? DCM_ROUTINE_STATUS_OK : DCM_ROUTINE_STATUS_FAILED;
        Dcm_SendPositive(resp, 5U);
        return;
    }

    /*==========================================================================
     * 例程 0xFF02 —— 信任当前 App 区（**调试专用**）
     *
     * 【它解决什么问题】
     * Bootloader 上电只认参数区里那份账本。如果用调试器直接把 App 烧进
     * 0x00500000，参数区还是空的 —— 上电会一直留在 Bootloader，永远跳不过去。
     *
     * 这个例程就是"事后认账"：算一遍 App 区的 CRC32 写进参数区，
     * 让下一次上电能够跳转。用来**验证跳转功能**，不参与真实刷写流程。
     *
     * 【请求格式】
     *      31 01 FF 02 <长度 4 字节，大端>
     *   例：31 01 FF 02 00 00 80 00   → 认账 0x00008000 = 32KB
     *
     * 【响应格式】
     *      71 01 FF 02 <状态> <CRC 4 字节>
     *
     * 【⚠️ 正式发布必须关掉】
     * 它会"不经验货就签收任意内容"。虽然仍要求编程会话 + 安全解锁，
     * 但这道门本来就是为了调试方便而开的。发布前把 dcm_cfg.h 里的
     * DCM_ENABLE_TRUST_APP 改成 STD_OFF。
     *========================================================================*/
#if (DCM_ENABLE_TRUST_APP == STD_ON)
    if (rid == DCM_RID_TRUST_APP_AREA)
    {
        uint8          resp[9];
        uint32         len;
        uint32         crcNow = 0U;
        Std_ReturnType rdResult = E_NOT_OK;

        /* 请求里必须带 4 字节长度：31 01 FF 02 + 4 = 8 字节 */
        if (Len < 8U)
        {
            Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
            return;
        }

        len = ((uint32)Req[4] << 24) | ((uint32)Req[5] << 16) |
              ((uint32)Req[6] << 8)  | (uint32)Req[7];

        /* 长度必须落在 App 区内 */
        if ((len == 0U) || (len > (uint32)DCM_DOWNLOAD_MAX_SIZE))
        {
            Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_REQUEST_OUT_OF_RANGE);
            return;
        }

        Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_RESPONSE_PENDING);
        Dcm_FlushTxBlocking();

        crcNow = Dcm_Crc32ReadRegion(DCM_DOWNLOAD_START_ADDR, len, &rdResult);

        if (rdResult == E_OK)
        {
            if (BootParam_SetValid(len, crcNow) != E_OK)
            {
                rdResult = E_NOT_OK;
            }
        }

        resp[0] = (uint8)(DCM_SID_ROUTINE_CONTROL + DCM_POSITIVE_RESPONSE_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8)((rid >> 8) & 0xFFU);
        resp[3] = (uint8)(rid & 0xFFU);
        resp[4] = (rdResult == E_OK) ? DCM_ROUTINE_STATUS_OK : DCM_ROUTINE_STATUS_FAILED;
        resp[5] = (uint8)((crcNow >> 24) & 0xFFU);
        resp[6] = (uint8)((crcNow >> 16) & 0xFFU);
        resp[7] = (uint8)((crcNow >> 8)  & 0xFFU);
        resp[8] = (uint8)(crcNow & 0xFFU);
        Dcm_SendPositive(resp, 9U);
        return;
    }
#endif /* DCM_ENABLE_TRUST_APP */

    Dcm_SendNegative(DCM_SID_ROUTINE_CONTROL, DCM_NRC_REQUEST_OUT_OF_RANGE);
}

/*==================================================================================================
 *                                  服务 0x34：请求下载
 *================================================================================================*/
static void Dcm_HandleRequestDownload(const uint8* Req, uint32 Len)
{
    uint8  dataFormatId;
    uint8  alfid;
    uint32 address;
    uint32 size;
    uint8  resp[4];

    if (Dcm_SessionInProgramming() == FALSE)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);
        return;
    }

    if (Dcm_Security != DCM_SECURITY_UNLOCKED)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_SECURITY_ACCESS_DENIED);
        return;
    }

    /* 34 <dataFormatId> <alfid> <addr 4> <size 4> = 正好 11 字节 */
    if (Len != 11U)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    dataFormatId = Req[1];
    alfid        = Req[2];

    /* 不支持压缩和加密（真实项目可能会用） */
    if (dataFormatId != 0x00U)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    /* 只支持"地址 4 字节 + 长度 4 字节" */
    if (alfid != DCM_ADDR_LEN_FORMAT_ID)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    address = ((uint32)Req[3] << 24) | ((uint32)Req[4] << 16) |
              ((uint32)Req[5] << 8)  |  (uint32)Req[6];
    size    = ((uint32)Req[7] << 24) | ((uint32)Req[8] << 16) |
              ((uint32)Req[9] << 8)  |  (uint32)Req[10];

    /*--------------------------------------------------------------------------
     * 目标区域校验 —— **这是保护 Bootloader 自己的第一道门**
     *
     * 只允许写 [DCM_DOWNLOAD_START_ADDR, +DCM_DOWNLOAD_MAX_SIZE)。
     * 地址越界、长度为 0、或者跨界，一律拒绝（NRC 0x31）。
     * Fls 层还有第二道门（拒绝 BLOCK_0），这里是双保险。
     *------------------------------------------------------------------------*/
    if ((size == 0U) ||
        (address < DCM_DOWNLOAD_START_ADDR) ||
        (address > (DCM_DOWNLOAD_START_ADDR + DCM_DOWNLOAD_MAX_SIZE)) ||
        (size > (DCM_DOWNLOAD_MAX_SIZE - (address - DCM_DOWNLOAD_START_ADDR))))
    {
        Dcm_SendNegative(DCM_SID_REQUEST_DOWNLOAD, DCM_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    Dcm_DownloadActive  = TRUE;
    Dcm_DownloadStart   = address;
    Dcm_DownloadSize    = size;
    Dcm_DownloadWritten = 0U;
    Dcm_DownloadBsc     = 1U;                       /* 块序号从 1 开始 */
    Dcm_DownloadCrc     = DCM_CRC32_INIT;

    /* 响应：74 <lengthFormat=0x20> <maxBlockLength 2字节>
     *   0x20 表示后面跟 2 字节长度；0x0402 = 1026 字节 */
    resp[0] = (uint8)(DCM_SID_REQUEST_DOWNLOAD + DCM_POSITIVE_RESPONSE_OFFSET);
    resp[1] = 0x20U;
    resp[2] = (uint8)((DCM_MAX_BLOCK_LENGTH >> 8) & 0xFFU);
    resp[3] = (uint8)(DCM_MAX_BLOCK_LENGTH & 0xFFU);
    Dcm_SendPositive(resp, 4U);
}

/*==================================================================================================
 *                                  服务 0x36：传数据
 *================================================================================================*/
static void Dcm_HandleTransferData(const uint8* Req, uint32 Len)
{
    uint8          bsc;
    uint32         dataLen;
    uint8          resp[2];
    Std_ReturnType r;

    if (Dcm_DownloadActive == FALSE)
    {
        /* 没先 0x34 就想传数据 */
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    if (Len < 2U)
    {
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    bsc     = Req[1];
    dataLen = Len - 2U;

    /* 块序号必须是期望值，否则说明中间丢了整条消息 → 顺序错 */
    if (bsc != Dcm_DownloadBsc)
    {
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    if (dataLen == 0U)
    {
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_INCORRECT_LENGTH_OR_FORMAT);
        return;
    }

    /* 不能超过 0x34 里声明的总长度 */
    if ((Dcm_DownloadWritten + dataLen) > Dcm_DownloadSize)
    {
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    /*--------------------------------------------------------------------------
     * 写 Flash
     *
     * 【为什么这里不需要回 NRC 0x78】
     * 一次 1 KB 的编程大约是毫秒级，远小于 P2=50 ms。
     * 只有"擦除"那种几秒级的操作才必须先回 0x78。
     *------------------------------------------------------------------------*/
    r = Fls_Write(Dcm_DownloadStart + Dcm_DownloadWritten, &Req[2], dataLen);

    if (r != E_OK)
    {
        Dcm_SendNegative(DCM_SID_TRANSFER_DATA, DCM_NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    /* 累加 CRC（用于最后的 0xFF01 校验） */
    Dcm_DownloadCrc = Dcm_Crc32Update(Dcm_DownloadCrc, &Req[2], dataLen);

    Dcm_DownloadWritten += dataLen;

    /* 块序号递增：1,2,...,0xFF,0x00,0x01,... 自然回绕 */
    Dcm_DownloadBsc = (uint8)(Dcm_DownloadBsc + 1U);

    resp[0] = (uint8)(DCM_SID_TRANSFER_DATA + DCM_POSITIVE_RESPONSE_OFFSET);
    resp[1] = bsc;
    Dcm_SendPositive(resp, 2U);
}

/*==================================================================================================
 *                                  服务 0x37：传输结束
 *================================================================================================*/
static void Dcm_HandleTransferExit(const uint8* Req, uint32 Len)
{
    uint8 resp[1];

    (void)Req;
    (void)Len;

    if (Dcm_DownloadActive == FALSE)
    {
        Dcm_SendNegative(DCM_SID_REQUEST_TRANSFER_EXIT, DCM_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    Dcm_RespOverwriteCount = 0U;

    Dcm_DownloadActive = FALSE;

    resp[0] = (uint8)(DCM_SID_REQUEST_TRANSFER_EXIT + DCM_POSITIVE_RESPONSE_OFFSET);
    Dcm_SendPositive(resp, 1U);
}

/*==================================================================================================
 *                                  服务分发
 *================================================================================================*/
Std_ReturnType Dcm_RxIndication(const uint8* ReqData, uint32 ReqLen)
{
    uint8 sid;

    if ((ReqData == NULL_PTR) || (ReqLen == 0U))
    {
        DCM_DET_ERROR(DCM_API_RX_INDICATION, DCM_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    Dcm_ReqCount++;

    /* 收到任何请求都算"诊断仪还活着"，刷新 S3 计时 */
    Dcm_S3TimerUs = Dcm_GetTimeUs();

    sid = ReqData[0];

    switch (sid)
    {
        case DCM_SID_DIAGNOSTIC_SESSION_CONTROL:
            Dcm_HandleSessionControl(ReqData, ReqLen);
            break;

        case DCM_SID_ECU_RESET:
            Dcm_HandleEcuReset(ReqData, ReqLen);
            break;

        case DCM_SID_SECURITY_ACCESS:
            Dcm_HandleSecurityAccess(ReqData, ReqLen);
            break;

        case DCM_SID_TESTER_PRESENT:
            Dcm_HandleTesterPresent(ReqData, ReqLen);
            break;

        case DCM_SID_ROUTINE_CONTROL:
            Dcm_HandleRoutineControl(ReqData, ReqLen);
            break;

        case DCM_SID_REQUEST_DOWNLOAD:
            Dcm_HandleRequestDownload(ReqData, ReqLen);
            break;

        case DCM_SID_TRANSFER_DATA:
            Dcm_HandleTransferData(ReqData, ReqLen);
            break;

        case DCM_SID_REQUEST_TRANSFER_EXIT:
            Dcm_HandleTransferExit(ReqData, ReqLen);
            break;

        default:
            Dcm_SendNegative(sid, DCM_NRC_SERVICE_NOT_SUPPORTED);
            break;
    }

    /* 大部分响应能立刻发出去；发不出去（CanTp 忙）就留给 MainFunction */
    Dcm_FlushTx();

    return E_OK;
}

/*==================================================================================================
 *                                  Dcm_Init / Dcm_MainFunction
 *================================================================================================*/
void Dcm_Init(const Dcm_ConfigType* ConfigPtr)
{
    (void)ConfigPtr;

    Dcm_Session        = DCM_SESSION_STATE_DEFAULT;
    Dcm_Security       = DCM_SECURITY_LOCKED;
    Dcm_S3TimerUs      = 0U;

    Dcm_SecSeed        = 0U;
    Dcm_SecSeedCounter = 0U;
    Dcm_SecAttempts    = 0U;
    Dcm_SecLockTimerUs = 0U;

    Dcm_DownloadActive  = FALSE;
    Dcm_DownloadStart   = 0U;
    Dcm_DownloadSize    = 0U;
    Dcm_DownloadWritten = 0U;
    Dcm_DownloadBsc     = 0U;
    Dcm_DownloadCrc     = DCM_CRC32_INIT;

    Dcm_TxLen          = 0U;
    Dcm_TxPending      = FALSE;

    Dcm_ReqCount       = 0U;
    Dcm_NrcCount       = 0U;
    Dcm_S3TimeoutCount = 0U;
    Dcm_ResetRequested = FALSE;

    Dcm_InitTimeBase();

    Dcm_S3TimerUs = Dcm_GetTimeUs();
}

void Dcm_MainFunction(void)
{
    uint32 now = Dcm_GetTimeUs();

    /*==========================================================================
     * 1. 把还没发出去的响应推给 CanTp
     *
     * Dcm_RxIndication 里已经试过一次；这里再兜一次是必要的 ——
     * 因为 CanTp 可能因为"上一帧还在等发送确认"而暂时拒绝。
     *========================================================================*/
    Dcm_FlushTx();

    /*==========================================================================
     * 2. S3 超时 —— 会话保持
     *
     * 诊断仪必须定期发 0x3E（心跳）或其它请求。
     * 超过 S3（5 秒）没有任何请求 → 自动退回默认会话并锁定安全访问。
     *
     * 【为什么必须有这个】
     * 否则诊断仪拔掉线之后，ECU 会一直停在"编程会话 + 已解锁"状态 ——
     * 等于门一直开着。这是安全要求，不是可选项。
     *========================================================================*/
    if (Dcm_Session != DCM_SESSION_STATE_DEFAULT)
    {
        if (Dcm_ElapsedExceeds(now, Dcm_S3TimerUs, DCM_S3_US) != FALSE)
        {
            Dcm_BackToDefault();
            Dcm_S3TimeoutCount++;       /* 让上层能把这个事件报出来 */
        }
    }
}

/*==================================================================================================
 *                                  查询接口
 *================================================================================================*/
Dcm_SessionStateType Dcm_GetSession(void)
{
    return Dcm_Session;
}

Dcm_SecurityStateType Dcm_GetSecurity(void)
{
    return Dcm_Security;
}

uint32 Dcm_GetRequestCount(void)
{
    return Dcm_ReqCount;
}

uint32 Dcm_GetRespOverwriteCount(void)
{
    return Dcm_RespOverwriteCount;
}

void Dcm_ClearResetRequested(void)
{
    Dcm_ResetRequested = FALSE;
}

uint32 Dcm_GetNegativeResponseCount(void)
{
    return Dcm_NrcCount;
}

uint8 Dcm_GetLastFlsStatus(void)
{
    return Dcm_LastFlsStatus;
}

uint32 Dcm_GetLastEraseMs(void)
{
    return Dcm_LastEraseMs;
}

uint32 Dcm_GetLastFlsSector(void)
{
    return Dcm_LastFlsSector;
}

uint32 Dcm_GetS3TimeoutCount(void)
{
    return Dcm_S3TimeoutCount;
}

boolean Dcm_IsResetRequested(void)
{
    return Dcm_ResetRequested;
}

uint32 Dcm_GetLastSeed(void)
{
    return Dcm_SecSeed;
}

uint32 Dcm_GetDebugExpectedKey(void)
{
    return Dcm_CalcKey(Dcm_SecSeed);
}

const char* Dcm_GetBuildStamp(void)
{
    return __DATE__ " " __TIME__;
}

void Dcm_GetVersionInfo(Std_VersionInfoType* versioninfo)
{
    if (versioninfo == NULL_PTR)
    {
        DCM_DET_ERROR(DCM_API_GET_VERSION_INFO, DCM_E_PARAM_POINTER);
        return;
    }

    versioninfo->vendorID         = (uint16)DCM_VENDOR_ID;
    versioninfo->moduleID         = (uint16)DCM_MODULE_ID;
    versioninfo->sw_major_version = 1U;
    versioninfo->sw_minor_version = 0U;
    versioninfo->sw_patch_version = 0U;
}
