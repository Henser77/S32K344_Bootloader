/*==================================================================================================
 *  Mcal/can.c —— CAN MCAL 层实现
 *
 *  【本文件是"胶水层"】
 *  上面是 AUTOSAR Can SWS 定义的接口（can.h），下面是 NXP RTD 的 FlexCAN 驱动。
 *  本文件的全部工作就是**在两个世界之间做翻译**：
 *
 *     上层语义                        RTD 语义
 *     ─────────────────────────────────────────────────────────
 *     逻辑句柄 Hth / Hrh       ←→    物理控制器号 + 物理邮箱号
 *     Can_PduType                    Flexcan_Ip_DataInfoType + 裸 ID + 数据指针
 *     Can_ControllerStateType  ←→    RTD 的 Start/Stop 模式
 *     Can_IdType（含帧类型位）   ←→    裸 CAN ID + 帧类型枚举
 *
 *  【RTD 类型只出现在本文件】
 *  can.h 与 can_cfg.h 都不 include RTD 头文件。这样上层（ECU 抽象层 / 服务层）
 *  include can.h 时，不会被动地依赖 RTD —— 换控制器或换芯片时上层不用改。
 *
 *  【本文件是唯一实现 FlexCAN0_Callback / FlexCAN1_Callback 的地方】
 *  CT 生成的 generate/src/FlexCAN_Ip_Sa_PBcfg.c 里把这两个函数名填进了
 * FlexCAN_Config0/1 的 .Callback 字段（extern 声明在那里，实现在这里）。
 *  也就是说：**RTD 收到硬件事件后，回调进本文件**，我们再做语义翻译后往上报。
 *  这正是 MCAL 层"向下对接驱动、向上提供服务"的位置。
 *================================================================================================*/

/*==================================================================================================
 *                                        INCLUDES
 *================================================================================================*/
#include "can.h"
#include "can_cfg.h"

/* RTD 的 FlexCAN 驱动 —— 只在本文件出现 */
#include "FlexCAN_Ip.h"

/* Det（开发错误追踪）。Det 是"叶子模块"，不依赖任何业务层，
 * 所以 MCAL 引用它**不破坏分层**（和引用 Std_Types.h 是同一性质）。 */
#include "Det.h"

/*==================================================================================================
 *                                    LOCAL DEFINES
 *================================================================================================*/

/**
 * @brief Det 上报的便捷宏
 * @details DEV_ERROR_DETECT = STD_OFF 时整个调用被预处理器删成 ((void)0)。
 *          说明见 canif.c 里同名宏。
 */
#if (CAN_DEV_ERROR_DETECT == STD_ON)
    #define CAN_DET_ERROR(api, err)                                     \
        ((void)Det_ReportError((uint16)CAN_MODULE_ID,                   \
                               (uint8)CAN_INSTANCE_ID,                  \
                               (uint8)(api),                            \
                               (uint8)(err)))
#else
    #define CAN_DET_ERROR(api, err)     ((void)0)
#endif

/**
 * @brief 本层为每个控制器预留的接收邮箱缓冲数量
 *
 * 【为什么需要这个宏】
 * RTD 的接收 API 需要调用者提供一个**持久存在**的缓冲结构（消息缓冲区），
 * 不能是栈上的临时变量 —— 因为硬件会在任意时刻往里写数据。
 * 所以本层为每个控制器静态分配一组缓冲。
 *
 * 【硬约束】can_cfg.c 里所有接收对象的 MbIdx 必须 < 本值，
 * 否则 can.c 的边界检查会拒收该邮箱的中断（见 Can_RxIsrBridge）。
 * CAN0 硬件支持 96 个邮箱、CAN1 支持 64 个，但本工程只用到邮箱 0..7，
 * 所以预留 8 个即可。要用更高编号的邮箱时，**必须同步放大本值**。
 */
#define CAN_MB_BUF_COUNT            (8U)

/*--------------------------------------------------------------------------------------------------
 * Can_IdType 的位定义
 *
 * Can_IdType 不只是裸 CAN ID，它用高两位携带帧类型信息：
 *     bit31 = 1  扩展帧（29 位 ID）
 *     bit30 = 1  CAN FD 帧
 *     bit29..0   实际 CAN ID
 *
 * RTD 的 msgId 只含裸 ID，所以收发两个方向都要在这里补/剥标志位。
 * 官方参考（Can_GeneralTypes.h 对 Can_IdType 的说明）与
 * NXP 官方 CanIf 的 CanIf_Transmit() 里的处理方式一致。
 *------------------------------------------------------------------------------------------------*/
#define CAN_ID_EXT_MASK             (0x80000000UL)
#define CAN_ID_FD_MASK              (0x40000000UL)
#define CAN_ID_ID_MASK              (0x1FFFFFFFUL)

/*--------------------------------------------------------------------------------------------------
 * 接收掩码寄存器的硬件位布局
 *
 * FlexCAN 的 RXIMR（个体掩码）不是"裸 ID 掩码"，它逐位对应接收帧的几个字段：
 *     bit31   →  IDE（0=标准帧，1=扩展帧）是否参与比较
 *     bit30   →  RTR（数据帧/远程帧）是否参与比较
 *     标准帧  →  11 位 ID 位于 bit28..18（移位量 18）
 *     扩展帧  →  29 位 ID 位于 bit28..0 （不移位）
 *
 * 所以配置表里给人的"ID 空间掩码"（0x7FF / 0x1FFFFFFF）必须由本层
 * 移位并叠加 IDE/RTR 位之后才能写进寄存器。
 *
 * 这套做法与 NXP 官方 Can_43_FLEXCAN 生成模板里的算法一致：
 *     CanHwFilterMask = 0xC0000000                              （IDE + RTR 参与比较）
 *     标准帧：+ (filter_mask << 18)
 *     扩展帧：+ (filter_mask << 0)
 * （见 Can_43_FLEXCAN_TS_T40D34M70I1R0/generate_PC/src/Can_43_FLEXCAN_PBcfg.c
 *   里 CanHwFilterMask 的计算，以及 FlexCAN_Ip_HwAccess.h:1419
 *   FlexCAN_SetRxIndividualMask() 直接写 RXIMR、不做任何移位）
 *------------------------------------------------------------------------------------------------*/
#define CAN_RXIMR_IDE_RTR_MASK      (0xC0000000UL)
#define CAN_RXIMR_STD_SHIFT         (18UL)
#define CAN_RXIMR_STD_IDMASK_MAX    (0x7FFUL)
#define CAN_RXIMR_EXT_IDMASK_MAX    (0x1FFFFFFFUL)

/*--------------------------------------------------------------------------------------------------
 * 接收邮箱 CS 字里的 FD 标志位
 *
 * 【为什么这里要自己定义一份】
 * 该位的定义在 FlexCAN_Ip_HwAccess.h 的 FLEXCAN_IP_MB_EDL_MASK，
 * 但 FlexCAN_Ip.h **不包含** FlexCAN_Ip_HwAccess.h（实测确认：
 * 直接引用会报 'FLEXCAN_IP_MB_EDL_MASK' undeclared）。
 * 为了不额外 include RTD 的私有头文件，这里按相同数值定义一份，
 * 数值来源标注在下面。
 *
 * 【这个位是干什么的】
 * 接收到的帧如果是 CAN FD，硬件会把该帧邮箱 CS 字的 EDL 位置 1。
 * 所以收到中断后读 msg->cs 就能判断"这帧是不是 FD"，
 * 据此在 Can_PduType.id 里补 CAN_ID_FD_MASK（bit30）上报给上层。
 *
 * 写入侧对称：FlexCAN_SetTxMsgBuff() 在 Cs->fd_enable 为真时
 * 也会置这一个位（FlexCAN_Ip_HwAccess.c:1704）。
 *------------------------------------------------------------------------------------------------*/
