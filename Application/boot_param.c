/*==================================================================================================
 *  src/boot_param.c —— 升级标志（参数区）实现
 *
 *  【写入策略：整扇区擦 + 整个结构重写】
 *  参数区只有一个扇区（8 KB），账本只占 32 字节。
 *  改任何字段都要"擦整个扇区 → 写回 32 字节"，因为 Flash 只能 1→0，
 *  不能就地改。
 *
 *  【代价】
 *  擦一个扇区约 3.4 毫秒。只在"开始刷写"和"刷写成功"这两个时刻各做一次，
 *  对刷写流程没有影响。
 *
 *  【为什么不做成"双备份 + 轮换"】
 *  那种方案能防止"擦除参数区时掉电导致账本全丢"。
 *  本工程采用更简单也更安全的顺序：**账本丢了 = App 无效 = 留在 Bootloader**，
 *  这个退路本身就是安全的（最坏情况是"刷完的固件要重刷一次"，不是变砖）。
 *================================================================================================*/

#include "boot_param.h"
#include "crc32.h"
#include "fls.h"
#include "uart.h"
#include <string.h>

/*==================================================================================================
 *                                      本地变量
 *================================================================================================*/

/** @brief RAM 里的账本副本（上电读一次，之后都是读它） */
static Boot_ParamType BootParam_Ram;

/** @brief 读到的账本是否通过校验 */
static boolean BootParam_Valid = FALSE;

/*==================================================================================================
 *                                      内部辅助
 *================================================================================================*/

/**
 * @brief 算账本自身的 CRC（覆盖 StructCrc32 之前的所有字节）
 */
static uint32 BootParam_CalcStructCrc(const Boot_ParamType* Param)
{
    uint32 len = (uint32)sizeof(Boot_ParamType) - (uint32)sizeof(uint32);

    return Crc32_Calculate((const uint8*)Param, len);
}

/**
 * @brief 把 RAM 里的账本整个写进 Flash
 *
 * @details 步骤：算结构 CRC → 擦参数扇区 → 写 32 字节 → 回读校验
 *
 * @note 回读校验用 `Fls_Read`（它内部会让 CPU D-Cache 失效，
 *       所以读到的确实是 Flash 里的内容，不会被缓存骗）。
 */
static Std_ReturnType BootParam_Write(const Boot_ParamType* Param)
{
    Boot_ParamType check;

    /* ① 先把结构 CRC 算好填进去（它覆盖之前的字段，所以必须最后算） */
    BootParam_Ram = *Param;
    BootParam_Ram.StructCrc32 = BootParam_CalcStructCrc(&BootParam_Ram);

    /* ② 擦参数扇区 */
    if (Fls_Erase((Fls_AddressType)BOOT_PARAM_ADDR,
                  (Fls_LengthType)FLS_PARAM_SIZE) != E_OK)
    {
        return E_NOT_OK;
    }

    /* ③ 写 32 字节（Fls_Write 内部会按 8 字节对齐补齐） */
    if (Fls_Write((Fls_AddressType)BOOT_PARAM_ADDR,
                  (const uint8*)&BootParam_Ram,
                  (Fls_LengthType)sizeof(Boot_ParamType)) != E_OK)
    {
        return E_NOT_OK;
    }

    /* ④ 回读校验：确认真的写进去了 */
    (void)memset(&check, 0, sizeof(check));

    if (Fls_Read((Fls_AddressType)BOOT_PARAM_ADDR,
                 (uint8*)&check,
                 (Fls_LengthType)sizeof(Boot_ParamType)) != E_OK)
    {
        return E_NOT_OK;
    }

    if ((check.Magic       != BootParam_Ram.Magic)     ||
        (check.AppValid    != BootParam_Ram.AppValid)  ||
        (check.AppSize     != BootParam_Ram.AppSize)   ||
        (check.AppCrc32    != BootParam_Ram.AppCrc32)  ||
        (check.BootRequest != BootParam_Ram.BootRequest) ||
        (check.StructCrc32 != BootParam_Ram.StructCrc32))
    {
        return E_NOT_OK;
    }

    return E_OK;
}

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

void BootParam_Load(void)
{
    (void)memset(&BootParam_Ram, 0, sizeof(BootParam_Ram));
    BootParam_Valid = FALSE;

    if (Fls_Read((Fls_AddressType)BOOT_PARAM_ADDR,
                 (uint8*)&BootParam_Ram,
                 (Fls_LengthType)sizeof(BootParam_Ram)) != E_OK)
    {
        return;         /* 读失败 → 当作无效 */
    }

    /* 空白 Flash（全 0xFF）时 Magic 对不上 → 直接判无效，不报错 */
    if (BootParam_Ram.Magic != BOOT_PARAM_MAGIC)
    {
        return;
    }

    if (BootParam_Ram.StructCrc32 != BootParam_CalcStructCrc(&BootParam_Ram))
    {
        /* 结构被写坏了（例如写参数区时掉电）—— 当作无效，
         * 退路是"留在 Bootloader"，这是安全的。 */
        BootParam_Ram.Magic = 0U;       /* 顺手清掉，避免下次又走这条分支 */
        return;
    }

    BootParam_Valid = TRUE;
}

