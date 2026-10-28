/*==================================================================================================
 *  Services/System/ecum.h —— 服务层：EcuM（ECU State Manager）接口
 *
 *  【本模块在 AUTOSAR 里的位置】
 *  AUTOSAR 的 Services Layer 里有两个"系统服务"模块：
 *      EcuM  —— 负责 ECU 的启动 / 关机 / 唤醒流程
 *      BswM  —— 负责运行期的模式仲裁
 *
 *  【本工程只实现"启动"这一半，而且没有状态机】
 *      EcuM_Init()          板级初始化 + BSW 模块按依赖顺序初始化
 *      EcuM_MainFunction()  裸机主循环调度（推进 CanTp / Dcm）
 *
 *  ⚠️ **这里刻意不实现睡眠/唤醒状态机**（STARTUP → RUN → POST_RUN → SLEEP → SHUTDOWN），
 *     因为 Bootloader 没有睡眠需求（刷写期间掉电就该重来）。
 *     挂个 EcuM 的名字却不做状态机，反而会让人误以为有那套机制。
 *
 *  【EcuM 存在的意义：初始化顺序有了唯一的家】
 *  和 BSW 模块之间有严格的依赖顺序：
 *      Clock → Port → IntCtrl → Det → Uart → Fls → Can → CanIf → PduR → CanTp → Dcm
 *  这个顺序以前埋在 main.c 里（占了两百多行），现在集中在 EcuM_Init() 一处。
 *  main.c 只剩两行，集成者不需要再记住顺序。
 *
 *  【和 CanGw 工程的 EcuM 的关系】
 *  那边有 FreeRTOS，所以多了 EcuM_StartOS() 和 EcuM_TaskCom()；
 *  本工程是裸机大循环，只有 EcuM_Init() + EcuM_MainFunction()。
 *================================================================================================*/
#ifndef ECUM_H
#define ECUM_H

#include "Std_Types.h"

/*==================================================================================================
 *                                        API
 *================================================================================================*/

/**
 * @brief 初始化整个 ECU（板级 + BSW）
 *
 * @details 按依赖顺序初始化所有模块，并打印每一步的结果。
 *          任何关键模块（Clock / Flash / CAN）初始化失败都会：
 *              打印中文错误 → 停在这里不再往下走（Bootloader 半死没有意义）
 */
void EcuM_Init(void);

/**
 * @brief 主循环调度
 *
 * @details 每圈调用，负责推进所有需要周期执行的 BSW 模块：
 *              CanTp_MainFunction()   —— 超时检查、发流控帧、发连续帧
 *              Dcm_MainFunction()     —— S3 会话超时检查、发响应
 *
 * @note 调用越快，CanTp 的 N_Bs/N_Cr 时序越准。主循环里不要再加阻塞操作。
 */
void EcuM_MainFunction(void);

#endif /* ECUM_H */
