/*==================================================================================================
 *  src/boot_app.h —— App 完整性校验 + 跳转
 *
 *  【这个模块负责 Bootloader 存在的意义本身】
 *  上电后回答两个问题：
 *      ① App 区那份固件是完整的吗？（算 CRC32 跟账本比对）
 *      ② 能跳吗？跳转前的 CPU 状态该设成什么样？
 *
 *  【判断链（任何一步失败都"留在 Bootloader"，绝不冒险跳）】
 *
 *      ├─ 账本结构有效吗？          → 否 → 留在 Bootloader
 *      ├─ 账本说 App 有效吗？        → 否 → 留在 Bootloader
 *      ├─ AppStart 是 FLS_APP_START 吗？ → 否 → 留在 Bootloader
 *      ├─ AppSize 在合理范围吗？     → 否 → 留在 Bootloader
 *      ├─ 向量表里的 SP 合法吗？     → 否 → 留在 Bootloader
 *      ├─ 向量表里的 PC 落在 App 区吗？ → 否 → 留在 Bootloader
 *      ├─ App 区 CRC32 与账本一致吗？ → 否 → 留在 Bootloader
 *      └─ 全部通过 → 跳转
 *
 *  【为什么每一步都要单独检】
 *  这 7 步里任何一步跳过，都可能让 CPU 跳进一块"看起来像代码的垃圾"，
 *  然后 HardFault 或死循环 —— 而且那时诊断仪连不上（因为已经不在 Bootloader 了），
 *  只能拆板子重新烧。
 *
 *  **"留在 Bootloader"永远比"冒险跳一次"划算。**
 *================================================================================================*/
#ifndef BOOT_APP_H
#define BOOT_APP_H

#include "Std_Types.h"
#include "fls_cfg.h"

/*==================================================================================================
 *                                      配置
 *================================================================================================*/

/** @brief App 起始地址（向量表就在这里） */
#define BOOT_APP_START              ((uint32)FLS_APP_START)

/** @brief App 区最大容量 */
#define BOOT_APP_MAX_SIZE           ((uint32)FLS_APP_SIZE)

/**
 * @brief 启动窗口：上电后先等这么久（毫秒），期间收到 UDS 请求就留在 Bootloader
 *
 * @details 【解决什么问题】
 *  App 有效时 Bootloader 上电就跳走。而 App 通常不认 UDS，
 *  于是"想再刷一次"就必须回去擦参数区 —— 只能靠调试器，非常别扭。
 *
 *  有了这个窗口：
 *      上电 -> Bootloader 开着 CAN 等 1.5 秒
 *          |- 这期间收到任意 UDS 请求 -> 留在 Bootloader（可以刷写）
 *          \- 一直没人来 -> 跳 App
 *  这样"想刷就刷"：上位机先开始循环发 3E 00，再给板子断电重上电即可。
 *
 * @note 设成 0 表示关闭窗口（上电直接跳，启动最快）。
 *       真实产品一般用"App 收到 10 02 后写标志再复位"的做法，
 *       那个方案不需要每次上电都等，但要 App 里有一套最小 UDS。
 */
/* BOOT_HOLD_WINDOW_MS 已由 Library/build_cfg.h 派生：
 *   开发版 1500 ms（配上位机 --wait，想刷就刷）
 *   发布版 0    （上电直接跳 App，启动最快） */

/**
 * @brief 算 App 区 CRC 时的分块大小
 * @details 一次性读几十 KB 到栈上会爆栈，所以分块累加。
 *          128 字节正好是 Flash 驱动一次操作的推荐粒度。
 */
#define BOOT_APP_CRC_CHUNK          (128U)

/*==================================================================================================
 *                                      类型
 *================================================================================================*/

/** @brief 上电决策的结果 */
typedef enum
{
    BOOT_DECIDE_STAY = 0,       /**< 留在 Bootloader（等待诊断仪刷写）*/
    BOOT_DECIDE_JUMP = 1        /**< 校验通过，可以跳转 */
} Boot_DecideResultType;

/*==================================================================================================
 *                                        API
 *================================================================================================*/

/**
 * @brief 上电决策：App 能不能跳？
 *
 * @details 会依次检查文件头列出的 7 个条件，并打印每一步的结果。
 *          只做判断，**不做跳转** —— 跳不跳由调用者决定。
 *
 * @return BOOT_DECIDE_JUMP  全部检查通过
 *         BOOT_DECIDE_STAY  任何一步失败（失败原因已打印）
 *
 * @pre Fls_Init() 与 BootParam_Load() 必须先调用
 */
Boot_DecideResultType BootApp_Decide(void);

/**
 * @brief 计算一段 Flash 区域的 CRC32
 *
 * @param[in]  Address     起始地址
 * @param[in]  Size        长度
 * @param[out] Crc32Out    算出的 CRC32
 *
 * @return E_OK 成功；E_NOT_OK 读 Flash 失败或参数非法
 *
 * @note 分块读取累加，不会一次占用大块 RAM。
 */
Std_ReturnType BootApp_CalcCrc(uint32 Address, uint32 Size, uint32* Crc32Out);

/**
 * @brief 跳到 App（**本函数不返回**）
 *
 * @details 执行顺序（顺序本身很重要）：
 *              ① 关总中断（cpsid i）—— 后面 Bootloader 的中断不能再进来
 *              ② 设向量表偏移（VTOR = App 起始地址）
 *              ③ 把 MSP 换成 App 的初始栈指针
 *              ④ 屏障，保证前面全部生效
 *              ⑤ 跳到 App 的 Reset_Handler
 *
 * @param[in] AppStart App 向量表地址
 *
 * @note 调用本函数之前，调用方应当先把板级外设恢复到可控状态
 *       （停掉 CAN 控制器等），否则 App 重新初始化外设时可能出问题。
 *       本模块刻意只管 CPU 层面的事，把"板级清理"留给 EcuM —— 那里也是
 *       板级初始化的地方，两者成对出现更好维护。
 */
void BootApp_Jump(uint32 AppStart);

#endif /* BOOT_APP_H */
