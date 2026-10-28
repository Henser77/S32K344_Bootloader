/*==================================================================================================
 *  Mcal/dcm.h —— Dcm（UDS 诊断服务层，ISO 14229-1）接口
 *
 *  【Dcm 在整条链路上的位置】
 *
 *      诊断仪 ──CAN──→ can.c ──→ cantp.c ──→ dcm.c ──→ fls.c
 *        8字节一帧      收发一帧     拼/拆消息    看懂命令    擦写Flash
 *
 *  Dcm 的输入是"一条完整的 UDS 请求"，输出是"一条完整的 UDS 响应"。
 *  它不懂 CAN，也不懂帧、不懂分段 —— 那些都是下面两层的事。
 *
 *  【Dcm 内部有两个状态机】
 *
 *    ① 会话状态机
 *         DEFAULT ──0x10 03──→ EXTENDED ──0x10 02──→ PROGRAMMING
 *            ↑                                           │
 *            └────────   S3 超时（5s 没心跳） / 复位   ──────┘
 *
 *    ② 安全状态机
 *         LOCKED ──0x27 01（求种子）──→ 等密钥 ──0x27 02（发密钥）──→ UNLOCKED
 *                                      ↓ 错 3 次
 *                                  锁定 10 秒（NRC 0x36 / 0x37）
 *
 *  【为什么刷写要"会话 + 安全"两道门】
 *  会话：防止车辆在正常行驶时被误刷（必须先明确切到编程会话）
 *  安全：防止任何人插上 CAN 盒子就能刷（必须先通过种子/密钥校验）
 *  这是 ISO 14229-1 规定的流程，也是所有量产 ECU 的做法。
 *================================================================================================*/
#ifndef DCM_H
#define DCM_H

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "Std_Types.h"
#include "ComStack_Types.h"     /* PduInfoType / PduIdType */
#include "dcm_cfg.h"

/*==================================================================================================
 *                                    DEFINES AND MACROS
 *================================================================================================*/

/** @brief 模块号：0x35（53）—— AUTOSAR 分配给 Dcm 的标准模块号 */
#define DCM_MODULE_ID               (0x35U)

/** @brief 厂商号：43 = NXP（本工程整体基于 NXP RTD） */
#define DCM_VENDOR_ID               (43U)

/** @brief 单通道工程，实例号固定 0 */
#define DCM_INSTANCE_ID             (0U)

/*==================================================================================================
 *                                 STRUCTURES AND OTHER TYPEDEFS
 *================================================================================================*/

/** @brief 当前会话 */
typedef enum
{
    DCM_SESSION_STATE_DEFAULT = 0U,
    DCM_SESSION_STATE_PROGRAMMING,
    DCM_SESSION_STATE_EXTENDED
} Dcm_SessionStateType;

/** @brief 安全访问状态 */
typedef enum
{
    DCM_SECURITY_LOCKED = 0U,       /**< 未解锁 */
    DCM_SECURITY_SEED_SENT,         /**< 已发种子，等密钥 */
    DCM_SECURITY_UNLOCKED           /**< 已解锁 */
} Dcm_SecurityStateType;

/**
 * @brief Dcm 配置结构体（AUTOSAR 标准类型）
 * @details 本工程用编译期配置，运行时不需要传配置，调用时传 NULL_PTR 即可。
 */
typedef struct
{
    uint8 NumChannels;              /**< 通道数（本工程恒为 1，占位） */
} Dcm_ConfigType;

/*==================================================================================================
 *                                    FUNCTION PROTOTYPES
 *================================================================================================*/

/**
 * @brief 初始化诊断服务层
 * @details 会话回到 DEFAULT、安全回到 LOCKED、计数清零、定时器起点重置。
 *          必须在 CanTp_Init() 之后调用。
 * @param[in] ConfigPtr 配置指针（本工程传 NULL_PTR）
 */
void Dcm_Init(const Dcm_ConfigType* ConfigPtr);

/**
 * @brief 周期处理函数 —— 在主循环里反复调用
 * @details 负责两件事：
 *          ① S3 超时（会话保持）与安全延时到期检测
 *          ② 把还没发出去的响应推给 CanTp
 */
void Dcm_MainFunction(void);

/**
 * @brief 上层（本工程由 main.c 调）交付一条完整的 UDS 请求
 *
 * @note ⚠️ **非 AUTOSAR 接口**，是本工程的便利入口。
 *       标准里 PduR 通过 `Dcm_StartOfReception()` / `Dcm_CopyRxData()` /
 *       `Dcm_TpRxIndication()` 三个回调把数据交给 Dcm。
 *       本工程没有 PduR，所以用这一个函数代替（CanTp 已经内部完整重组好了）。
 *       将来接真实 PduR 时，换成那三个回调即可，服务处理逻辑一行都不用改。
 *
 * @param[in] ReqData 请求数据（第一个字节是 SID）
 * @param[in] ReqLen  请求长度
 *
 * @return E_OK     请求已受理（响应已排队或已发出）
 *         E_NOT_OK 参数非法（空指针 / 长度为 0）
 */