#define CAN_RX_CS_FD_MASK           (0x80000000UL)   /* = FLEXCAN_IP_MB_EDL_MASK (HwAccess.h:186) */

/*==================================================================================================
 *                                    LOCAL VARIABLES
 *================================================================================================*/

/** @brief 当前生效的配置对象（Can_Init 传入） */
static const Can_ConfigType* Can_DriverConfig = NULL_PTR;

/** @brief 初始化完成标志。用于 Can_Write 等 API 的前置检查 */
static boolean Can_DriverInitialized = FALSE;

/** @brief 各控制器的当前模式 */
static Can_ControllerStateType Can_ControllerMode[CAN_CONTROLLER_COUNT];

/** @brief 各控制器的当前错误状态（由真实硬件事件驱动更新） */
static Can_ErrorStateType Can_ControllerError[CAN_CONTROLLER_COUNT];

/**
 * @brief 各控制器各邮箱的接收缓冲（静态分配）
 * @details 硬件在中断里直接往里写，因此必须是静态存储期、不能是栈变量。
 */
static Flexcan_Ip_MsgBuffType Can_RxMsgBuffer[CAN_CONTROLLER_COUNT][CAN_MB_BUF_COUNT];

/** @brief 向上通知的 hook（CanIf 就位后由正式接口替换） */
static Can_RxIndicationHookType         Can_RxHook         = NULL_PTR;
static Can_TxConfirmationHookType       Can_TxHook         = NULL_PTR;
static Can_ControllerBusOffHookType     Can_BusOffHook     = NULL_PTR;
static Can_ControllerErrorStateHookType Can_ErrorStateHook = NULL_PTR;

/*==================================================================================================
 *                              LOCAL FUNCTION PROTOTYPES
 *================================================================================================*/
static const Can_TxHwObjectConfigType* Can_FindTxObjectByHth(Can_HwHandleType Hth);
static const Can_TxHwObjectConfigType* Can_FindTxObjectByHw(uint8 Controller, uint32 MbIdx);
static const Can_RxHwObjectConfigType* Can_FindRxObjectByHw(uint8 Controller, uint32 MbIdx);
static void Can_UpdateErrorState(uint8 Controller, Can_ErrorStateType NewState);
static void Can_ConfigRxMailboxes(const Can_ConfigType* Config);
static void Can_IsrBridge(uint8 instance, Flexcan_Ip_EventType eventType,
                          uint32 buffIdx, const Flexcan_Ip_StateType* flexcanState);

/*==================================================================================================
 *                    RTD 回调实现（CT 生成的配置里引用的符号）
 *
 * 【调用时机】RTD 在 CAN 中断里调用它们。事件类型由 eventType 区分。
 *
 * 【关键陷阱 —— 第三个参数 buffIdx 的含义随事件类型改变】
 *   FLEXCAN_EVENT_RX_COMPLETE / TX_COMPLETE : buffIdx 是**邮箱索引**
 *   其余事件（ERROR / BUSOFF / RX_WARNING / TX_WARNING …）: buffIdx 是**ESR1 寄存器值**
 *
 * 因此**绝不能在入口处统一把 buffIdx 当邮箱号用**。下面两个入口都只做转发，
 * 由 Can_IsrBridge 先按事件类型分派、再解释第三参数。
 *================================================================================================*/
void FlexCAN0_Callback(uint8 instance, Flexcan_Ip_EventType eventType,
                       uint32 buffIdx, const Flexcan_Ip_StateType* flexcanState)
{
    Can_IsrBridge(instance, eventType, buffIdx, flexcanState);
}

void FlexCAN1_Callback(uint8 instance, Flexcan_Ip_EventType eventType,
                       uint32 buffIdx, const Flexcan_Ip_StateType* flexcanState)
{
    Can_IsrBridge(instance, eventType, buffIdx, flexcanState);
}

/*--------------------------------------------------------------------------------------------------
 * 错误回调 —— 由 CT 配置指向本文件
 *
 * 【分工】CT 负责"配置指向谁"，本文件负责"被指向时做什么"。
 *
 *   .mex 里：  FlexCanErrorCallback = FlexCAN0_ErrorCallback / FlexCAN1_ErrorCallback
 *   生成后：  generate/src/FlexCAN_Ip_Sa_PBcfg.c
 *               :100 / :105   extern 声明（签名必须与本文件实现一致）
 *               :218 / :309   &FlexCAN0_ErrorCallback / &FlexCAN1_ErrorCallback
 *   运行时：  FlexCAN_Ip_Init 把该地址存进 pState->error_callback
 *             （FlexCAN_Ip.c:2188）
 *   事件时：  RTD 在 FlexCAN_Ip.c:2059 调用
 *               pState->error_callback(Instance, ErrEvent, u32ErrStatus, pState)
 *             → 进入下面这两个函数
 *
 * 【为什么这两个函数不能被省略】
 * 如果 CT 里 FlexCanErrorCallback 留成 NULL_PTR，RTD 会在 2059 行判空跳过，
 * 于是 Bus-off / Error / Warning 事件**全部静默丢弃** ——
 * 控制器掉线并自动恢复，上层毫不知情，而且**编译链接都不会报错**。
 * 这是本层最容易漏、最难查的一个点。
 *
 * 【改 CT 配置时的检查项】
 *   - FlexCanCallback      → 必须填 FlexCAN0_Callback / FlexCAN1_Callback
 *   - FlexCanErrorCallback → 必须填 FlexCAN0_ErrorCallback / FlexCAN1_ErrorCallback
 *   （填错名字会链接报 undefined reference，那个好查；
 *     忘记填则静默丢事件，这个难查 —— 所以记在这里）
 *------------------------------------------------------------------------------------------------*/
void FlexCAN0_ErrorCallback(uint8 instance, Flexcan_Ip_EventType eventType,
                            uint32 errStatus, const Flexcan_Ip_StateType* flexcanState)
{
    /* 注意第三个参数在这里是 **ESR1 寄存器值**，不是邮箱号。
     * Can_IsrBridge 会按事件类型区分处理，不会误当邮箱号索引数组。 */
    Can_IsrBridge(instance, eventType, errStatus, flexcanState);
}

void FlexCAN1_ErrorCallback(uint8 instance, Flexcan_Ip_EventType eventType,
                            uint32 errStatus, const Flexcan_Ip_StateType* flexcanState)
{
    Can_IsrBridge(instance, eventType, errStatus, flexcanState);
}

