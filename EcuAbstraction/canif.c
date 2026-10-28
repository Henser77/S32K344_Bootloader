/*==================================================================================================
 *  EcuAbstraction/canif.c —— CanIf 实现
 *
 *  【设计要点】
 *  1. 接收路径**在中断里**完成：按 CAN ID 过滤 → 转 PduInfoType → 立刻交 PduR。
 *     放在中断里做的原因：拼装只是几字节拷贝（微秒级），
 *     而主循环可能被擦 Flash 阻塞几十毫秒 —— 放主循环会丢帧。
 *  2. 发送路径查表：CanIf 发送 PDU 编号 → CAN ID + 硬件对象（Hth）。
 *     这样上层（PduR/CanTp/Dcm）完全不需要知道 CAN ID 和邮箱编号。
 *  3. 所有映射都来自 canif_cfg.h，本文件不写死任何 ID。
 *================================================================================================*/

#include "canif.h"
#include "can_cfg.h"        /* CanConf_TxObject_Hth4 等硬件对象句柄 */
#include "pdur.h"           /* 接收/确认要向 PduR 上报 */
#include "Det.h"
#include <string.h>

/** @brief AUTOSAR 模块 ID：CanIf = 60 (0x3C) */
#define CANIF_MODULE_ID             (0x3CU)

/*==================================================================================================
 *                                      本地配置表
 *================================================================================================*/

/** @brief 发送 PDU 的 CAN ID 表（索引 = CanIf 发送 PDU 编号） */
static const uint32 CanIf_TxCanIdTable[CANIF_TX_PDU_COUNT] = CANIF_TX_CAN_ID_TABLE;

/** @brief 发送 PDU 的硬件对象表（索引 = CanIf 发送 PDU 编号） */
static const Can_HwHandleType CanIf_TxHthTable[CANIF_TX_PDU_COUNT] =
{
    (Can_HwHandleType)(CAN_HTH_FIRST_INDEX + CANIF_TX_HTH_UDS)
};

/*==================================================================================================
 *                                      本地变量
 *================================================================================================*/

/** @brief 接收观察回调（调试用，可为 NULL） */
static CanIf_RxIndicationHookType CanIf_RxHook = NULL_PTR;

/** @brief 统计（诊断用） */
static uint32 CanIf_RxCount     = 0U;
static uint32 CanIf_RxFiltered  = 0U;    /* 被 CAN ID 过滤掉的帧数 */
static uint32 CanIf_TxCount     = 0U;
static uint32 CanIf_TxBusyCount = 0U;    /* 邮箱忙导致的发送失败次数 */

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

void CanIf_Init(void)
{
    CanIf_RxHook      = NULL_PTR;
    CanIf_RxCount     = 0U;
    CanIf_RxFiltered  = 0U;
    CanIf_TxCount     = 0U;
    CanIf_TxBusyCount = 0U;

    /* 把"接收中断"和"发送确认"接到本层 —— 这是 CanIf 与 MCAL 的唯一连线点。
     * 注意：can.c 的回调签名给的是**硬件句柄**，而 CanTp 要的是 **PDU 编号**，
     *       这个翻译就是本层的存在意义。 */
    Can_RegisterRxHook(CanIf_RxIndication);
    Can_RegisterTxHook(CanIf_TxConfirmation);
}

