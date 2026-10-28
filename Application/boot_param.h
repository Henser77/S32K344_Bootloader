/*==================================================================================================
 *  src/boot_param.h —— 升级标志（参数区）
 *
 *  【它解决什么问题】
 *  Bootloader 上电后必须回答一个问题：
 *      "App 区里那份固件是完整的吗？能跳过去吗？"
 *
 *  光看"App 区第一个字是不是 0xFF"是不够的 —— 刷到一半断电时，
 *  App 区前半是新的、后半是 0xFF 或旧的，看起来"有代码"，跳过去必然跑飞。
 *
 *  所以需要一块"小账本"，记录：
 *      这份 App 有效吗？（AppValid）
 *      它有多长？（AppSize）
 *      它的 CRC32 是多少？（AppCrc32）
 *  Bootloader 上电重新算一遍 App 区的 CRC，和账本比对，一致才跳。
 *
 *  【账本存在哪里】
 *  Flash 参数区 0x00600000，独占一个 8 KB 扇区（FLS_PARAM_START）。
 *  不和 Bootloader 代码区、App 区、测试扇区重叠。
 *
 *  【为什么要 StructCrc32】
 *  Flash 写入只能 1→0，如果写到一半掉电，账本里可能是"新旧混合"的内容。
 *  所以整个结构最后有一个自己的 CRC —— 结构自身被写坏了就能立刻发现，
 *  而不是拿着半真半假的字段去做决定。
 *
 *  【断电安全的关键顺序】★ 这是 Bootloader 最重要的一条纪律
 *
 *      刷写流程                    AppValid 取值        上电结果
 *      ─────────────────────────────────────────────────────────
 *      开始擦除 App 区之前          置 0                 → 留在 Bootloader
 *      擦除 / 写入 / CRC 校验       保持 0               → 留在 Bootloader
 *      全部成功、CRC 一致           置 1 + 写 CRC/Size   → 跳 App
 *
 *  顺序反了（先写"有效"再擦除）就会出现：刷到一半断电 → 账本说"有效" →
 *  跳进一个半新半旧的 App → 跑飞，而且诊断仪再也连不上（因为没停留在 Bootloader）。
 *================================================================================================*/
#ifndef BOOT_PARAM_H
#define BOOT_PARAM_H

#include "Std_Types.h"
#include "fls_cfg.h"

/*==================================================================================================
 *                                      配置
 *================================================================================================*/

/** @brief 账本结构的魔数："SK3B" —— 用来区分"初始化过"和"全是 0xFF 的空白 Flash" */
#define BOOT_PARAM_MAGIC            (0x534B3342UL)

/** @brief 账本存放地址 */
#define BOOT_PARAM_ADDR             (FLS_PARAM_START)

/** @brief 结构里 AppValid 的取值 */
#define BOOT_PARAM_APP_INVALID      (0U)
#define BOOT_PARAM_APP_VALID        (1U)

/*==================================================================================================
 *                                      数据结构
 *================================================================================================*/

/**
 * @brief 升级标志（参数区账本）
 *
 * @note 必须是 4 字节对齐、总长 8 字节的倍数（Flash 写入要求）。
 *       当前 8 个 uint32 = 32 字节。
 */
typedef struct
{
    uint32 Magic;           /**< 魔数，见 BOOT_PARAM_MAGIC */
    uint32 AppValid;        /**< 1 = App 完整可跳转；0 = 无效 */
    uint32 AppStart;        /**< App 起始地址（应为 FLS_APP_START） */
    uint32 AppSize;         /**< App 有效字节数（用于算 CRC，别超过 FLS_APP_SIZE） */
    uint32 AppCrc32;        /**< App 区的 CRC32（标准 CRC-32，与 zlib 一致） */
    uint32 BootRequest;     /**< 1 = 下次上电留在 Bootloader（App/诊断仪请求）
                             *   0 = 正常（校验通过就跳 App）
                             *
                             *   【为什么用这个字段而不是新增一个】
                             *   它原来是 Reserved0，一直是 0 —— 正好等于"没有请求"。
                             *   所以改名不改变布局（仍是 32 字节），
                             *   老账本读出来自动就是"无请求"，**向后兼容**。
                             *   注意：写它会让 StructCrc32 变化，
                             *   所以必须走 BootParam_Write 重算，不能直接改 Flash。 */
    uint32 Reserved1;       /**< 预留 */
    uint32 StructCrc32;     /**< 本结构前 28 字节的 CRC32，必须放最后一个字段 */
} Boot_ParamType;

