/*==================================================================================================
 *  Mcal/cantp.h —— CanTp（ISO 15765-2 传输层）接口
 *
 *  【这个模块解决什么问题】
 *  UDS 的一条诊断消息可能有几百上千字节，但**一帧经典 CAN 只能装 8 字节**。
 *  CanTp 负责：
 *      发送方向：把一条长消息切成多帧，按协议一帧一帧发出去
 *      接收方向：把收到的多帧拼回一条完整消息
 *
 *  【ISO 15765-2 的四种帧】
 *
 *      类型   首字节高4位   首字节低4位 / 后续字节含义
 *      SF     0000         低 4 位 = 数据长度（1~7），后面直接是数据
 *      FF     0001         低 4 位 + 第 2 字节 = 总长度（12 位，最大 4095），后面 6 字节是数据
 *      CF     0010         低 4 位 = 序号 SN（1,2,...,F,0,1...），后面最多 7 字节数据
 *      FC     0011         低 4 位 = 流控状态 FS，第 2 字节 = 块大小 BS，第 3 字节 = STmin
 *
 *      SN（序号）是核心：CF 必须按 1,2,3...F,0,1... 循环编号，
 *      接收方逐个核对 —— 丢了任何一帧都能立刻发现，不会错位拼接。
 *
 *  【一次完整的多帧传输长什么样】
 *
 *      发送方(诊断仪)                          接收方(我们)
 *      ─────────────────────────────────────────────────
 *      FF  10 80 <6字节>   总长128字节 ─────→  收到，回 FC
 *                          ←─────  FC 30 00 00   允许继续，一次发完，无间隔
 *      CF  21 <7字节>       ─────────────→
 *      CF  22 <7字节>       ─────────────→
 *      ...
 *      CF  2F <7字节>       ─────────────→  拼满 128 字节，消息完整
 *
 *  【本工程和 AUTOSAR 标准的差异】
 *  标准里 CanTp 上下还有 PduR（路由）和 CanIf（接口抽象），本工程都省掉了：
 *
 *      Dcm → CanTp → Can（Mcal/can.c）
 *
 *  但 CanTp 对外的函数名和语义**尽量保持和 AUTOSAR 一致**，
 *  这样将来要嵌进完整 AUTOSAR 工程时不用重写。
 *================================================================================================*/
#ifndef CANTP_H
#define CANTP_H

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "Std_Types.h"
#include "ComStack_Types.h"     /* AUTOSAR 标准类型：PduInfoType / PduIdType /
                                   TPParameterType / BufReq_ReturnType / Std_ReturnType */
#include "cantp_cfg.h"

/*==================================================================================================
 *                                    DEFINES AND MACROS
 *================================================================================================*/

/**
 * @brief 模块号
 * @details 0x2A（42）—— AUTOSAR 分配给 CanTp 的标准模块号。
 */
#define CANTP_MODULE_ID             (0x2AU)

/**
 * @brief 厂商号
 * @details 43 —— NXP 的 AUTOSAR vendor ID。本工程整体基于 NXP RTD，
 *          所以沿用这个值让版本信息在工具链里能对上。
 *          若将来作为独立产品发布，应申请自己的 vendor ID。
 */
#define CANTP_VENDOR_ID             (43U)

/** @brief 本工程只有一路 CAN 通道，实例号固定 0 */
#define CANTP_INSTANCE_ID           (0U)

/** @brief 协议控制信息（PCI）类型，取首字节高 4 位 */
#define CANTP_PCI_SF                (0x0U)      /**< 单帧 Single Frame */
#define CANTP_PCI_FF                (0x1U)      /**< 首帧 First Frame */
#define CANTP_PCI_CF                (0x2U)      /**< 连续帧 Consecutive Frame */
#define CANTP_PCI_FC                (0x3U)      /**< 流控帧 Flow Control */

/** @brief 流控帧的状态（FS） */
#define CANTP_FS_CTS                (0x0U)      /**< Continue To Send —— 可以继续 */
#define CANTP_FS_WAIT               (0x1U)      /**< Wait —— 对端还没准备好，等着 */
#define CANTP_FS_OVFLW              (0x2U)      /**< Overflow —— 对端缓冲区装不下 */