/*==================================================================================================
 *                    编译期类型守卫 —— 防止回调签名与 RTD/CT 悄悄漂移
 *
 * 【为什么需要它】
 * 这四个回调的"声明"在 CT 生成的 generate/src/FlexCAN_Ip_Sa_PBcfg.c 里，
 * "定义"在本文件里 —— 落在**不同的编译单元**。C 语言的规则是：
 *
 *     编译器只检查各自文件内部，不交叉比对
 *     链接器只按符号名匹配，不检查签名
 *
 * 于是"同名 + 签名不同"能编译、能链接、运行期是未定义行为。举例：
 *     - 漏掉 const（Flexcan_Ip_StateType* 而不是 const Flexcan_Ip_StateType*）
 *     - buffIdx 写成 uint8（位宽错，邮箱号被截断）
 *     - 参数顺序颠倒（eventType 和 buffIdx 被互换解释）
 * 以上都能链接成功，且编译器一声不响。这类问题极难在运行期定位。
 *
 * 【做法】
 * 用 GCC 的 __builtin_types_compatible_p 在编译期比较"我们的函数类型"与
 * "RTD 配置结构体字段要求的类型"（FlexCAN_Ip_CallbackType /
 * FlexCAN_Ip_ErrorCallbackType，定义在 FlexCAN_Ip_Types.h:526/535）。
 * 不一致时数组长度为负 → 编译报错。
 *
 * 用 typedef + 负长度数组而不是 C11 的 _Static_assert，
 * 是因为本工程编译选项是 -std=c99（见 .cproject 的 Language standard）。
 *
 * 【这组断言能查什么、不能查什么 —— 必须说清，否则会误以为已全覆盖】
 *
 *   ✅ 能查：参数个数错、参数类型错（含漏掉 const）、返回值类型错。
 *           已验证有效性：故意换成一个四参数全 uint8 的函数指针类型，
 *           编译报 "size of array is negative"。
 *
 *   ❌ 不能查：**普通回调与错误回调被互换**。原因是本 RTD 版本里
 *           FlexCAN_Ip_CallbackType 与 FlexCAN_Ip_ErrorCallbackType
 *           **参数类型完全相同**（只有第 3 个参数的名字不同：
 *           buffIdx vs u32ErrStatus，而参数名不影响类型兼容性），
 *           所以编译器认为这两个类型是同一个，互换也查不出来。
 *
 *           已实测确认两者 __builtin_types_compatible_p 为真。
 *
 *           那"两个回调落到对的字段里"怎么保证？靠**位置初始化 + 字段顺序**
 *           正确。本项目已用重定位偏移验证过：
 *               generate/src/FlexCAN_Ip_Sa_PBcfg.c 里
 *               &FlexCAN0_Callback      @ 0x50 == offsetof(Callback)
 *               &FlexCAN0_ErrorCallback @ 0x54 == offsetof(ErrorCallback)
 *           （sizeof(Flexcan_Ip_ConfigType) = 0x58，FlexCAN1 的落在 +0x58）
 *           若将来 CT 改了字段顺序，重定位会变，但编译器不会提醒 ——
 *           所以**重新 Generate 后要复核这一项**。
 *================================================================================================*/
#if defined(__GNUC__) && !defined(__ICCARM__)
typedef char Can_CbSigAssert_FlexCAN0_Callback[
    __builtin_types_compatible_p(__typeof__(&FlexCAN0_Callback),
                                 FlexCAN_Ip_CallbackType) ? 1 : -1];
typedef char Can_CbSigAssert_FlexCAN1_Callback[
    __builtin_types_compatible_p(__typeof__(&FlexCAN1_Callback),
                                 FlexCAN_Ip_CallbackType) ? 1 : -1];
typedef char Can_CbSigAssert_FlexCAN0_ErrorCallback[
    __builtin_types_compatible_p(__typeof__(&FlexCAN0_ErrorCallback),
                                 FlexCAN_Ip_ErrorCallbackType) ? 1 : -1];
typedef char Can_CbSigAssert_FlexCAN1_ErrorCallback[
    __builtin_types_compatible_p(__typeof__(&FlexCAN1_ErrorCallback),
                                 FlexCAN_Ip_ErrorCallbackType) ? 1 : -1];
#endif /* __GNUC__ */

/*==================================================================================================
 *                              LOCAL FUNCTIONS —— 查表
 *
 * 【本层的核心动作就是"查表"】
 * 上层给一个逻辑句柄，本层查表得到物理位置。不做任何推导。
 *================================================================================================*/

/**
 * @brief Hth（逻辑发送句柄）→ 发送硬件对象
 */
static const Can_TxHwObjectConfigType* Can_FindTxObjectByHth(Can_HwHandleType Hth)
{
    uint8 i;

    if (Can_DriverConfig == NULL_PTR)
    {
        return NULL_PTR;
    }

    for (i = 0U; i < Can_DriverConfig->TxObjectCount; i++)
    {
        if (Can_DriverConfig->TxObjects[i].Hth == Hth)
        {
            return &Can_DriverConfig->TxObjects[i];
        }
    }
    return NULL_PTR;
}

/**
 * @brief (控制器, 物理邮箱) → 发送硬件对象
 * @details 用于 TX 完成事件：RTD 报上来的是邮箱号，需要反查回逻辑句柄。
 *
 *          【为什么必须带上控制器号一起查】
 *          不同控制器的邮箱编号是各自独立的（CAN0 和 CAN1 都有"邮箱 1"）。
 *          只用邮箱号查会串台 —— 把 CAN1 的发送完成认成 CAN0 的。
 */
static const Can_TxHwObjectConfigType* Can_FindTxObjectByHw(uint8 Controller, uint32 MbIdx)
{
    uint8 i;

    if (Can_DriverConfig == NULL_PTR)
    {
        return NULL_PTR;
    }

    for (i = 0U; i < Can_DriverConfig->TxObjectCount; i++)
    {
        if ((Can_DriverConfig->TxObjects[i].ControllerId == Controller) &&
            ((uint32)Can_DriverConfig->TxObjects[i].MbIdx == MbIdx))
        {
            return &Can_DriverConfig->TxObjects[i];
        }
    }
    return NULL_PTR;
}

/**
 * @brief 取某个控制器的 RTD 配置对象指针（用于查询 FD 能力等硬件属性）
 * @details 这是本层"必要时回查 RTD 配置"的唯一入口。
 *
 *          为什么要回查：FD 能力不是我们 MCAL 配置表里的字段，而是 RTD 配置
 *          （CT 生成的 Flexcan_Ip_ConfigType）里的 fd_enable。两处各存一份
 *          相同事实会造成"两套并行事实"，迟早不一致 —— 所以只认 RTD 那一份。
 *
 * @return 配置指针；控制器号越界或未配置时返回 NULL_PTR
 */
static const Flexcan_Ip_ConfigType* Can_GetRtdControllerConfig(uint8 Controller)
{
    if ((Can_DriverConfig == NULL_PTR) || (Controller >= Can_DriverConfig->ControllerCount))
    {
        return NULL_PTR;
    }
    return (const Flexcan_Ip_ConfigType*)Can_DriverConfig->Controllers[Controller].RtdConfig;
}

/**
 * @brief (控制器, 物理邮箱) → 接收硬件对象
 */
static const Can_RxHwObjectConfigType* Can_FindRxObjectByHw(uint8 Controller, uint32 MbIdx)
{
    uint8 i;

    if (Can_DriverConfig == NULL_PTR)
    {
        return NULL_PTR;
    }

    for (i = 0U; i < Can_DriverConfig->RxObjectCount; i++)
    {
        if ((Can_DriverConfig->RxObjects[i].ControllerId == Controller) &&
            ((uint32)Can_DriverConfig->RxObjects[i].MbIdx == MbIdx))
        {
            return &Can_DriverConfig->RxObjects[i];
        }
    }
    return NULL_PTR;
}

/*==================================================================================================
 *                              LOCAL FUNCTIONS —— 错误状态机
 *
 * 【为什么需要状态机，而不是直接转发事件】
 * RTD 报的是偏硬件语义的事件（RX_WARNING / TX_WARNING / BUSOFF），
 * 而 AUTOSAR 要求上报的是**控制器错误状态**（Can_ErrorStateType）。
 * 两者不是一一对应，必须由 MCAL 层收敛：
 *
 *     RX_WARNING / TX_WARNING   错误计数器 >= 96  →  逼近 Error Passive
 *     BUSOFF                    控制器进入 Bus-off
 *
 * 【诚实说明局限】
 * FlexCAN 在计数器 >= 96 时就发 warning，而 Error Passive 的规范阈值是 >= 128。
 * 因此本实现把 warning 事件**提前映射**为 PASSIVE。严格判定需要读 ECR 寄存器的
 * 收发计数器，而 RTD 的公开头文件里没有该 API。这是一个已知的近似，
 * 将来可通过直接访问寄存器精确化。
 *================================================================================================*/