/*==================================================================================================
 *                                        API
 *================================================================================================*/

/**
 * @brief 上电时从 Flash 读账本到 RAM
 * @details 读失败就把 RAM 里的账本标成无效（宁可留在 Bootloader，也不要跳错）。
 * @pre Fls_Init() 必须已经调用
 */
void BootParam_Load(void);

/**
 * @brief 账本本身是否有效（魔数 + 结构 CRC 都对）
 * @return TRUE 有效；FALSE 空白或被写坏
 */
boolean BootParam_IsStructValid(void);

/**
 * @brief 账本是否声称"App 有效"
 * @note 这只是"账本说有效"，不代表 App 区真的没坏 ——
 *       真正的完整性由 BootApp 模块算 CRC 来确认。
 */
boolean BootParam_IsAppValid(void);

/**
 * @brief 取账本的只读指针（供上电校验读 AppStart / AppSize / AppCrc32）
 */
const Boot_ParamType* BootParam_Get(void);

/**
 * @brief 把账本标记为"App 无效"
 *
 * @details ★ **必须在擦除 App 区之前调用**（见文件头的断电安全顺序）。
 *
 * @return E_OK 已写进 Flash；E_NOT_OK 失败（此时**不应该**继续擦除 App 区）
 */
Std_ReturnType BootParam_SetInvalid(void);

/**
 * @brief 把账本标记为"App 有效"，并记录长度和 CRC
 *
 * @param[in] AppSize   App 有效字节数
 * @param[in] AppCrc32  App 区的 CRC32（标准 CRC-32）
 *
 * @return E_OK 已写进 Flash；E_NOT_OK 失败
 *
 * @note 只有"写入完成且 CRC 校验一致"之后才允许调用。
 */
Std_ReturnType BootParam_SetValid(uint32 AppSize, uint32 AppCrc32);

/*==================================================================================================
 *                          请求"下次上电留在 Bootloader"
 *
 *  【解决什么问题】
 *  发布版把启动窗口设成 0 之后，"软件进入 Bootloader"就只剩"App 无效"这一条路了 ——
 *  一旦 App 是好的、而你又想重刷，只能上 J-Link 救。
 *
 *  这个标志就是第二条路：谁想进 Bootloader，就往账本里按下这个"门铃"再复位。
 *  Bootloader 启动时看到它，就清掉并留在 Bootloader。
 *
 *  【谁可以按门铃】
 *      app 侧   调用 BootParam_SetBootRequest() 然后触发复位
 *                  （App 需要能写 Flash —— 或者由 App 通过 UDS 例程 0xFF03 让
 *                   当前正在运行的 Bootloader 代写）
 *      诊断仪   在 Bootloader 里发 31 01 FF 03（例程控制，见 dcm_cfg.h 的
 *                  DCM_RID_REQUEST_BOOT），然后发 11 01 复位
 *
 *  【为什么放在 Flash 账本里，而不是 RAM】
 *      放 RAM 的话，断电就没了 —— "我要刷固件"这种意图必须扛得住掉电。
 *      代价是每次按门铃要擦写一个扇区（约 3.4 ms），完全可以接受。
 *================================================================================================*/
/** @brief BootRequest 字段的取值 */
#define BOOT_PARAM_BOOT_REQ_OFF     (0U)
#define BOOT_PARAM_BOOT_REQ_ON      (1U)

/**
 * @brief 账本里是否记着"请求进 Bootloader"
 * @return TRUE = 有请求（上电应留在 Bootloader，不跳 App）
 */
boolean BootParam_IsBootRequested(void);

/**
 * @brief 按下"门铃"：写 BootRequest = 1
 * @return E_OK 写成功；E_NOT_OK Flash 写失败
 * @note  写完不会自动复位 —— 调用方决定什么时候复位
 */
Std_ReturnType BootParam_SetBootRequest(void);

/**
 * @brief 清掉"门铃"：写 BootRequest = 0
 * @return E_OK 写成功；E_NOT_OK Flash 写失败
 * @note  Bootloader 启动时看到请求会调用本函数清掉，
 *        这样"同一份请求只消费一次"，不会每次上电都赖在 Bootloader 里。
 */
Std_ReturnType BootParam_ClearBootRequest(void);

#endif /* BOOT_PARAM_H */