/** @brief 经典 CAN 一帧最大字节数 */
#define CANTP_CAN_DL                (8U)

/** @brief 首帧里携带的数据字节数（8 - 2 字节 PCI） */
#define CANTP_FF_DATA_LEN           (6U)

/** @brief 连续帧里携带的数据字节数（8 - 1 字节 PCI） */
#define CANTP_CF_DATA_LEN           (7U)

/** @brief 单帧能承载的最大数据字节数（8 - 1 字节 PCI） */
#define CANTP_SF_MAX_DATA           (7U)

/** @brief 12 位长度域能表示的最大值 */
#define CANTP_MAX_MSG_LEN           (4095U)

/*==================================================================================================
 *                                 STRUCTURES AND OTHER TYPEDEFS
 *================================================================================================*/

/** @brief CanTp 当前所处状态（调试和自检用） */
typedef enum
{
    CANTP_STATE_IDLE = 0U,          /**< 空闲，等新消息 */
    CANTP_STATE_RX_WAIT_CF,         /**< 已收首帧，正在等后续连续帧 */
    CANTP_STATE_TX_WAIT_FC,         /**< 已发首帧，正在等对端流控帧 */
    CANTP_STATE_TX_SENDING          /**< 正在逐帧发连续帧 */
} CanTp_StateType;

/** @brief 传输中止的原因（诊断用） */
typedef enum
{
    CANTP_ABORT_NONE = 0U,
    CANTP_ABORT_RX_TIMEOUT,         /**< 等连续帧超时（N_Cr） */
    CANTP_ABORT_TX_TIMEOUT,         /**< 等流控帧超时（N_Bs） */
    CANTP_ABORT_RX_SEQ,             /**< 连续帧序号错（丢帧） */
    CANTP_ABORT_TX_OVFLW,           /**< 对端报告缓冲区溢出 */
    CANTP_ABORT_RX_OVFLW,           /**< 消息长度超过本模块缓冲 */
    CANTP_ABORT_TX_FAILED           /**< 底层发送失败 */
} CanTp_AbortReasonType;

/**
 * @brief CanTp 配置结构体（AUTOSAR 标准类型）
 *
 * @details 【为什么本工程这个结构体是空的】
 *          本工程用**编译期配置**（Pre-Compile）：通道数、缓冲区大小、
 *          超时参数全部写在 cantp_cfg.h 的宏里，运行时不需要传配置。
 *
 *          但 AUTOSAR SWS 规定 CanTp_Init 的参数类型是
 *          `const CanTp_ConfigType*`，为了保持接口签名一致，这里保留该类型，
 *          调用方传 NULL_PTR 即可。
 *
 *          如果将来要做 Post-Build（同一份代码配不同参数），
 *          往这个结构体里加字段即可，函数签名不用改。
 */
typedef struct
{
    uint8 NumChannels;              /**< 通道数（本工程恒为 1，占位用） */
} CanTp_ConfigType;

/*==================================================================================================
 *                                    FUNCTION PROTOTYPES
 *================================================================================================*/

/**
 * @brief 初始化传输层（AUTOSAR SWS 标准签名）
 *
 * @details 干三件事：
 *          ① 清空所有状态和计数
 *          ② 启用 Cortex-M7 的周期计数器 DWT->CYCCNT 作为时间基准
 *          ③ 自检时间基准是否真的在走（结果由 CanTp_IsTimeBaseOk() 查询）
 *
 *          `CfgPtr` 在本工程里不使用（编译期配置），传 NULL_PTR 即可。
 *
 *          必须在 Can_Init() 之后、主循环之前调用一次。
 *
 * @param[in] CfgPtr 配置指针（本工程传 NULL_PTR）
 */
void CanTp_Init(const CanTp_ConfigType* CfgPtr);

/**
 * @brief 周期处理函数 —— 在主循环里反复调用
 *
 * @details 本模块所有"需要时间推进"的工作都在这里做：
 *          ① 检查 N_Bs / N_Cr 超时
 *          ② 把待发的流控帧发出去
 *          ③ 逐帧发送连续帧（遵守对端要求的 STmin 和块大小 BS）
 *
 *          ⚠️ 这个函数的调用频率直接决定时序精度：
 *             主循环转得越快，STmin 和超时就越准。
 *             本工程主循环是空的，转一圈不到 1 微秒，完全够用。
 */