Std_ReturnType CanIf_Transmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr)
{
    Can_PduType    pdu;
    Std_ReturnType ret;
    uint8          len;

    if ((PduInfoPtr == NULL_PTR) || (PduInfoPtr->SduDataPtr == NULL_PTR))
    {
        (void)Det_ReportError(CANIF_MODULE_ID, 0U, 0x49U, 0x01U);     /* moduleId 4 = CanIf */
        return E_NOT_OK;
    }

    if ((uint32)TxPduId >= (uint32)CANIF_TX_PDU_COUNT)
    {
        (void)Det_ReportError(CANIF_MODULE_ID, 0U, 0x49U, 0x02U);
        return E_NOT_OK;
    }

    len = (uint8)PduInfoPtr->SduLength;

    /* 经典 CAN 单帧最多 8 字节 —— 超了直接拒绝，不要悄悄截断 */
    if ((len == 0U) || (len > 8U))
    {
        (void)Det_ReportError(CANIF_MODULE_ID, 0U, 0x49U, 0x03U);
        return E_NOT_OK;
    }

    pdu.id          = (Can_IdType)CanIf_TxCanIdTable[TxPduId];
    pdu.swPduHandle = TxPduId;              /* 发送确认时靠它把编号找回来 */
    pdu.length      = len;
    pdu.sdu         = PduInfoPtr->SduDataPtr;   /* Can_Write 只读，不会改 */

    ret = Can_Write(CanIf_TxHthTable[TxPduId], &pdu);

    if (ret == E_OK)
    {
        CanIf_TxCount++;
        return E_OK;
    }

    /* CAN_BUSY（邮箱还占着）或真失败 —— 都让调用方下次重试 */
    CanIf_TxBusyCount++;
    return E_NOT_OK;
}

void CanIf_RxIndication(Can_HwHandleType Hrh, const Can_PduType* PduInfo)
{
    PduInfoType rxInfo;

    (void)Hrh;      /* 本工程只有 1 个接收对象，用不到 Hrh */

    if ((PduInfo == NULL_PTR) || (PduInfo->sdu == NULL_PTR))
    {
        return;
    }

    /*--------------------------------------------------------------------------
     * ① 按 CAN ID 过滤：只认诊断请求 ID（0x7E0）
     *
     * 过滤在 CanIf 里做，而不是在 Can 驱动的邮箱过滤器里做，是为了让
     * "哪些 ID 有意义"这件事集中在一个地方（canif_cfg.h），而不是散在 CT 配置里。
     *------------------------------------------------------------------------*/
    if ((PduInfo->id & CANIF_RX_CAN_ID_MASK) != (Can_IdType)CANIF_RX_CAN_ID_UDS)
    {
        CanIf_RxFiltered++;
        return;
    }

    CanIf_RxCount++;

    /*--------------------------------------------------------------------------
     * ② 转成标准 PduInfoType，交给 PduR
     *
     * CanTp 的标准签名是 CanTp_RxIndication(PduIdType, const PduInfoType*) ——
     * 不带 CAN ID，只认 PDU 编号。这个转换就是 CanIf 的核心工作。
     *------------------------------------------------------------------------*/
    rxInfo.SduDataPtr  = PduInfo->sdu;
    rxInfo.SduLength   = (PduLengthType)PduInfo->length;
    rxInfo.MetaDataPtr = NULL_PTR;

    PduR_CanIfRxIndication((PduIdType)CANIF_RX_PDU_ID_UDS, &rxInfo);

    /*--------------------------------------------------------------------------
     * ③ 通知观察回调（调试模块用来打印原始帧）
     *
     * 放在最后，且必须允许为 NULL。
     * 它同样在中断上下文执行，所以实现里只做拷贝。
     *------------------------------------------------------------------------*/
    if (CanIf_RxHook != NULL_PTR)
    {
        CanIf_RxHook((PduIdType)CANIF_RX_PDU_ID_UDS, &rxInfo);
    }
}

void CanIf_TxConfirmation(Can_HwHandleType Hth)
{
    (void)Hth;      /* 只有 1 个发送对象，不区分 Hth */

    /* 硬件句柄 → PDU 编号 → 告诉 PduR"你上次那帧发出去了" */
    PduR_CanIfTxConfirmation((PduIdType)CANIF_TX_PDU_ID_UDS, E_OK);
}

void CanIf_RegisterRxIndicationHook(CanIf_RxIndicationHookType Hook)
{
    CanIf_RxHook = Hook;
}

void CanIf_GetStats(uint32* Rx, uint32* RxFiltered, uint32* Tx, uint32* TxBusy)
{
    if (Rx != NULL_PTR)         { *Rx         = CanIf_RxCount;     }
    if (RxFiltered != NULL_PTR) { *RxFiltered = CanIf_RxFiltered; }
    if (Tx != NULL_PTR)         { *Tx         = CanIf_TxCount;     }
    if (TxBusy != NULL_PTR)     { *TxBusy     = CanIf_TxBusyCount; }
}