static void Can_UpdateErrorState(uint8 Controller, Can_ErrorStateType NewState)
{
    Can_ErrorStateType OldState;

    if (Controller >= CAN_CONTROLLER_COUNT)
    {
        return;
    }

    OldState = Can_ControllerError[Controller];

    /* 状态没变就不上报 —— 避免重复事件刷屏 */
    if (OldState == NewState)
    {
        return;
    }

    Can_ControllerError[Controller] = NewState;

    if (Can_ErrorStateHook != NULL_PTR)
    {
        Can_ErrorStateHook(Controller, OldState, NewState);
    }
}

/*==================================================================================================
 *                        Can_IsrBridge —— 中断事件桥接（本层最关键的函数）
 *
 * 把 RTD 的硬件事件翻译成 AUTOSAR 语义，并向上通知。
 *================================================================================================*/
static void Can_IsrBridge(uint8 instance, Flexcan_Ip_EventType eventType,
                          uint32 buffIdx, const Flexcan_Ip_StateType* flexcanState)
{
    /* 控制器号越界 —— 不是本层管理的控制器，直接丢弃 */
    if (instance >= CAN_CONTROLLER_COUNT)
    {
        return;
    }

    switch (eventType)
    {
        /*------------------------------------------------------------------------------------------
         * 发送完成 → 向上报告"发送确认"
         *
         * 【为什么这个分支必须有】
         * AUTOSAR 上层的发送确认（CanIf_TxConfirmation）就靠这个事件驱动。
         * 若把它丢弃，上层永远不知道一帧有没有真正发出去 ——
         * 重传、超时、发送队列全都无从实现。
         *----------------------------------------------------------------------------------------*/
        case FLEXCAN_EVENT_TX_COMPLETE:
        {
            /* 此处 buffIdx 语义确定：邮箱索引 */
            if (buffIdx < (uint32)CAN_MB_BUF_COUNT)
            {
                const Can_TxHwObjectConfigType* txObj = Can_FindTxObjectByHw(instance, buffIdx);

                /* 邮箱不在发送表里 → 不是本层发出的帧，忽略（不猜测） */
                if ((txObj != NULL_PTR) && (Can_TxHook != NULL_PTR))
                {
                    Can_TxHook(txObj->Hth);
                }
            }
            break;
        }

        /*------------------------------------------------------------------------------------------
         * 接收完成 → 向上报告"接收指示"
         *----------------------------------------------------------------------------------------*/
        case FLEXCAN_EVENT_RX_COMPLETE:
        {
            /* 此处 buffIdx 语义确定：邮箱索引 */
            if (buffIdx < (uint32)CAN_MB_BUF_COUNT)
            {
                const Can_RxHwObjectConfigType* rxObj = Can_FindRxObjectByHw(instance, buffIdx);

                if ((rxObj != NULL_PTR) && (flexcanState != NULL_PTR))
                {
                    const Flexcan_Ip_MsgBuffType* msg = flexcanState->mbs[buffIdx].pMBmessage;

                    if (msg != NULL_PTR)
                    {
                        Can_PduType pdu;

                        pdu.swPduHandle = (PduIdType)rxObj->Hrh;
                        pdu.length      = msg->dataLen;
                        pdu.sdu         = (uint8*)msg->data;

                        /* 构造 Can_IdType：裸 ID + 帧类型标志位。
                         * 上层（CanIf/PduR）靠 bit31 区分标准帧与扩展帧，
                         * 这里不补就等于丢信息。 */
                        pdu.id = ((uint32)msg->msgId) & CAN_ID_ID_MASK;
                        if (rxObj->IsExtended != FALSE)
                        {
                            pdu.id |= CAN_ID_EXT_MASK;
                        }

                        /* FD 标志：不能靠配置推，必须看**这一帧实际收到的格式**。
                         *
                         * 硬件会在 FD 帧的邮箱 CS 字里把 EDL 位置 1
                         * （FlexCAN_Ip_HwAccess.c:1704 是写入侧的对称操作）。
                         * 所以直接读 CS 字判断，而不是查配置表 ——
                         * 一条 FD 总线上完全可能既收到经典帧又收到 FD 帧，
                         * 报文格式是"帧的属性"，不是"邮箱的属性"。
                         *
                         * 现在 CT 里 fd_enable = FALSE，这个位永远不会置起来，
                         * 本行等价于不变；将来开 FD 后自动生效，无需改动。 */
                        if ((msg->cs & CAN_RX_CS_FD_MASK) != 0U)
                        {
                            pdu.id |= CAN_ID_FD_MASK;
                        }

                        if (Can_RxHook != NULL_PTR)
                        {
                            Can_RxHook(rxObj->Hrh, &pdu);
                        }
                    }
                }

                /* 重新挂载该邮箱，准备下一次接收。
                 *
                 * 【必须放在 rxObj != NULL_PTR 判断之内】
                 * 只有**属于接收对象表**的邮箱才应该被重新挂载。
                 * 若放在判断之外（只受 buffIdx < CAN_MB_BUF_COUNT 约束），
                 * 那么任何落在该区间的邮箱中断都会触发一次挂载 ——
                 * 包括发送邮箱和根本没配置的邮箱，会把它们意外"武装"成接收邮箱，
                 * 改变硬件行为。 */
                if (rxObj != NULL_PTR)
                {
                    (void)FlexCAN_Ip_Receive(instance, (uint8)buffIdx,
                                             &Can_RxMsgBuffer[instance][buffIdx], FALSE);
                }
            }
            break;
        }

        /*------------------------------------------------------------------------------------------
         * Bus-off → 更新错误状态 + 向上通知
         *----------------------------------------------------------------------------------------*/
        case FLEXCAN_EVENT_BUSOFF:
        {
            /* 本分支 buffIdx 是 ESR1 值，不得当邮箱号使用 */
            Can_UpdateErrorState(instance, CAN_ERRORSTATE_BUSOFF);

            if (Can_BusOffHook != NULL_PTR)
            {
                Can_BusOffHook(instance);
            }
            break;
        }

        /*------------------------------------------------------------------------------------------
         * 错误计数器告警 → 逼近 Error Passive
         *----------------------------------------------------------------------------------------*/
        case FLEXCAN_EVENT_RX_WARNING:
        case FLEXCAN_EVENT_TX_WARNING:
        {
            /* buffIdx 是 ESR1 值，不使用 */
            Can_UpdateErrorState(instance, CAN_ERRORSTATE_PASSIVE);
            break;
        }

        /*------------------------------------------------------------------------------------------
         * 其它错误事件：暂不上报，但显式列出以免落入 default 后误用 buffIdx
         *----------------------------------------------------------------------------------------*/
        case FLEXCAN_EVENT_ERROR:
        case FLEXCAN_EVENT_ERROR_FAST:
        {
            /* buffIdx 是 ESR1 值，不使用 */
            break;
        }

        default:
        {
            /* 未启用的硬件特性事件（RxFIFO / DMA 系列），忽略 */
            break;
        }
    }
}