void CanTp_MainFunction(void);

/**
 * @brief 下层（CanIf）收到一帧时调用 —— **中断上下文**（AUTOSAR SWS 标准签名）
 *
 * @details 【标准签名意味着什么】
 *          `CanTp_RxIndication(PduIdType RxPduId, const PduInfoType* PduInfoPtr)`
 *          —— 注意它**不再带 CAN ID**。这是有意设计：
 *              CAN ID 的过滤是 **CanIf** 的职责，CanTp 只认一个"PDU 编号"。
 *          所以本工程里 main.c 的 `Boot_CanRxCallback` 扮演 CanIf 的适配角色：
 *              按 ID 过滤 → 转成 PduInfoType → 调本函数。
 *
 * @param[in] RxPduId    接收 PDU 编号（本工程恒为 0）
 * @param[in] PduInfoPtr 收到的数据（SduDataPtr / SduLength）
 */
void CanTp_RxIndication(PduIdType RxPduId, const PduInfoType* PduInfoPtr);

/**
 * @brief 下层发送完成时调用 —— **中断上下文**（AUTOSAR SWS 标准签名）
 *
 * @details 【为什么需要它】
 *          本工程只配了 1 个发送邮箱，同一时刻只能挂一帧。
 *          所以发完一帧连续帧后必须等这个确认，才能发下一帧 ——
 *          否则第二帧会拿到 CAN_BUSY 而丢掉。
 *
 * @param[in] TxPduId 发送 PDU 编号（本工程恒为 0）
 * @param[in] result  E_OK = 发送成功；其它 = 失败（会中止本次传输）
 */
void CanTp_TxConfirmation(PduIdType TxPduId, Std_ReturnType result);

/**
 * @brief 上层（PduR / 本工程由 Dcm 直接调）请求发送一条消息
 *        —— AUTOSAR SWS 标准签名
 *
 * @details 短消息（≤7 字节）直接发单帧；
 *          长消息发首帧，然后等对端回送流控帧，由 CanTp_MainFunction 继续。
 *
 *          ⚠️ 数据会被**拷贝**到内部缓冲，函数返回后调用方可以立即复用原缓冲。
 *
 * @param[in] TxPduId    发送 PDU 编号（本工程恒为 0）
 * @param[in] PduInfoPtr 要发的数据（SduDataPtr / SduLength）
 *
 * @return E_OK     已开始发送（不代表已发完）
 *         E_NOT_OK 参数错误，或上一次发送还没结束
 */
Std_ReturnType CanTp_Transmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr);

/*==================================================================================================
 *                     其余 AUTOSAR SWS 标准接口（CanTp 完整 API 表）
 *================================================================================================*/

/**
 * @brief 取消指定的发送请求
 * @details 本工程发送量小、流程同步，正常不会用到；
 *          保留是为了 API 表完整，将来接入 PduR 时能直接对接。
 * @return E_OK 已取消；E_NOT_OK 该 TxPduId 没有正在进行的发送
 */
Std_ReturnType CanTp_CancelTransmit(PduIdType TxPduId);

/**
 * @brief 取消指定的接收请求
 * @details 注意：AUTOSAR 规定它**不能**主动发流控帧中止对端，
 *          只是让本端停止接收。
 * @return E_OK 已取消；E_NOT_OK 该 RxPduId 没有正在进行的接收
 */
Std_ReturnType CanTp_CancelReceive(PduIdType RxPduId);

/**
 * @brief 运行期修改传输参数
 * @details 本工程参数在编译期固定，因此除 STmin/N_Bs/N_Cr 之外的值不支持。
 * @param[in] id        通道/PDU 编号
 * @param[in] parameter TP_STMIN / TP_BS / TP_BC
 * @param[in] value     新值
 * @return E_OK 支持且已生效；E_NOT_OK 不支持或参数非法
 */
Std_ReturnType CanTp_ChangeParameter(PduIdType id, TPParameterType parameter, uint16 value);

