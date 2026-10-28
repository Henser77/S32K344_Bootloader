/*==================================================================================================
 *  Services/Communication/pdur.c —— PduR 实现
 *
 *  【本文件只有四件事：两条路由 + 一条消息交付 + 一个路由表】
 *  因为路由表只有一行，所以本文件很短 —— 短不代表多余，
 *  它把"Dcm 依赖 CanTp、CanTp 依赖 Can"这两个依赖关系切断在这里。
 *================================================================================================*/

#include "pdur.h"
#include "pdur_cfg.h"
#include "canif.h"
#include "cantp.h"
#include "dcm.h"
#include "boot_trace.h"
#include "Det.h"
#include <string.h>

/** @brief AUTOSAR 模块 ID：PduR = 51 (0x33) */
#define PDUR_MODULE_ID              (0x33U)

/*==================================================================================================
 *                                      本地变量
 *================================================================================================*/

/** @brief CanTp 拼装完成的消息缓冲（交给 Dcm 用） */
static uint8 PduR_RxMsgBuf[CANTP_RX_BUF_SIZE];

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

void PduR_Init(void)
{
    (void)memset(PduR_RxMsgBuf, 0, sizeof(PduR_RxMsgBuf));
}

void PduR_CanIfRxIndication(PduIdType RxPduId, const PduInfoType* PduInfoPtr)
{
    /* 路由：CanIf 接收 PDU → CanTp 接收 PDU
     * 本工程只有一条路由，所以不需要查表；将来多一条再加数组。 */
    if ((uint32)RxPduId != (uint32)PDUR_CANIF_RX_TO_CANTP)
    {
        return;     /* 不是我们关心的 PDU，丢掉 */
    }

    CanTp_RxIndication((PduIdType)CANTP_RX_PDU_ID, PduInfoPtr);
}

void PduR_CanIfTxConfirmation(PduIdType TxPduId, Std_ReturnType result)
{
    (void)TxPduId;      /* 只有一条发送路由 */

    /* 发送确认要一路传回 CanTp —— 它靠这个信号发下一帧连续帧
     * （本工程只有 1 个发送邮箱，不等确认会覆盖掉还没发出去的那帧）*/
    CanTp_TxConfirmation((PduIdType)CANTP_TX_PDU_ID, result);
}

Std_ReturnType PduR_DcmTransmit(PduIdType TxPduId, const PduInfoType* PduInfoPtr)
{
    if ((uint32)TxPduId >= (uint32)PDUR_DCM_TX_PDU_COUNT)
    {
        (void)Det_ReportError(PDUR_MODULE_ID, 0U, 0x02U, 0x01U);     /* moduleId 0x30 = PduR */
        return E_NOT_OK;
    }

    /* 路由：Dcm 响应 → CanTp（长度可能 > 8，由 CanTp 自己拆帧） */
    return CanTp_Transmit((PduIdType)PDUR_DCM_TX_TO_CANTP, PduInfoPtr);
}

Std_ReturnType PduR_CanTpTransmit(PduIdType TxNPduId, const PduInfoType* PduInfoPtr)
{
    if ((uint32)TxNPduId != (uint32)PDUR_CANTP_TX_TO_CANIF)
    {
        (void)Det_ReportError(PDUR_MODULE_ID, 0U, 0x02U, 0x02U);
        return E_NOT_OK;
    }

    /* 路由：CanTp 的一个 8 字节帧 → CanIf → Can_Write */
    return CanIf_Transmit((PduIdType)CANIF_TX_PDU_ID_UDS, PduInfoPtr);
}

void PduR_MainFunction(void)
{
    uint32 msgLen = 0U;

    /*------------------------------------------------------------------
     * 只有一件事：CanTp 拼好一条完整消息了吗？有就交给 Dcm。
     *
     * 这里相当于 AUTOSAR 里 CanTp → PduR → Dcm 的那条交付链，
     * 只是本工程简化成"整条消息一次性交付"。
     *------------------------------------------------------------------*/
    if (CanTp_IsRxMessageReady() == FALSE)
    {
        return;
    }

    if (CanTp_GetRxMessage(PduR_RxMsgBuf, (uint32)sizeof(PduR_RxMsgBuf), &msgLen) != E_OK)
    {
        return;
    }

    /* 交给 Dcm 解析（0x10/0x27/0x31/0x34/0x36/0x37/0x3E...） */
    (void)Dcm_RxIndication(PduR_RxMsgBuf, msgLen);

    /* 调试模块记录这次请求（编译开关关掉后什么都不做） */
    BootTrace_OnUdsRequest(PduR_RxMsgBuf, msgLen);
}