Std_ReturnType Dcm_RxIndication(const uint8* ReqData, uint32 ReqLen);

/**
 * @brief 取模块版本信息（AUTOSAR 标准接口）
 * @param[out] versioninfo 版本结构（VendorId / ModuleId / 软硬件版本号）
 */
void Dcm_GetVersionInfo(Std_VersionInfoType* versioninfo);

/*==================================================================================================
 *                        查询接口（自检 / 调试用，非 AUTOSAR 接口）
 *================================================================================================*/
Dcm_SessionStateType  Dcm_GetSession(void);
Dcm_SecurityStateType Dcm_GetSecurity(void);
uint32 Dcm_GetRequestCount(void);

/**
 * @brief 因"上一条响应还没发走"而丢弃的响应条数
 * @details 正常应恒为 0。非 0 说明出现"CanTp 忙时又来新请求"的竞争。
 */
uint32 Dcm_GetRespOverwriteCount(void);

/**
 * @brief 清掉"复位请求"标志
 *
 * @details 【为什么需要它】
 *  0x11 01 置了这个标志之后，EcuM 会尝试复位。但实测 S32K344 上
 *  写 AIRCR.SYSRESETREQ 不产生复位（见 ecum.c 里的说明）。
 *  复位没成功就必须把标志清掉，否则主循环每圈都会再试一次 —— 板子等于死机。
 */
void Dcm_ClearResetRequested(void);
uint32 Dcm_GetNegativeResponseCount(void);

/**
 * @brief S3 超时（会话保持超时）发生了几次
 * @details 超过 5 秒没收到任何请求，会话会自动退回 DEFAULT。
 *          上层把它报出来，免得看日志的人纳闷"我刚才明明切到扩展会话了"。
 */
uint32 Dcm_GetS3TimeoutCount(void);

/**
 * @brief 最近一次擦除例程（0xFF00）失败时，Fls 层返回的原始状态码
 * @details 取值含义见 fls.h 的 g_FlsLastStatus 说明：
 *              0x00 成功   0x02 通用错误(PEP/PES)   0x03 超时
 *              0x05 未擦净  0x06 写入校验失败  0x77 本模块校验失败
 *              0xEE 还没拿到状态
 *          只回给诊断仪一个"失败"状态字节是不够的，这个值才是排查依据。
 */
uint8 Dcm_GetLastFlsStatus(void);

/**
 * @brief 最近一次擦除例程（0xFF00）实际耗时（毫秒）
 * @details 擦 1 MB 正常需要 2~10 秒。接近 0 却返回成功 = 循环没真正执行。
 */
uint32 Dcm_GetLastEraseMs(void);

/** @brief 最近一次擦除失败时停在哪个扇区 */
uint32 Dcm_GetLastFlsSector(void);
const char* Dcm_GetBuildStamp(void);

/**
 * @brief 是否收到过"复位"请求
 * @details 主循环看到它变成 TRUE 就可以执行真正的复位。
 *          这样安排是为了让复位动作发生在"响应已经发出去之后"。
 */
boolean Dcm_IsResetRequested(void);

/**
 * @brief 到目前为止收到过多少条 UDS 请求
 *
 * @details 【为什么要暴露这个计数】
 *  Bootloader 的"启动窗口"需要知道"这 1.5 秒里有没有诊断仪来找过我"。
 *  用计数比用标志更灵活：调用方先记下当前值，过一会儿再看有没有变。
 */
uint32 Dcm_GetRequestCount(void);

/**
 * @brief 因"上一条响应还没发走"而丢弃的响应条数
 * @details 正常应恒为 0。非 0 说明出现"CanTp 忙时又来新请求"的竞争。
 */
uint32 Dcm_GetRespOverwriteCount(void);

/**
 * @brief 清掉"复位请求"标志
 *
 * @details 【为什么需要它】
 *  0x11 01 置了这个标志之后，EcuM 会尝试复位。但实测 S32K344 上
 *  写 AIRCR.SYSRESETREQ 不产生复位（见 ecum.c 里的说明）。
 *  复位没成功就必须把标志清掉，否则主循环每圈都会再试一次 —— 板子等于死机。
 */
void Dcm_ClearResetRequested(void);

/*==================================================================================================
 *                 调试接口（⚠️ 正式产品必须删掉这两个函数）
 *================================================================================================*/

/**
 * @brief 取上一条 0x27 请求里发出的种子
 * @note 调试用
 */
uint32 Dcm_GetLastSeed(void);

/**
 * @brief 取该种子对应的正确密钥
 *
 * @note ⚠️ **正式产品必须删掉这个函数。**
 *       把正确密钥暴露到任何端口（包括串口日志）都等于把安全访问废掉 ——
 *       拿到日志的人可以重放密钥直接解锁 ECU。
 *
 *       保留它的唯一理由是：**手工用 USB-CAN 工具测试时，人能算不出密钥**。
 *       有了它，串口日志里直接能看到"该回什么"，从而能完整走通 0x27 流程。
 */
uint32 Dcm_GetDebugExpectedKey(void);

#endif /* DCM_H */