boolean BootParam_IsStructValid(void)
{
    return BootParam_Valid;
}

boolean BootParam_IsAppValid(void)
{
    if (BootParam_Valid == FALSE)
    {
        return FALSE;
    }

    return (BootParam_Ram.AppValid == BOOT_PARAM_APP_VALID) ? TRUE : FALSE;
}

const Boot_ParamType* BootParam_Get(void)
{
    return &BootParam_Ram;
}

Std_ReturnType BootParam_SetInvalid(void)
{
    Boot_ParamType p;

    (void)memset(&p, 0, sizeof(p));
    p.Magic    = BOOT_PARAM_MAGIC;
    p.AppValid = BOOT_PARAM_APP_INVALID;
    p.AppStart = (uint32)FLS_APP_START;
    p.AppSize  = 0U;
    p.AppCrc32 = 0U;

    /* ★ 擦除（置无效）不应丢掉"请求进 Bootloader"这个意图。
     *   刷写流程是：擦除 -> 置无效 -> 下载 -> 校验 -> 置有效，
     *   如果置无效时把 BootRequest 清掉，那么"标记后擦除再断电"就会
     *   丢掉请求，下次上电又跳 App —— 与使用者的意图相反。 */
    p.BootRequest = BootParam_Ram.BootRequest;

    if (BootParam_Write(&p) != E_OK)
    {
        return E_NOT_OK;
    }

    BootParam_Valid = TRUE;
    return E_OK;
}

Std_ReturnType BootParam_SetValid(uint32 AppSize, uint32 AppCrc32)
{
    Boot_ParamType p;

    /* 长度必须落在 App 区范围内，否则拒绝（防止上层传错参数把账本写成垃圾） */
    if ((AppSize == 0U) || (AppSize > (uint32)FLS_APP_SIZE))
    {
        return E_NOT_OK;
    }

    (void)memset(&p, 0, sizeof(p));
    p.Magic    = BOOT_PARAM_MAGIC;
    p.AppValid = BOOT_PARAM_APP_VALID;
    p.AppStart = (uint32)FLS_APP_START;
    p.AppSize  = AppSize;
    p.AppCrc32 = AppCrc32;

    /* ★ 刷写完成、验货通过 -> 把"请求进 Bootloader"自动清掉。
     *   语义是："App 已经是新的了，下次上电正常去跑它"。 */
    p.BootRequest = BOOT_PARAM_BOOT_REQ_OFF;

    if (BootParam_Write(&p) != E_OK)
    {
        return E_NOT_OK;
    }

    BootParam_Valid = TRUE;
    return E_OK;
}

/*==================================================================================================
 *                          请求"下次上电留在 Bootloader"
 *================================================================================================*/

/**
 * @brief 把当前账本改成带某个 BootRequest 值并落盘
 *
 * @details 两种起点都要处理：
 *            账本有效    -> 以现有内容为基准，只改 BootRequest（保留 App 信息）
 *            账本无效    -> 先建一个"App 无效"的最小账本，再置位
 *                          （否则 Flash 里全是 0xFF，Magic 对不上，
 *                            下次上电连"有请求"都读不出来）
 */
static Std_ReturnType BootParam_WriteBootRequest(uint32 Value)
{
    Boot_ParamType p;

    if (BootParam_Valid != FALSE)
    {
        p = BootParam_Ram;              /* 以现有账本为基准 */
    }
    else
    {
        (void)memset(&p, 0, sizeof(p));
        p.Magic    = BOOT_PARAM_MAGIC;
        p.AppValid = BOOT_PARAM_APP_INVALID;
        p.AppStart = (uint32)FLS_APP_START;
        p.AppSize  = 0U;
        p.AppCrc32 = 0U;
    }

    if (p.BootRequest == Value)
    {
        return E_OK;                    /* 值已经对了，不必擦写 Flash */
    }

    p.BootRequest = Value;

    if (BootParam_Write(&p) != E_OK)
    {
        return E_NOT_OK;
    }

    BootParam_Valid = TRUE;
    return E_OK;
}

boolean BootParam_IsBootRequested(void)
{
    if (BootParam_Valid == FALSE)
    {
        return FALSE;                   /* 账本无效 = 没有请求（无效本身就会留在 Bootloader） */
    }

    return (BootParam_Ram.BootRequest == BOOT_PARAM_BOOT_REQ_ON) ? TRUE : FALSE;
}

Std_ReturnType BootParam_SetBootRequest(void)
{
    return BootParam_WriteBootRequest(BOOT_PARAM_BOOT_REQ_ON);
}

Std_ReturnType BootParam_ClearBootRequest(void)
{
    return BootParam_WriteBootRequest(BOOT_PARAM_BOOT_REQ_OFF);
}
