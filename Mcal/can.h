/*==================================================================================================
 *  Mcal/can.h —— CAN MCAL 层对外接口
 *
 *  【本层职责】
 *  把 NXP RTD 的 FlexCAN 驱动，封装成符合 AUTOSAR Can SWS 的驱动接口。
 *
 *  【三条硬约束】
 *  1. 本文件**不得出现任何 FlexCAN_Ip_* / RTD 类型**。上层只能看到 Std_Types 与
 *     AUTOSAR 的 Can_* 类型。RTD 类型一律封在 can.c 里。
 *     原因：一旦上层 include 到 RTD 头文件，换 CAN 控制器（FlexCAN→MCAN）
 *     或换芯片，上层代码就要跟着改 —— 分层就白做了。
 *
 *  2. 上层用**逻辑句柄**（HTH 发送 / HRH 接收）寻址硬件对象，不直接使用物理邮箱号。
 *     物理邮箱的分配写在 can_cfg.c 的配置表里，由 can.c 查表翻译。
 *
 *  3. 本文件不包含任何"怎么配"的信息。邮箱怎么分、DLC 上限多少、哪个邮箱收哪个 ID，
 *     全部在 can_cfg.h / can_cfg.c 里。改配置不改逻辑。
 *
 *  【类型从哪来】
 *  Can_PduType / Can_HwHandleType / Can_IdType / Can_ControllerStateType /
 *  Can_ErrorStateType 都是 AUTOSAR 标准类型，由 SDK 的 Can_GeneralTypes.h 提供。
 *  **不要自己重定义** —— 上层（CanIf/PduR）要用同一份定义才能正确交换数据。
 *================================================================================================*/
#ifndef CAN_H
#define CAN_H

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "Std_Types.h"          /* uint8 / Std_ReturnType / E_OK / E_NOT_OK / NULL_PTR / TRUE / FALSE */
#include "Can_GeneralTypes.h"   /* Can_PduType / Can_HwHandleType / Can_IdType /
                                   Can_ControllerStateType / Can_ErrorStateType */

/*==================================================================================================
 *                                    DEFINES AND MACROS
 *================================================================================================*/

/*--------------------------------------------------------------------------------------------------
 *                    模块标识与开发错误检测（Det）配置
 *
 *  AUTOSAR 规定每个 BSW 模块都必须在自己的头文件里声明这几样东西。
 *  详见 canif.h 里同一段的完整说明。
 *------------------------------------------------------------------------------------------------*/

/** @brief Can 的标准 AUTOSAR 模块号（0x50 = 80） */
#define CAN_MODULE_ID                       (0x50U)

/** @brief 单实例模块，实例号固定 0（多控制器用 ApiId 的实例参数区分） */
#define CAN_INSTANCE_ID                     (0U)

/**
 * @brief 开发错误检测开关
 * @details 优先级：本模块单独定义 > 全局 BSW_DEV_ERROR_DETECT > 默认 STD_ON
 */
#ifndef CAN_DEV_ERROR_DETECT
    #ifdef BSW_DEV_ERROR_DETECT
        #define CAN_DEV_ERROR_DETECT        (BSW_DEV_ERROR_DETECT)
    #else
        #define CAN_DEV_ERROR_DETECT        (STD_ON)
    #endif
#endif

/** @brief Can 的 API 编号（Det 上报时用） */
typedef enum
{
    CAN_API_INIT                        = 0U,
    CAN_API_DEINIT                      = 1U,
    CAN_API_WRITE                       = 2U,
    CAN_API_SET_CONTROLLER_MODE         = 3U,
    CAN_API_GET_CONTROLLER_MODE         = 4U,
    CAN_API_GET_CONTROLLER_ERROR_STATE  = 5U,
    CAN_API_MAIN_FUNCTION_WRITE         = 6U,
    CAN_API_GET_VERSION_INFO            = 7U,
    CAN_API_SET_BAUDRATE                = 8U
} Can_ApiIdType;

/** @brief Can 的错误编号（Det 上报时用） */
typedef enum
{
    CAN_E_PARAM_POINTER         = 0x01U,    /**< 传了 NULL 指针 */
    CAN_E_UNINIT                = 0x02U,    /**< 未初始化就调用 API */
    CAN_E_PARAM_HANDLE          = 0x03U,    /**< 硬件句柄不存在（Hth / Hrh 不在配置表里） */
    CAN_E_PARAM_CONTROLLER      = 0x04U,    /**< 控制器号越界 */
    CAN_E_PARAM_DATA_LENGTH     = 0x05U,    /**< 数据长度超过该发送对象的能力 */
    CAN_E_PARAM_CONTROLLER_MODE = 0x06U,    /**< 请求了不支持的控制器模式 */
    CAN_E_TRANSMIT_BUFFER_BUSY  = 0x07U,    /**< 发送缓冲满（正常拥塞，一般不报 Det） */
    CAN_E_INVALID_CONFIG        = 0x08U     /**< 配置表内部自相矛盾 */
} Can_ErrorIdType;

