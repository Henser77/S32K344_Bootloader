/*==================================================================================================
 *  EcuAbstraction/canif_cfg.h —— CanIf 配置
 *
 *  【这个文件放什么】
 *  只放"配置数据"，不放逻辑 —— 和 AUTOSAR 的做法一致（canif.c 不写死任何 ID）。
 *
 *  【本工程的配置非常小】
 *  只有一路诊断收发：
 *      接收：CAN ID 0x7E0  → CanIf 接收 PDU 0 → 上交 PduR
 *      发送：CanIf 发送 PDU 0 → CAN ID 0x7E8 → 走发送硬件对象 Hth4
 *
 *  【为什么 CAN ID 定义在 CanIf 而不在 CanTp】
 *  CanTp 处理的是 "N-PDU"，它**不应该知道 CAN ID** —— 那是 CanIf 的事。
 *  把 ID 从 cantp_cfg.h 挪到这里，是为了让 CanTp 彻底与硬件解耦：
 *  它只收发"8 字节数据块"，不关心这 8 字节挂在哪个 ID 上。
 *================================================================================================*/
#ifndef CANIF_CFG_H
#define CANIF_CFG_H

/*==================================================================================================
 *                                    接收方向（Can → CanIf）
 *================================================================================================*/

/** @brief 诊断物理请求 ID（诊断仪发给我们） */
#define CANIF_RX_CAN_ID_UDS         (0x7E0U)

/** @brief CAN ID 比较掩码（剥掉标准帧/扩展帧/FD 标志位） */
#define CANIF_RX_CAN_ID_MASK        (0x1FFFFFFFUL)

/** @brief 诊断请求对应的 CanIf 接收 PDU 编号（上报给 PduR 时使用） */
#define CANIF_RX_PDU_ID_UDS         (0U)

/*==================================================================================================
 *                                    发送方向（CanIf → Can）
 *================================================================================================*/

/** @brief 诊断响应 ID（我们发给诊断仪） */
#define CANIF_TX_CAN_ID_UDS         (0x7E8U)

/** @brief 诊断响应对应的 CanIf 发送 PDU 编号 */
#define CANIF_TX_PDU_ID_UDS         (0U)

/**
 * @brief 诊断响应使用的发送硬件对象（Hth）
 *
 * @details 值来自 Mcal/can_cfg.h，必须与 can_cfg.c 里 Can_TxObjects[] 的行顺序一致。
 *          本工程只有一个发送对象（发 0x7E8），所以是 Hth4 的第 0 行。
 */
#define CANIF_TX_HTH_UDS            (0U)

/** @brief 发送 PDU 的 CAN ID 查表（索引 = CanIf 发送 PDU 编号） */
#define CANIF_TX_CAN_ID_TABLE       { (CANIF_TX_CAN_ID_UDS) }

/** @brief 发送 PDU 的硬件对象查表（索引 = CanIf 发送 PDU 编号） */
#define CANIF_TX_HTH_TABLE          { (CANIF_TX_HTH_UDS) }

/** @brief 接收路由表项数 / 发送 PDU 数 */
#define CANIF_RX_PDU_COUNT          (1U)
#define CANIF_TX_PDU_COUNT          (1U)

#endif /* CANIF_CFG_H */
