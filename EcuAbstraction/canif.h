/*==================================================================================================
 *  EcuAbstraction/canif.h —— ECU 抽象层：CanIf（CAN Interface）
 *
 *  【本层在架构中的位置】
 *
 *      PduR（服务层，唯一的路由表持有者）
 *         ↑ ↓
 *      CanIf（本层）   ← 把"PDU 编号"翻译成"硬件句柄 + CAN ID"
 *         ↑ ↓
 *      Can（MCAL）     ← 把"硬件句柄 + CAN ID"翻译成"邮箱操作"
 *         ↑ ↓
 *      FlexCAN 硬件
 *
 *  【本层职责（本工程只用到 4 条）】
 *      1. 接收：被 Can 驱动回调 → 按 CAN ID 过滤 → 转成标准 PduInfoType
 *               → 上交 PduR
 *      2. 发送：CanIf_Transmit(PDU编号, PduInfo) → 查表得 CAN ID + Hth
 *               → 调 Can_Write()
 *      3. 发送确认：被 Can 驱动回调 → 查表得 PDU 编号 → 上交 PduR
 *      4. 注册一个"接收观察回调"，供调试模块打印原始帧用
 *
 *  【和 CanGw 工程里的 CanIf 是什么关系】
 *  同一套 AUTOSAR 接口命名，实现按本工程需要裁剪。
 *  CanGw 那份有 ControllerMode / PduMode / BusOff / ErrorState / 多通道，
 *  因为它是信号网关；本工程只有一路诊断收发，用不到那些。
 *
 *  【本文件不包含 RTD / MCAL 实现细节】
 *  只依赖 AUTOSAR 标准类型和 Can 驱动的回调签名。
 *================================================================================================*/
#ifndef CANIF_H
#define CANIF_H

#include "Std_Types.h"
#include "ComStack_Types.h"
#include "Can.h"
#include "canif_cfg.h"

/*==================================================================================================
 *                                    类型定义
 *================================================================================================*/

/**
 * @brief 接收指示回调（供调试模块用）
 *
 * @details CanIf 把帧交给 PduR 之后，会再通知一次这个回调。
 *          签名刻意做成"只有 PDU 编号 + PduInfo"，不含 CAN ID ——
 *          因为 ID 是 CanIf 自己的知识，调试模块只关心"数据是什么"。
 *
 *          它运行在**中断上下文**，所以回调里绝对不能打印！
 *          本工程的 BootTrace 只是把数据拷进环形缓冲，打印留给主循环。
 */
typedef void (*CanIf_RxIndicationHookType)(PduIdType RxPduId, const PduInfoType* PduInfoPtr);

/*==================================================================================================
 *                                        API
 *================================================================================================*/

/**
 * @brief 初始化 CanIf
 * @details 只清内部计数和状态；配置是编译期的（canif_cfg.h），不做动态解析。
 * @pre 必须在 Can_Init 之后调用（它要注册接收/发送回调）
 */
void CanIf_Init(void);

/**
 * @brief 发送一个 PDU（由 PduR 调用）
 *
 * @param[in] TxPduId      CanIf 发送 PDU 编号（见 canif_cfg.h）
 * @param[in] PduInfoPtr   要发的数据（长度必须 ≤ 8）
 *
 * @return E_OK    已交给 Can 驱动（数据已被拷贝进邮箱）
 *         E_NOT_OK 编号非法、长度非法，或邮箱忙（调用方下次重试）
 */
Std_ReturnType CanIf_Transmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr);

/**
 * @brief 接收指示（由 Mcal/can.c 的接收中断回调）
 *
 * @param[in] Hrh     接收硬件对象句柄
 * @param[in] PduInfo 收到的帧（id / length / sdu）
 *
 * @note **运行在中断上下文**：
 *       ① 不能打印、不能阻塞
 *       ② PduInfo->sdu 指向的缓冲在回调返回后会被下一帧覆盖，必须立刻处理/拷贝
 */
void CanIf_RxIndication(Can_HwHandleType Hrh, const Can_PduType* PduInfo);

/**
 * @brief 发送确认（由 Mcal/can.c 的发送中断回调）
 * @param[in] Hth 发送硬件对象句柄
 */
void CanIf_TxConfirmation(Can_HwHandleType Hth);

/**
 * @brief 注册接收观察回调（调试用，可以传 NULL_PTR 表示不需要）
 * @note 回调在中断上下文里被调用，实现不能阻塞
 */
void CanIf_RegisterRxIndicationHook(CanIf_RxIndicationHookType Hook);

/**
 * @brief 取 CanIf 的收发统计（诊断用）
 *
 * @param[out] Rx         收到的、通过 ID 过滤的帧数
 * @param[out] RxFiltered 收到但被 ID 过滤掉的帧数
 * @param[out] Tx         成功交给 Can_Write 的帧数
 * @param[out] TxBusy     发送失败（邮箱忙或真失败）的次数
 *
 * @details 【为什么需要这个】
 *  出现"上位机发了请求、ECU 不回"这种故障时，只看串口是分不清的：
 *      Rx 在涨  → 板子收到了，问题在发送侧
 *      Rx 不动  → 板子根本没收到，问题在线/接线/适配器
 *      Tx 在涨但 Rx 不动 → 板子在发，但对方没收到（或对方没发过来）
 *  有了这四个数，一次就能把问题劈成两半。
 */
void CanIf_GetStats(uint32* Rx, uint32* RxFiltered, uint32* Tx, uint32* TxBusy);

#endif /* CANIF_H */