/*--------------------------------------------------------------------------------------------------
 *  Can_Write() 的叠加返回值
 *------------------------------------------------------------------------------------------------*/

/**
 * @brief   Can_Write() 的叠加返回值（Overlayed return value）
 * @details SWS_Can_00039 规定：Can_Write() 的返回类型名义上是 Std_ReturnType，
 *          但允许叠加 CAN_BUSY 表示"暂时没有可用的发送对象"。
 *
 *          CAN_BUSY 本身已由 Can_GeneralTypes.h 定义，这里**不重复定义**，
 *          只提供一个强转的便捷宏。
 *
 *          为什么必须有它：Std_ReturnType 只有 E_OK / E_NOT_OK 两个值，
 *          无法区分下面两种性质完全不同的失败：
 *            - CAN_BUSY  ：暂时没空 → 上层应当"稍后重试"
 *            - E_NOT_OK  ：参数非法 / 未初始化 / 句柄不存在 → 上层应当"报错"
 *          把 CAN_BUSY 折叠成 E_NOT_OK，等于强迫上层用同一种策略处理两类问题：
 *          要么无脑重试（真错误时死循环），要么无脑报错（正常拥塞当故障）。
 */
#define CAN_BUSY_RET    ((Std_ReturnType)CAN_BUSY)

/*==================================================================================================
 *                                 STRUCTURES AND OTHER TYPEDEFS
 *================================================================================================*/

/**
 * @brief CAN 驱动配置类型的前向声明
 *
 * 【为什么这里只声明、不定义】
 * 配置结构体（Controllers / TxObjects / RxObjects 三张表）的完整定义在 can_cfg.h。
 * 而 can.h 必须能声明 Can_Init(const Can_ConfigType*) —— 它只需要知道
 * "有这么个类型"，不需要知道里面有什么字段。
 *
 * 用不完整类型声明指针参数是 C 语言允许的，好处是：
 *   **上层只想调用 Can_Init() 时，不必知道配置结构体的内部布局。**
 * 完整定义在 can_cfg.h，那是"配置层"的事。
 *
 * 【注意这两行的区别 —— 这里踩过一次坑】
 *     struct Can_ConfigType;                      ← 只声明 struct tag
 *     typedef struct Can_ConfigType Can_ConfigType;  ← 同时建立类型别名
 *
 * 只写第一行的话，`Can_ConfigType` 这个**类型名并不存在**，
 * 必须写 `struct Can_ConfigType` 才能用。而本工程（以及 AUTOSAR 惯例）
 * 是用不带 struct 的 `Can_ConfigType`，所以**两行都必须有**。
 * 编译期会直接报 "unknown type name 'Can_ConfigType'"，不会静默出错。
 */
struct Can_ConfigType;
typedef struct Can_ConfigType Can_ConfigType;

/*==================================================================================================
 *                                 GLOBAL FUNCTION PROTOTYPES
 *================================================================================================*/

/**
 * @brief       初始化 CAN 驱动
 * @details     按配置表逐个初始化控制器、配置并挂载接收邮箱。
 *              调用后控制器处于 CAN_CS_STOPPED 状态，需要
 *              Can_SetControllerMode(x, CAN_CS_STARTED) 才会真正上总线。
 *
 * @param[in]   Config 配置对象（由 can_cfg.c 提供 Can_Config）
 */
void Can_Init(const Can_ConfigType* Config);

/**
 * @brief       反初始化 CAN 驱动
 * @details     停止所有控制器并释放驱动状态。
 */
void Can_DeInit(void);

/**
 * @brief       切换控制器模式
 *
 * @param[in]   Controller   控制器编号（0 = CAN0，1 = CAN1）
 * @param[in]   Transition   目标模式：CAN_CS_STARTED / CAN_CS_STOPPED / CAN_CS_SLEEP
 * @return      E_OK 成功；E_NOT_OK 失败（参数非法、控制器不存在、或该模式不支持）
 *
 * @note        CAN_CS_SLEEP 当前**明确返回 E_NOT_OK**（未实现），
 *              而不是"不改硬件却返回 E_OK"。后者会让上层以为控制器真的睡了，
 *              而总线上其实还在活动 —— 这类"接口撒谎"比不实现更危险。
 */