/*==================================================================================================
 *                         LOCAL FUNCTIONS —— 使能错误/总线关闭中断
 *
 * 【为什么这一步必须做，而且必须做在 MCAL 层】
 *
 * FlexCAN 有两组互不相干的中断源，走两条不同的中断向量：
 *
 *     FlexCANx_1_IRQn  →  CANx_ORED_0_31_MB_IRQHandler  →  邮箱收发完成
 *     FlexCANx_0_IRQn  →  CANx_ORED_IRQHandler          →  Bus-off / Error
 *                          （S32K344_COMMON.h: FlexCAN0_0_IRQn = 109,
 *                            "Interrupt indicating that the CAN bus went to Bus Off state"）
 *
 * 而硬件层面，Bus-off / Error 这些事件要真正产生中断，还必须在 CTRL1 里
 * 置位对应的**屏蔽位**（BOFFMSK / ERRMSK）。RTD 对这几个位**只读不写**：
 *
 *     FlexCAN_Ip.c:1941  if ((ESR1 & ERRINT)  && (CTRL1 & ERRMSK))   ← 只在读取
 *     FlexCAN_Ip.c:2027  if ((ESR1 & BOFFINT) && (CTRL1 & BOFFMSK))  ← 只在读取
 *
 * 唯一的写入口是 FlexCAN_Ip_SetErrorInt()（→ FlexCAN_SetErrIntCmd）。
 * 也就是说：**不去调它，屏蔽位永远是 0，Bus-off 事件永远不会派发**，
 * 哪怕 CT 里已经注册了 ErrorCallback。
 *
 * 【为什么必须在 Freeze / Stop 模式下调用】
 * RTD 的官方注释（FlexCAN_Ip.h:790）：
 *     "@Note This function should be called from StopMode or FreezeMode."
 * 而且 RX/TX_WARNING 两项在开启了 DEV_ERROR_DETECT 时会直接检查
 * FlexCAN_IsFreezeMode()，不满足就返回 FLEXCAN_STATUS_ERROR。
 *
 * 调用时机：Can_Init() 里紧跟 FlexCAN_Ip_Init() 之后 ——
 * 此时模块刚初始化完、仍处于 Freeze 模式，正是唯一正确的窗口。
 * 一旦调用过 FlexCAN_Ip_SetStartMode()，就进不了这个窗口了
 * （再进 Freeze 会打断正在进行的收发）。
 *
 * 【为什么用 FlexCAN_Ip_SetErrorInt 而不是底层 FlexCAN_SetErrIntCmd】
 * 后者需要 FLEXCAN_Type* 基址指针，而基址数组 Flexcan_Ip_apxBase[]
 * 是 FlexCAN_Ip.c 里的 static 变量，外部拿不到。
 * FlexCAN_Ip_SetErrorInt 是公开宏，内部完成基址查找与用户模式派发。
 *================================================================================================*/
static void Can_EnableErrorInterrupts(uint8 Controller)
{
    /* Bus-off 中断 —— 控制器进入 Bus-off 状态时上报。
     * 这是最容易观测的一项：拔掉 CAN 线或短接 CANH/CANL 就能触发。 */
    (void)FlexCAN_Ip_SetErrorInt(Controller, FLEXCAN_IP_INT_BUSOFF, TRUE);

    /* 通用错误中断 —— 位错误、CRC 错误、格式错误等协议层错误上报。 */
    (void)FlexCAN_Ip_SetErrorInt(Controller, FLEXCAN_IP_INT_ERR, TRUE);

    /* 【RX/TX_WARNING 暂不使能 —— 这是有意的取舍】
     * 这两项对应错误计数器 >= 96 时的告警（逼近 Error Passive 的 128 阈值）。
     * 使能后会在 Can_IsrBridge 里被映射成 CAN_ERRORSTATE_PASSIVE。
     *
     * 不使能的原因：
     *   1. 它们在 DEV_ERROR_DETECT 打开时对 Freeze 模式的要求更严格，
     *      依赖冻结窗口不够稳健
     *   2. 调试阶段总线异常（未接终端电阻）时会有大量告警，淹没真正的问题
     *
     * 需要更细的错误追踪时，把下面两行注释打开即可：
     *   (void)FlexCAN_Ip_SetErrorInt(Controller, FLEXCAN_IP_INT_RX_WARNING, TRUE);
     *   (void)FlexCAN_Ip_SetErrorInt(Controller, FLEXCAN_IP_INT_TX_WARNING, TRUE);
     */
}

/*==================================================================================================
 *                              LOCAL FUNCTIONS —— 接收邮箱初始化
 *================================================================================================*/
static void Can_ConfigRxMailboxes(const Can_ConfigType* Config)
{
    uint8 i;

    for (i = 0U; i < Config->RxObjectCount; i++)
    {
        const Can_RxHwObjectConfigType* rx = &Config->RxObjects[i];

        /* 【必须先整体清零】
         * Flexcan_Ip_DataInfoType 里有一部分字段由条件编译控制 ——
         * 本工程的 FlexCAN_Ip_Cfg.h 里 FLEXCAN_IP_FEATURE_HAS_FD = STD_ON，
         * 所以该结构体**含有** fd_enable / fd_padding / enable_brs 三个 FD 字段。
         * 不整体清零就只赋值部分成员，剩下的会是栈上的垃圾值 ——
         * 表现为"配置莫名其妙生效/不生效"，且编译器不报警。
         * 用 = {0} 一次性清零，比逐个字段赋值更抗结构体变化。 */
        Flexcan_Ip_DataInfoType rxInfo = {0};

        /* 防御：控制器号或邮箱号越界则跳过。
         * 这类配置错误应当在集成阶段就暴露，而不是留到运行时踩内存。 */
        if ((rx->ControllerId >= CAN_CONTROLLER_COUNT) ||
            (rx->MbIdx >= CAN_MB_BUF_COUNT))
        {
            continue;
        }

        /* 防御：控制器没初始化成功就跳过它的邮箱。
         *
         * 【为什么需要这一条】
         * Can_Init() 里若某个控制器 FlexCAN_Ip_Init() 失败，它的状态会保持
         * CAN_CS_UNINIT。此时若仍对它的邮箱调 FlexCAN_Ip_ConfigRxMb() /
         * SetRxIndividualMask()，在 FLEXCAN_IP_DEV_ERROR_DETECT = STD_ON 的
         * 配置下会触发 RTD 内部的 DevAssert（访问未初始化的实例状态）。 */
        if (Can_ControllerMode[rx->ControllerId] == CAN_CS_UNINIT)
        {
            continue;
        }

        rxInfo.msg_id_type = (rx->IsExtended != FALSE) ? FLEXCAN_MSG_ID_EXT
                                                       : FLEXCAN_MSG_ID_STD;
        rxInfo.data_length = 8U;
        rxInfo.fd_enable   = FALSE;
        rxInfo.is_polling  = FALSE;

        /*------------------------------------------------------------------
         * 1. 设置该邮箱的**接收掩码**（先掩码，再配邮箱）
         *
         * 【这一步不能省 —— 是"这个邮箱收哪个 ID"能生效的前提】
         * FlexCAN 的接收匹配规则是：
         *     (接收到的字段 XOR 邮箱配置的字段) AND RXIMR == 0  →  匹配
         * 而 RXIMR 的复位值是 0，于是上式恒等于 0 —— **任何帧都匹配**。
         *
         * 也就是说：只调 ConfigRxMb(…, 0x100) 而不设掩码，MB 会把总线上的
         * 所有帧接下来，并把它们全都当作"Hrh 0 的 0x100"上报给上层。
         * 上层完全无法察觉 ID 已经错了。
         *
         * 掩码 = 0 时（CAN_RX_MASK_ACCEPT_ALL）等价于"收所有 ID"，
         * 调试时想临时收全部帧，把配置表里的掩码改成 0 即可。
         *----------------------------------------------------------------*/
        {
            uint32 idMask = rx->CanIdMask;
            uint32 hwMask;

            if (rx->IsExtended != FALSE)
            {
                /* 扩展帧：29 位 ID 在 bit28..0，不移位 */
                idMask &= CAN_RXIMR_EXT_IDMASK_MAX;
                hwMask  = CAN_RXIMR_IDE_RTR_MASK | idMask;
            }
            else
            {
                /* 标准帧：11 位 ID 在 bit28..18，需要左移 18 位 */
                idMask &= CAN_RXIMR_STD_IDMASK_MAX;
                hwMask  = CAN_RXIMR_IDE_RTR_MASK | (idMask << CAN_RXIMR_STD_SHIFT);
            }

            (void)FlexCAN_Ip_SetRxIndividualMask(rx->ControllerId, rx->MbIdx, hwMask);
        }

        /* 2. 把该邮箱配置成"接收指定 ID" */
        (void)FlexCAN_Ip_ConfigRxMb(rx->ControllerId, rx->MbIdx, &rxInfo, rx->CanId);

        /* 3. 把缓冲挂上去并打开该邮箱的接收中断 */
        (void)FlexCAN_Ip_Receive(rx->ControllerId, rx->MbIdx,
                                 &Can_RxMsgBuffer[rx->ControllerId][rx->MbIdx], FALSE);
    }
}

