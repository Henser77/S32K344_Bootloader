/*==================================================================================================
 *  Services/Communication/pdur.h —— 服务层：PduR（PDU Router）
 *
 *  【本层在架构中的位置】
 *
 *      Dcm（诊断）        CanTp（分段传输）
 *         └───────┬───────────┘
 *                 ▼
 *               PduR      ← 本层：唯一知道"哪条 PDU 走哪条路"的地方
 *                 ▼
 *              CanIf      →  Can  →  硬件
 *
 *  【本层存在的根本理由】
 *  不是为了"多几层调用"，而是为了**让 Dcm 不知道 CanTp 的存在，让 CanTp 不知道 Can 的存在**。
 *
 *  如果没有 PduR，Dcm 就得写：
 *      CanTp_Transmit(CANTP_TX_PDU_ID, &info);      ← Dcm 直接依赖 CanTp
 *  换一种传输层（比如 CAN FD 的 CanTp、或者以太网的 DoIPTp），Dcm 就得改。
 *
 *  PduR 把这个知识全部吸收进来：**只有它知道"哪条 PDU 走哪条路"**。
 *
 *  【和 CanGw 工程的 PduR 的关系】
 *  同一套 AUTOSAR 接口命名，路由表按本工程裁剪。
 *  CanGw 那份要路由 Com / Dcm / CanTp 多条链路，本工程只有 CanTp ↔ Dcm 一条。
 *================================================================================================*/
#ifndef PDUR_H
#define PDUR_H

#include "Std_Types.h"
#include "ComStack_Types.h"

/*==================================================================================================
 *                                        API
 *================================================================================================*/

/**
 * @brief 初始化 PduR
 * @details 只清内部状态；路由表是编译期的（pdur_cfg.h）。
 */
void PduR_Init(void);

/**
 * @brief 主函数 —— 把 CanTp 拼装完成的消息交给 Dcm
 *
 * @details CanTp 只负责"把 8 字节帧拼成一条完整消息"，它不知道该消息是什么服务。
 *          本函数检查 CanTp 是否有一条完整消息待取，取出来交给 Dcm 解析。
 *
 * @note 本函数的角色等价于 AUTOSAR 里
 *       CanTp → PduR_CanTpCopyRxData/RxIndication → Dcm 的那条链路，
 *       只是本工程简化成"整条消息一次性交付"（Dcm_RxIndication 直接收完整缓冲）。
 */
void PduR_MainFunction(void);

/*-------------------------------------------------- 下层 → 本层 ----------------------------------*/

/**
 * @brief CanIf 收到一帧（接收中断上下文）
 * @param[in] RxPduId    CanIf 接收 PDU 编号
 * @param[in] PduInfoPtr 帧数据
 * @note 运行在中断里，本函数只做转交，不打印不阻塞
 */
void PduR_CanIfRxIndication(PduIdType RxPduId, const PduInfoType* PduInfoPtr);

/**
 * @brief CanIf 报告发送完成
 * @param[in] TxPduId CanIf 发送 PDU 编号
 * @param[in] result  发送结果
 */
void PduR_CanIfTxConfirmation(PduIdType TxPduId, Std_ReturnType result);

/*-------------------------------------------------- 上层 → 本层 ----------------------------------*/

/**
 * @brief Dcm 请求发送一个响应
 *
 * @param[in] TxPduId     Dcm 发送 PDU 编号（见 pdur_cfg.h）
 * @param[in] PduInfoPtr  响应数据（可能超过 8 字节，由 CanTp 负责拆帧）
 *
 * @return E_OK    已交给 CanTp
 *         E_NOT_OK 编号非法，或 CanTp 正忙（调用方下次重试）
 */
Std_ReturnType PduR_DcmTransmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr);

/*-------------------------------------------------- 传输层 → 本层 ---------------------------------*/

/**
 * @brief CanTp 请求发送一个 N-PDU（一个 8 字节 CAN 帧）
 *
 * @param[in] TxNPduId    CanTp 的 N-PDU 发送编号
 * @param[in] PduInfoPtr  帧数据（长度 ≤ 8）
 *
 * @return E_OK    已交给 CanIf
 *         E_NOT_OK 邮箱忙或编号非法
 *
 * @note 这个 API 是 CanTp 与硬件的**唯一出口**。
 *       有了它，cantp.c 里不再出现 can.h / Can_Write / 硬件句柄 ——
 *       CanTp 变成纯协议模块，换 CAN 控制器只需改配置。
 */
Std_ReturnType PduR_CanTpTransmit(PduIdType TxNPduId, const PduInfoType* PduInfoPtr);

#endif /* PDUR_H */