/**
 * @brief 读取当前生效的传输参数
 * @param[in]  id        通道/PDU 编号
 * @param[in]  parameter 要读的参数
 * @param[out] value     读出结果
 * @return E_OK 成功；E_NOT_OK 参数不支持
 */
Std_ReturnType CanTp_ReadParameter(PduIdType id, TPParameterType parameter, uint16* value);

/**
 * @brief 关闭传输层
 * @details 把所有状态回到"未初始化"，不再响应收发。
 */
void CanTp_Shutdown(void);

/**
 * @brief 取模块版本信息（AUTOSAR 标准接口）
 * @param[out] versioninfo 版本结构（VendorId / ModuleId / 软硬件版本号）
 */
void CanTp_GetVersionInfo(Std_VersionInfoType* versioninfo);

/**
 * @brief 查询是否已收到一条完整消息
 * @return TRUE 有完整消息可取（取走后自动清除）
 */
boolean CanTp_IsRxMessageReady(void);

/**
 * @brief 取出收到的完整消息
 *
 * @note ⚠️ **非 AUTOSAR 接口**，是本工程的便利函数。
 *       标准里 CanTp 通过 `PduR_CanTpStartOfReception()` /
 *       `PduR_CanTpCopyRxData()` / `PduR_CanTpRxIndication()` 三个回调
 *       把数据**流式**交给 PduR，PduR 再交给 Dcm，CanTp 自己不保存完整消息。
 *
 *       本工程没有 PduR，所以做了两种选择之二：
 *           CanTp 内部完整重组 → 上层用这个函数一次性取走。
 *       这样做牺牲了一点"标准感"，换来：
 *           ① 代码简单、易调试
 *           ② 重组过程不受上层处理速度影响（不会因为 Dcm 处理慢而丢帧）
 *
 *       将来若接入真实 PduR，把这个函数换成那三个回调即可。
 *
 * @param[out] Buffer     存放消息的缓冲
 * @param[in]  BufferSize 该缓冲的大小
 * @param[out] OutLength  实际消息长度
 *
 * @return E_OK     取到消息
 *         E_NOT_OK 当前没有就绪的消息，或缓冲太小
 */
Std_ReturnType CanTp_GetRxMessage(uint8* Buffer, uint32 BufferSize, uint32* OutLength);

/*==================================================================================================
 *                                查询接口（自检 / 调试用）
 *
 *  @note 这一组**都不是 AUTOSAR 接口** —— 标准里没有它们。
 *        保留的目的是让上电自检和串口日志能说清"CanTp 现在的状态"。
 *        正式产品可以整组删掉，不影响协议行为。
 *================================================================================================*/

/** @brief 当前状态 */
CanTp_StateType CanTp_GetState(void);

/** @brief 最近一次中止的原因 */
CanTp_AbortReasonType CanTp_GetAbortReason(void);

/**
 * @brief 中止瞬间已收到的字节数
 * @details 配合 CanTp_GetLastAbortRxTotal() 使用：
 *              RxLen == 0             → 根本没收到首帧
 *              0 < RxLen < RxTotal    → 拼到一半断掉（最常见）
 *              RxTotal == 0           → 不是接收方向的中止
 */
uint32 CanTp_GetLastAbortRxLen(void);

/** @brief 中止瞬间这条消息原本的总长度（首帧里声明的） */
uint32 CanTp_GetLastAbortRxTotal(void);

/** @brief 时间基准（DWT 周期计数器）是否可用 */
boolean CanTp_IsTimeBaseOk(void);

/** @brief 累计收到的帧数（所有类型） */
uint32 CanTp_GetRxFrameCount(void);

/** @brief 累计收到的完整消息数 */
uint32 CanTp_GetRxMsgCount(void);

/** @brief 累计发出的帧数 */
uint32 CanTp_GetTxFrameCount(void);

/** @brief 累计中止次数 */
uint32 CanTp_GetAbortCount(void);

/** @brief fls 风格：本文件的编译时间戳，用于确认烧录的是新版本 */
const char* CanTp_GetBuildStamp(void);

#endif /* CANTP_H */