/*==================================================================================================
 *                                   GLOBAL FUNCTIONS
 *================================================================================================*/

void Can_Init(const Can_ConfigType* Config)
{
    uint8 i;

    if (Config == NULL_PTR)
    {
        return;
    }

    Can_DriverConfig      = Config;
    Can_DriverInitialized = FALSE;

    /* 初始化内部状态数组 */
    for (i = 0U; i < CAN_CONTROLLER_COUNT; i++)
    {
        Can_ControllerMode[i]  = CAN_CS_UNINIT;
        Can_ControllerError[i] = CAN_ERRORSTATE_ACTIVE;
    }

    /*--------------------------------------------------------------------------
     * 1. 逐个控制器初始化 RTD 驱动
     *------------------------------------------------------------------------*/
    for (i = 0U; i < Config->ControllerCount; i++)
    {
        const Can_ControllerConfigType* ctrl = &Config->Controllers[i];

        if (ctrl->ControllerId >= CAN_CONTROLLER_COUNT)
        {
            continue;
        }

        /* 【这里做 void* → RTD 类型的强转】
         * can_cfg.h 用 void* 保存 RTD 配置指针，是为了让配置头文件不依赖 RTD。
         * 本文件是唯一知道真实类型的地方，所以在这里转回来。
         * 这是"用一点类型安全换取接口隔离"的取舍 —— 代价是这里必须转对。 */
        const Flexcan_Ip_ConfigType* rtdCfg =
            (const Flexcan_Ip_ConfigType*)ctrl->RtdConfig;

        /*------------------------------------------------------------------
         * 直接使用生成配置里的回调，本层不再覆盖
         *
         * 【为什么现在不需要覆盖了】
         * 先前 CT 生成的两个配置里 .ErrorCallback 都是 NULL_PTR，
         * 而 RTD 在 FlexCAN_Ip.c:2059 是用 pState->error_callback(...) 派发
         * Bus-off / Error / Warning 的 —— 空指针时 RTD 内部判空跳过，
         * 这些事件会被静默丢弃。所以当时在 Can_Init 里做了一次运行时覆盖。
         *
         * 现在 CT 的配置界面里已经填了两个 ErrorCallback（见 .mex 的
         * FlexCanErrorCallback 字段），生成代码里对应为：
         *     generate/src/FlexCAN_Ip_Sa_PBcfg.c:100/105  extern 声明
         *     generate/src/FlexCAN_Ip_Sa_PBcfg.c:218/309  &FlexCAN0/1_ErrorCallback
         * 而那四个回调函数正是本文件实现的（第 121~166 行）。
         * 因此运行时覆盖已属冗余，删掉它让"配置来源"只有一处：
         *     **回调由 CT 配置决定，实现在本层** —— 这是更干净的分工。
         *
         * 【约束（改 CT 配置时必须遵守）】
         * CT 里的 FlexCanCallback / FlexCanErrorCallback 必须保持填成本文件
         * 实现的这四个函数名，否则：
         *   - 忘了填 ErrorCallback → Bus-off 事件静默丢弃（无编译错误，最难查）
         *   - 名字和本文件不一致      → 链接报 undefined reference（好查）
         *  链接错误能立刻发现，而"忘了填"不会报错 —— 所以上面这条要记住。
         *----------------------------------------------------------------*/
        Flexcan_Ip_StatusType initStatus = FlexCAN_Ip_Init((uint8)ctrl->ControllerId,
                                                          (Flexcan_Ip_StateType*)ctrl->RtdState,
                                                          rtdCfg);

        /* 【必须检查返回值】
         * 早期版本忽略了它：即使 Init 失败（例如配置引用的实例不存在、
         * 模块被禁用），后面照样把控制器标成 STOPPED、照样使能中断、
         * 照样挂载接收邮箱 —— 结果是"上层以为驱动初始化好了"，
         * 实际硬件根本没起来，所有 API 都静默失效。
         * 初始化失败就跳过该控制器，让它的状态保持 CAN_CS_UNINIT，
         * 后续 Can_SetControllerMode / Can_Write 会正常报错而不是假装成功。 */
        if (initStatus == FLEXCAN_STATUS_SUCCESS)
        {
            /*----------------------------------------------------------------
             * 接收掩码类型必须设为"个体掩码"
             *
             * MCR.IRMQ 决定接收过滤用哪套掩码：
             *     IRMQ = 0  →  全局掩码 RXMGMASK（所有邮箱共用）
             *     IRMQ = 1  →  个体掩码 RXIMR[n]（每个邮箱独立）
             *
             * 我们的配置是"每个邮箱收不同 ID"，所以必须用个体掩码。
             * IRMQ 复位值是 0（全局），RTD 也不会自动改 —— 必须显式设。
             * 同样受 Freeze 模式约束（FlexCAN_Ip.h:598），此处正好在窗口内。
             *----------------------------------------------------------------*/
            (void)FlexCAN_Ip_SetRxMaskType(ctrl->ControllerId, FLEXCAN_RX_MASK_INDIVIDUAL);

            /*----------------------------------------------------------------
             * 使能 Bus-off / Error 中断的**硬件屏蔽位**
             *
             * 【为什么必须紧跟在 Init 之后】
             * RTD 明确要求 FlexCAN_Ip_SetErrorInt() 在 Stop/Freeze 模式下调用
             * （FlexCAN_Ip.h:790）。FlexCAN_Ip_Init() 返回时模块正处于 Freeze 模式
             * —— 已核对 RTD 源码：Init 路径里没有 ExitFreezeMode 调用，
             * ExitFreezeMode 只出现在 SetStartMode 里（FlexCAN_Ip.c:3595）。
             * 这是唯一安全的窗口 —— 一旦调用过 SetStartMode()，
             * 再进 Freeze 会打断正在进行的收发。
             *
             * 【为什么不能省】
             * CT 里注册 ErrorCallback 只解决了"有函数可调"，
             * 而 CTRL1.BOFFMSK / ERRMSK 这两个屏蔽位 RTD **只读不写**，
             * 不显式置位的话中断根本不会产生 —— 回调永远等不到事件。
             * 详见 Can_EnableErrorInterrupts() 的注释。
             *----------------------------------------------------------------*/
            Can_EnableErrorInterrupts(ctrl->ControllerId);

            Can_ControllerMode[ctrl->ControllerId] = CAN_CS_STOPPED;
        }
        /* Init 失败：保持 CAN_CS_UNINIT，不使能中断、不挂邮箱 */
    }

    /*--------------------------------------------------------------------------
     * 2. 配置并挂载所有接收邮箱
     *    此时控制器仍处于 Freeze 状态（未 Start），可以安全配置邮箱。
     *------------------------------------------------------------------------*/
    Can_ConfigRxMailboxes(Config);

    Can_DriverInitialized = TRUE;
}

