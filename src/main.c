/*==================================================================================================
 *  src/main.c —— 集成层（只剩骨架）
 *
 *  【这个文件现在只有 3 件事】
 *      1. 调 EcuM_Init() 把整个 ECU 初始化
 *      2. 死循环里推进 BSW（EcuM_MainFunction）
 *      3. 死循环里推进路由（PduR_MainFunction）
 *
 *  【为什么这么薄】
 *  以前这个文件 633 行，同时干了 4 件事：初始化顺序、CanIf 适配、PduR 路由、调试打印。
 *  出了问题根本分不清是哪一层的毛病。现在按职责拆开：
 *
 *      EcuM_Init() / EcuM_MainFunction()   → Services/System/ecum.c      初始化顺序 + 调度
 *      CanIf_*                             → EcuAbstraction/canif.c      CAN ID 过滤 / PDU 转换
 *      PduR_*                              → Services/Communication/pdur.c  路由
 *      BootTrace_*                         → src/boot_trace.c             调试上报
 *
 *  【启动顺序为什么不在 main 里】
 *  模块之间有严格依赖（Clock → Port → IntCtrl → Det → Uart → Fls → Can → CanIf
 *  → PduR → CanTp → Dcm），这个顺序属于 EcuM 的职责。
 *  放在 main 里，任何一个新来的人改动顺序都可能踩坑。
 *================================================================================================*/

#include "ecum.h"
#include "pdur.h"
#include "boot_trace.h"

int main(void)
{
    /*--------------------------------------------------------------------------
     * 第一步：把整个 ECU 拉起来
     *
     * 这里面按依赖顺序初始化了所有模块；任何关键模块失败都会打印原因并停机。
     *------------------------------------------------------------------------*/
    EcuM_Init();

    /*--------------------------------------------------------------------------
     * 第二步：裸机大循环
     *
     * 【为什么这里没有 Can_MainFunction_Read()】
     * CAN 接收是**中断驱动**的：
     *     IntCtrl 配了 FlexCAN0_1_IRQn → CAN0_ORED_0_31_MB_IRQHandler
     *     → can.c 的 Can_IsrBridge → CanIf_RxIndication → PduR → CanTp
     * 所以接收不需要主循环轮询。
     *
     * 【这里不能塞阻塞操作】
     * CanTp 的 N_Bs/N_Cr 超时是按"主循环调用频率"来判定的，
     * 主循环被卡住几十毫秒（比如擦 Flash）期间，拼装会暂停 —— 这是设计上接受的，
     * 因为擦写只在诊断仪主动请求时发生，而那时对端也在等响应。
     *------------------------------------------------------------------------*/
    for (;;)
    {
        /* 推进 CanTp（超时检查、发流控帧、发连续帧）和 Dcm（S3 超时、发响应） */
        EcuM_MainFunction();

        /* 如果 CanTp 拼好了一条完整消息，路由给 Dcm 并拿到响应 */
        PduR_MainFunction();

        /* 调试上报（S3 超时、传输中止、原始帧、UDS 请求摘要）
         * 正式产品把 boot_trace.h 里的 BOOT_TRACE_ENABLE 改成 STD_OFF */
        BootTrace_MainFunction();
    }
}
