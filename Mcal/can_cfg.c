/*==================================================================================================
 *  Mcal/can_cfg.c —— CAN MCAL 配置数据（Bootloader 版）
 *
 *  【本文件是"配置"，can.c 是"逻辑"】
 *  改行为只改这里。can.c 一行都不用动 ——
 *  本工程里的 can.c 是从 App 工程原样拷过来的，**一行没改**，
 *  只是把这里的表从"2 路 CAN / 4 发 4 收"换成了"1 路 CAN / 1 发 1 收"。
 *  这就是"配置驱动"最直接的证据。
 *
 *  【本文件是唯一知道 RTD 配置对象名的地方之一】
 *  FlexCAN_Config0 / FlexCAN_State0 这些符号由 CT 生成的
 *  generate/src/FlexCAN_Ip_Sa_PBcfg.c 提供。
 *  它们是 RTD 层的配置实例，我们在 MCAL 层把它们绑定到逻辑控制器编号上。
 *================================================================================================*/

#include "can_cfg.h"

/*==================================================================================================
 *                              CT 生成的 RTD 配置对象（外部声明）
 *
 * 来源：generate/src/FlexCAN_Ip_Sa_PBcfg.c
 * 注意这里声明的类型是 Flexcan_Ip_ConfigType，但 can_cfg.h 里存的是 void*。
 * 为了让本文件编译，需要 RTD 的类型定义 ——
 * 这是**配置层唯一允许接触 RTD 类型的地方**（因为它的职责就是描述用哪个 RTD 实例）。
 *
 * ⚠️ 这里声明的每一个 FlexCAN_ConfigN / FlexCAN_StateN 都必须在
 *    generate/src/FlexCAN_Ip_Sa_PBcfg.c 里真实存在。
 *    CT 里配了几个 FlexCAN 实例，那里就有几组；
 *    多声明一个会**链接失败**（undefined reference）—— 这就是原本 App 工程
 *    拷过来后链接不过的原因（它声明了 Config1/State1，但 CT 只配了 1 路）。
 *================================================================================================*/
#include "FlexCAN_Ip.h"

extern const Flexcan_Ip_ConfigType FlexCAN_Config0;   /**< CAN0 的 RTD 配置（CT 生成） */
extern Flexcan_Ip_StateType        FlexCAN_State0;    /**< CAN0 的 RTD 运行状态（CT 生成） */

/*==================================================================================================
 *  ▼▼▼ 本工程的 CAN 配置：1 路 CAN0 + 1 发 1 收
 *================================================================================================*/

/*==================================================================================================
 *                                     控制器表
 *
 *   逻辑编号 0  →  RTD 的 FlexCAN_Config0 / State0  →  物理 CAN0
 *
 *  引脚（由 CT 的 Pins 工具配置）：
 *      PTA6  = CAN0_RX  (pin 102)
 *      PTA7  = CAN0_TX  (pin 100)
 *
 *  位率（由 CT 的 FlexCAN 组件配置）：
 *      500 kbps，20 TQ，80% 采样点
 *      preDivider = 8，seg = 10/5/4（80 MHz / 8 / 20 = 500 k）
 *
 *  上层只认逻辑编号。将来若加第二路 CAN，在这张表加一行即可。
 *================================================================================================*/
static const Can_ControllerConfigType Can_Controllers[] =
{
    /* ControllerId, RtdConfig,        RtdState        */
    {  0U,           &FlexCAN_Config0, &FlexCAN_State0  },
};

/*==================================================================================================
 *                                  发送硬件对象表（HTH）
 *
 * 句柄取值必须 >= CAN_HTH_FIRST_INDEX（= 4），见 can_cfg.h 的编号约定。
 *
 *   Hth 4  →  CAN0 邮箱 1   标准帧  最多 8 字节
 *
 *  【这个邮箱用来发什么】
 *  发 UDS 响应：0x7E8（诊断仪的物理响应地址）。
 *
 *  【CAN ID 不在本表里】
 *  AUTOSAR 的 Can_Write(Hth, Can_PduType) 里，CAN ID 是**每次调用时**
 *  通过 PduInfo->id 传进去的。所以这张表只管"用哪个邮箱、能发多长"。
 *
 *  调用示例（Dcm 里发响应时）：
 *      Can_PduType pdu;
 *      pdu.id     = 0x7E8U;          ← CAN ID 在这里
 *      pdu.length = 8U;
 *      pdu.sdu    = responseBuf;
 *      (void)Can_Write(CanConf_TxObject_Hth4, &pdu);
 *
 * 【为什么用 CAN_HTH_FIRST_INDEX + n 而不是直接写 4】
 * 把"分段起点"这个约定集中在一个宏上。将来若增加接收对象、起点右移，
 * 只需改 can_cfg.h 的一个数字，这张表自动跟着变。
 * 直接写数字会在改约定时静默错位 —— 这类缺陷编译不报错、只在运行时表现为
 * "句柄莫名其妙"。
 *================================================================================================*/