Std_ReturnType Can_SetControllerMode(uint8 Controller, Can_ControllerStateType Transition);

/**
 * @brief       查询控制器当前模式
 *
 * @param[in]   Controller   控制器编号
 * @param[out]  ModePtr      返回当前模式
 * @return      E_OK 成功；E_NOT_OK 参数非法
 */
Std_ReturnType Can_GetControllerMode(uint8 Controller, Can_ControllerStateType* ModePtr);

/**
 * @brief       发送一帧 CAN 报文
 *
 * @param[in]   Hth        发送硬件对象句柄（逻辑句柄，取值见 can_cfg.h）
 * @param[in]   PduInfo    待发送的 PDU：id（含帧类型标志）、length、sdu
 * @return      E_OK        发送请求已被接受
 *              CAN_BUSY    暂时无可用发送对象，上层可稍后重试
 *              E_NOT_OK    参数非法 / 未初始化 / 句柄不存在 / 控制器未启动
 *
 * @note        本函数**不做阻塞等待**，把请求交给硬件后立即返回。
 *              真正的"发送完成"通过 TxConfirmation hook 异步通知。
 */
Std_ReturnType Can_Write(Can_HwHandleType Hth, const Can_PduType* PduInfo);

/**
 * @brief       查询控制器错误状态
 *
 * @param[in]   Controller        控制器编号
 * @param[out]  ErrorStatePtr     返回错误状态（ACTIVE / PASSIVE / BUSOFF）
 * @return      E_OK 成功；E_NOT_OK 参数非法
 *
 * @note        返回值必须反映**真实**状态。恒返回 ACTIVE 属于"接口撒谎"，
 *              会让上层的故障管理（DTC）永远收不到错误信息。
 */
Std_ReturnType Can_GetControllerErrorState(uint8 Controller, Can_ErrorStateType* ErrorStatePtr);

/**
 * @brief       周期处理函数（轮询模式与 Bus-off 恢复检查）
 * @details     本工程当前所有硬件对象都用中断模式，因此这三个函数暂时为空实现。
 *              保留它们的原因：
 *                - 引入轮询邮箱后，实现在这里
 *                - Bus-off 退出检测应放在 Can_MainFunction_BusOff()
 *                  （FlexCAN 的自动恢复不产生中断，只能靠轮询发现）
 *                - 上层（服务层）会按固定周期调用它们
 */
void Can_MainFunction_Read(void);
void Can_MainFunction_Write(void);
void Can_MainFunction_BusOff(void);

/*==================================================================================================
 *                        向上通知接口（CanIf 未就位前的过渡机制）
 *
 * 【这些 hook 将来会被替换成什么】
 * 每一个 hook 都对应 AUTOSAR CanIf 的一个正式回调。等"ECU 抽象层"的 CanIf 就位后，
 * 把 hook 换成 CanIf 的接口调用即可，**can.c 内部逻辑不需要改**。
 *
 * 【调用上下文 —— 必须知道】
 * 所有 hook 都在 **CAN 中断上下文**里被调用。因此：
 *   - hook 实现必须非阻塞，不能做耗时操作（不能解析、不能打印）
 *   - 接入 FreeRTOS 后，hook 里只能用 ...FromISR 系列的 API
 *   - 对应中断的优先级必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
 *     （本工程该值为 1，CAN 中断配的是 6，满足条件）
 *================================================================================================*/

/** @brief 收到一帧有效报文时调用 */
typedef void (*Can_RxIndicationHookType)(Can_HwHandleType Hrh, const Can_PduType* Pdu);

/** @brief 一帧报文发送完成时调用 */
typedef void (*Can_TxConfirmationHookType)(Can_HwHandleType Hth);

/** @brief 控制器进入 Bus-off 时调用 */
typedef void (*Can_ControllerBusOffHookType)(uint8 Controller);

/** @brief 控制器错误状态发生迁移时调用 */
typedef void (*Can_ControllerErrorStateHookType)(uint8 Controller,
                                                 Can_ErrorStateType OldState,
                                                 Can_ErrorStateType NewState);

void Can_RegisterRxHook(Can_RxIndicationHookType hook);
void Can_RegisterTxHook(Can_TxConfirmationHookType hook);
void Can_RegisterBusOffHook(Can_ControllerBusOffHookType hook);
void Can_RegisterErrorStateHook(Can_ControllerErrorStateHookType hook);

#endif /* CAN_H */