void Can_DeInit(void)
{
    uint8 i;

    for (i = 0U; i < CAN_CONTROLLER_COUNT; i++)
    {
        if (Can_ControllerMode[i] != CAN_CS_UNINIT)
        {
            (void)FlexCAN_Ip_Deinit(i);
            Can_ControllerMode[i]  = CAN_CS_UNINIT;
            Can_ControllerError[i] = CAN_ERRORSTATE_ACTIVE;
        }
    }

    Can_DriverConfig      = NULL_PTR;
    Can_DriverInitialized = FALSE;
}

Std_ReturnType Can_SetControllerMode(uint8 Controller, Can_ControllerStateType Transition)
{
    if (Controller >= CAN_CONTROLLER_COUNT)
    {
        CAN_DET_ERROR(CAN_API_SET_CONTROLLER_MODE, CAN_E_PARAM_CONTROLLER);
        return E_NOT_OK;
    }

    /* 驱动未初始化时不允许切换模式。
     * 否则会调进 RTD 的 SetStartMode/SetStopMode，在 DEV_ERROR_DETECT 打开时
     * 触发 DevAssert；在没打开时则读写未初始化实例的寄存器 —— 两者都不该发生。 */
    if (Can_DriverInitialized == FALSE)
    {
        CAN_DET_ERROR(CAN_API_SET_CONTROLLER_MODE, CAN_E_UNINIT);
        return E_NOT_OK;
    }

    /* 该控制器本身初始化失败（状态仍为 UNINIT）时同样拒绝 */
    if (Can_ControllerMode[Controller] == CAN_CS_UNINIT)
    {
        CAN_DET_ERROR(CAN_API_SET_CONTROLLER_MODE, CAN_E_PARAM_CONTROLLER);
        return E_NOT_OK;
    }

    switch (Transition)
    {
        case CAN_CS_STARTED:
        {
            if (FlexCAN_Ip_SetStartMode(Controller) == FLEXCAN_STATUS_SUCCESS)
            {
                Can_ControllerMode[Controller] = CAN_CS_STARTED;
                return E_OK;
            }
            break;
        }

        case CAN_CS_STOPPED:
        {
            if (FlexCAN_Ip_SetStopMode(Controller) == FLEXCAN_STATUS_SUCCESS)
            {
                Can_ControllerMode[Controller] = CAN_CS_STOPPED;
                return E_OK;
            }
            break;
        }

        case CAN_CS_SLEEP:
        {
            /* 【故意返回"不支持"】
             * 不加这个注释直接返回 E_NOT_OK，别人会以为是漏实现。
             * 事实是：真正的 Sleep 需要停控制器 → 关时钟 → 使能唤醒源，
             * 本阶段不做。而**返回假的 E_OK 更糟** —— 上层会以为控制器睡了，
             * 总线上其实还在活动，这类"接口撒谎"比明确的失败难查得多。
             *
             * 【报 Det】
             * 调用者请求了一个本控制器不支持的模式 —— 这是调用方的认知错误，
             * 应该让他在开发期就知道，而不是等到现场发现"怎么没睡下去"。 */
            CAN_DET_ERROR(CAN_API_SET_CONTROLLER_MODE, CAN_E_PARAM_CONTROLLER_MODE);
            return E_NOT_OK;
        }

        default:
        {
            /* 传了一个既不是 STARTED / STOPPED / SLEEP 的枚举值 —— 纯调用错误 */
            CAN_DET_ERROR(CAN_API_SET_CONTROLLER_MODE, CAN_E_PARAM_CONTROLLER_MODE);
            break;
        }
    }

    return E_NOT_OK;
}