static const Can_TxHwObjectConfigType Can_TxObjects[] =
{
    /* Hth（用符号名，见 can_cfg.h）,  ControllerId, MbIdx, IsExtended, PayloadSize */
    {  CanConf_TxObject_Hth4,         0U,          1U,    FALSE,      8U },
};

/*==================================================================================================
 *                                  接收硬件对象表（HRH）
 *
 * 句柄取值必须 < CAN_HTH_FIRST_INDEX（= 4），见 can_cfg.h 的编号约定。
 *
 *   Hrh 0  ←  CAN0 邮箱 5   标准帧  ID 0x7E0  精确匹配
 *
 *  【这个邮箱用来收什么】
 *  UDS 请求：0x7E0（诊断仪的物理请求地址）。
 *  刷写命令 0x34 RequestDownload / 0x36 TransferData / 0x37 全走这个地址。
 *
 *  【CanIdMask 怎么填】
 *   CAN_RX_MASK_EXACT_STD  标准帧精确匹配（11 位全比对）   ← 本工程用这个
 *   CAN_RX_MASK_EXACT_EXT  扩展帧精确匹配（29 位全比对）
 *   CAN_RX_MASK_ACCEPT_ALL 收所有 ID（调试时很方便：分析仪发什么都进这个邮箱）
 *   自定义范围掩码         想收 0x7E0~0x7E7，就写 0x7F8（低 3 位不比对）
 *
 *   ⚠️ 掩码不是可选项。S32K344 的掩码寄存器复位值 = 0 = "接受所有 ID"，
 *   不填这个字段就等于"这个邮箱收总线上的一切"，配置里的 CanId 形同虚设。
 *   Bootloader 尤其不能犯这个错 —— 收到不该收的帧会让 UDS 状态机错乱。
 *
 *  【MbIdx 的取值范围约束】
 *   CAN0 的 RTD 配置 max_num_mb = 96  → MbIdx 必须 < 96
 *   同时 MbIdx 必须 < can.c 里 CAN_MB_BUF_COUNT（静态接收缓冲数量），
 *   否则 can.c 的边界检查会拒收并丢弃该帧。
 *   这两个约束是**硬约束**，改这里时必须一起检查。
 *================================================================================================*/
static const Can_RxHwObjectConfigType Can_RxObjects[] =
{
    /* Hrh（用符号名）,  		 ControllerId, MbIdx, CanId,   CanIdMask,              IsExtended */
    {  CanConf_RxObject_Hrh0,  0U,         5U,    0x7E0U,  CAN_RX_MASK_EXACT_STD,   FALSE },
};

/*==================================================================================================
 *                                  顶层配置对象
 *
 * Can_Init(&Can_Config) 的唯一入参。
 *
 * 【表项数为什么不手写数字】
 * 用 sizeof 计算，避免"表里加了项但数量忘了改"这类静默不一致 ——
 * 那是"两套并行事实"问题的典型形态：表是事实，数字是另一份事实，
 * 两者脱节时不报错。
 *================================================================================================*/
const Can_ConfigType Can_Config =
{
    .Controllers     = Can_Controllers,
    .ControllerCount = (uint8)(sizeof(Can_Controllers) / sizeof(Can_Controllers[0])),
    .TxObjects       = Can_TxObjects,
    .TxObjectCount   = (uint8)(sizeof(Can_TxObjects) / sizeof(Can_TxObjects[0])),
    .RxObjects       = Can_RxObjects,
    .RxObjectCount   = (uint8)(sizeof(Can_RxObjects) / sizeof(Can_RxObjects[0])),
};

/*==================================================================================================
 *                     编译期一致性断言（Compile-Time Assertion）
 *
 * 【为什么需要】
 *  can_cfg.h 里的 CAN_CONTROLLER_COUNT / CAN_TX_OBJECT_COUNT / CAN_RX_OBJECT_COUNT
 *  是**手写的数字**，can_cfg.c 里的表是**另一份事实**。两者脱节时：
 *      - can.c 的边界检查会按错误的数量截断 → 静默丢对象
 *      - 上层的句柄引用可能指向不存在的行
 *
 * 【怎么实现】
 *  C99 没有 static_assert。惯用技巧是"用条件表达式当数组长度"：
 *      条件成立 → 长度 1 → 合法
 *      条件不成立 → 长度 -1 → 编译器报 "size of array is negative"
 *
 *  这个断言在**编译期**生效，不占任何运行时空间和指令。
 *
 * 【什么时候会撑破】
 *  在表里加/删一行，但忘了改 can_cfg.h 里对应的数量宏。
 *================================================================================================*/
typedef char Can_TxObjectCountAssert[
    ((sizeof(Can_TxObjects) / sizeof(Can_TxObjects[0])) == (uint32)CAN_TX_OBJECT_COUNT) ? 1 : -1];

typedef char Can_RxObjectCountAssert[
    ((sizeof(Can_RxObjects) / sizeof(Can_RxObjects[0])) == (uint32)CAN_RX_OBJECT_COUNT) ? 1 : -1];

typedef char Can_ControllerCountAssert[
    ((sizeof(Can_Controllers) / sizeof(Can_Controllers[0])) == (uint32)CAN_CONTROLLER_COUNT) ? 1 : -1];