Std_ReturnType Can_GetControllerMode(uint8 Controller, Can_ControllerStateType* ModePtr)
{
    /* 两个检查分开写 —— 这样 Det 能报出**到底哪个参数错了**，
     * 合成一个 if 只能报"参数错"，调试时还得猜。 */
    if (Controller >= CAN_CONTROLLER_COUNT)
    {
        CAN_DET_ERROR(CAN_API_GET_CONTROLLER_MODE, CAN_E_PARAM_CONTROLLER);
        return E_NOT_OK;
    }
    if (ModePtr == NULL_PTR)
    {
        CAN_DET_ERROR(CAN_API_GET_CONTROLLER_MODE, CAN_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    *ModePtr = Can_ControllerMode[Controller];
    return E_OK;
}

Std_ReturnType Can_GetControllerErrorState(uint8 Controller, Can_ErrorStateType* ErrorStatePtr)
{
    if (Controller >= CAN_CONTROLLER_COUNT)
    {
        CAN_DET_ERROR(CAN_API_GET_CONTROLLER_ERROR_STATE, CAN_E_PARAM_CONTROLLER);
        return E_NOT_OK;
    }
    if (ErrorStatePtr == NULL_PTR)
    {
        CAN_DET_ERROR(CAN_API_GET_CONTROLLER_ERROR_STATE, CAN_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    *ErrorStatePtr = Can_ControllerError[Controller];
    return E_OK;
}

Std_ReturnType Can_Write(Can_HwHandleType Hth, const Can_PduType* PduInfo)
{
    const Can_TxHwObjectConfigType* hwObj;
    Flexcan_Ip_DataInfoType txInfo;
    Flexcan_Ip_StatusType   status;
    uint32                  rawCanId;
    boolean                 reqFd;   /* 本帧是否要求 FD —— 在 4b 步判定，第 5 步用 */

    /*--------------------------------------------------------------------------
     * 1. 前置检查
     *------------------------------------------------------------------------*/
    if (Can_DriverInitialized == FALSE)
    {
        CAN_DET_ERROR(CAN_API_WRITE, CAN_E_UNINIT);
        return E_NOT_OK;
    }
    if (PduInfo == NULL_PTR)
    {
        CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_POINTER);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 2. 查表：逻辑句柄 → 物理硬件对象
     *    句柄不存在属**调用方用法错误**，不是"拥塞"，返回 E_NOT_OK。
     *------------------------------------------------------------------------*/
    hwObj = Can_FindTxObjectByHth(Hth);
    if (hwObj == NULL_PTR)
    {
        CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_HANDLE);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 3. 控制器必须已启动
     *
     * 【这里刻意区分两种"没启动"】
     *   CAN_CS_UNINIT  → 控制器初始化就失败了，此时还往上发 = 调用方没检查初始化结果
     *                    → 报 Det
     *   CAN_CS_STOPPED → 正常停机流程中的状态（例如关机前）→ **不报 Det**
     *                    → 只返回 E_NOT_OK，让调用方自己决定
     *
     * 这就是"合法状态"和"程序错误"的区别，和 CanIf 里 TX_OFFLINE 的处理同理。
     *------------------------------------------------------------------------*/
    if (Can_ControllerMode[hwObj->ControllerId] != CAN_CS_STARTED)
    {
        if (Can_ControllerMode[hwObj->ControllerId] == CAN_CS_UNINIT)
        {
            CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_CONTROLLER);
        }
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 4. PDU 合法性校验
     *------------------------------------------------------------------------*/
    if (PduInfo->sdu == NULL_PTR)
    {
        CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_POINTER);
        return E_NOT_OK;
    }
    if (PduInfo->length > hwObj->PayloadSize)
    {
        CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_DATA_LENGTH);
        return E_NOT_OK;
    }

    /*--------------------------------------------------------------------------
     * 4b. FD 一致性校验（现在 FD 关闭，本段只会走"经典帧"分支；
     *     代码提前就位，将来开 FD 时不用回头改收发的主动路径）
     *
     * 【AUTOSAR 的 FD 判定规则】
     * 帧格式由 Can_PduType.id 的 bit30 携带（不是控制器的全局模式）：
     *     bit31 = 1  扩展帧
     *     bit30 = 1  CAN FD 帧
     *     bit29..0   实际 ID
     *
     * 官方 Can_43_FLEXCAN 就是这么做的（Can_43_FLEXCAN_Ipw.c:2100）：
     *     DataInfo.fd_enable = ((PduInfo->id & FD_FRAME_U32) != 0U) ? TRUE : FALSE;
     * 其校验逻辑见 Can_43_FLEXCAN.c:766-779（Can_ValidatePdu）。
     *
     * 【两种必须拒绝的情况】
     *   a) 控制器不具备 FD 能力，却要求发 FD 帧
     *      → 真发出去硬件会出错（EDL 位置位但模块 FDEN 未开）
     *   b) 长度 > 8 字节，却没有标 FD 位
     *      → 经典帧装不下 9 字节，硬件会截断或报错
     * -------------------------------------------------------------------------*/
    {
        /* 请求的是不是 FD 帧 —— 只看 bit30，与控制器全局模式无关 */
        reqFd = ((PduInfo->id & CAN_ID_FD_MASK) != 0U) ? TRUE : FALSE;

        if (reqFd != FALSE)
        {
            const Flexcan_Ip_ConfigType* rtdCfg = Can_GetRtdControllerConfig(hwObj->ControllerId);

            /* 控制器不是 FD 模式（CT 里 fd_enable = FALSE）却要求发 FD 帧 */
            if ((rtdCfg == NULL_PTR) || (rtdCfg->fd_enable == FALSE))
            {
                /* 这是**配置与调用不匹配**：CT 里没开 FD，上层却标了 FD 位。
                 * 属于配置文件之间没对齐，开发期就该抓出来。 */
                CAN_DET_ERROR(CAN_API_WRITE, CAN_E_INVALID_CONFIG);
                return E_NOT_OK;
            }
        }
        else
        {
            /* 没标 FD 位，则长度受经典 CAN 的 8 字节硬上限约束。
             * 注意这里用独立的 8 而不是 hwObj->PayloadSize ——
             * 因为 PayloadSize 是"该对象允许的上限"，开了 FD 后可能配成 64，
             * 但"经典帧仍然是 8"这个协议事实不会变。 */
            if (PduInfo->length > 8U)
            {
                /* 长度 > 8 却没标 FD 位 → 调用方漏了 CAN_ID_FD_MASK */
                CAN_DET_ERROR(CAN_API_WRITE, CAN_E_PARAM_DATA_LENGTH);
                return E_NOT_OK;
            }
        }
    }

    /*--------------------------------------------------------------------------
     * 5. 翻译：AUTOSAR 语义 → RTD 语义
     *
     * 与 Can_ConfigRxMailboxes() 同理：本工程 FLEXCAN_IP_FEATURE_HAS_FD = STD_ON，
     * Flexcan_Ip_DataInfoType 含 fd_enable / fd_padding / enable_brs 三个 FD 字段。
     * 必须整体清零，否则未赋值的字段是栈垃圾。
     *
     * 【顺序坑：清零必须在第 4b 步之后】
     * fd_enable 是在 4b 步算出来的 reqFd，这里清零后要再赋回去。
     * 早期版本把 fd_enable 写在清零之前，结果被 {0} 覆盖 —— 编译不报错，
     * 表现为"FD 标志莫名丢失"。
     *------------------------------------------------------------------------*/
    txInfo = (Flexcan_Ip_DataInfoType){0};
    txInfo.msg_id_type = (hwObj->IsExtended != FALSE) ? FLEXCAN_MSG_ID_EXT
                                                      : FLEXCAN_MSG_ID_STD;
    txInfo.data_length = PduInfo->length;
    txInfo.is_remote   = FALSE;
    txInfo.is_polling  = FALSE;
    txInfo.fd_enable   = reqFd;      /* ← 来自 4b 步的判定，不是硬写 FALSE */

    /* 剥掉 Can_IdType 的标志位，只留 bit29..0 的裸 ID。
     * 帧类型（标准/扩展）由配置表的 IsExtended 决定，不从 ID 推导 ——
     * 这是"配置驱动"：上层给 ID，本层按句柄查表决定怎么发。 */
    rawCanId = PduInfo->id & CAN_ID_ID_MASK;

    /*--------------------------------------------------------------------------
     * 6. 交给 RTD 发送
     *------------------------------------------------------------------------*/
    status = FlexCAN_Ip_Send(hwObj->ControllerId, hwObj->MbIdx,
                             &txInfo, rawCanId, PduInfo->sdu);

    if (status == FLEXCAN_STATUS_SUCCESS)
    {
        return E_OK;
    }

    /*--------------------------------------------------------------------------
     * 7. 区分"暂时没空"与"真失败"
     *
     * FLEXCAN_STATUS_BUSY 表示该邮箱正被上一次发送占用 → 映射为规范的
     * 叠加返回值 CAN_BUSY，让上层可以稍后重试，而不是当成故障上报。
     *------------------------------------------------------------------------*/
    if (status == FLEXCAN_STATUS_BUSY)
    {
        return CAN_BUSY_RET;
    }

    return E_NOT_OK;
}

/*==================================================================================================
 *                               周期处理函数（当前为空）
 *
 * 三个函数都留空，理由：
 *   1. 本工程所有硬件对象都用**中断模式**（is_polling = FALSE），
 *      没有需要轮询处理的收发
 *   2. Bus-off 退出检测尚未实现 —— FlexCAN 的自动恢复不产生中断，
 *      要精确知道"什么时候退出了 Bus-off"，只能在这里轮询
 *      FlexCAN_Ip_GetErrorStatus() 的 FLTCONF 位。列为下一步 TODO。
 * 保留函数体为空而不是删除，是因为上层会按固定周期调用它们，
 * 接口先占住位置。
 *================================================================================================*/
void Can_MainFunction_Read(void)   { }
void Can_MainFunction_Write(void)  { }
void Can_MainFunction_BusOff(void) { }

/*==================================================================================================
 *                                   HOOK 注册
 *================================================================================================*/
void Can_RegisterRxHook(Can_RxIndicationHookType hook)
{
    Can_RxHook = hook;
}

void Can_RegisterTxHook(Can_TxConfirmationHookType hook)
{
    Can_TxHook = hook;
}

void Can_RegisterBusOffHook(Can_ControllerBusOffHookType hook)
{
    Can_BusOffHook = hook;
}

void Can_RegisterErrorStateHook(Can_ControllerErrorStateHookType hook)
{
    Can_ErrorStateHook = hook;
}
