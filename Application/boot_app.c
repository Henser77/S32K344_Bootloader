/*==================================================================================================
 *  src/boot_app.c —— App 完整性校验 + 跳转实现
 *================================================================================================*/

#include "boot_app.h"
#include "boot_param.h"
#include "crc32.h"
#include "fls.h"
#include "uart.h"
#include <string.h>

/*==================================================================================================
 *                                      本地常量
 *================================================================================================*/

/*--------------------------------------------------------------------------------------------------
 * RAM 的合法范围（栈指针必须落在这两个区间之一）
 *
 * 取值来自链接脚本 Project_Settings/Linker_Files/linker_flash_s32k344.ld：
 *      int_sram        : ORIGIN = 0x20400000, LENGTH = 0x0002FF00  → 0x20400000 ~ 0x2042FEFF
 *      int_stack_dtcm  : ORIGIN = 0x2001F000, LENGTH = 0x00001000  → 0x2001F000 ~ 0x2001FFFF
 *      int_dtcm        : ORIGIN = 0x20000000, LENGTH = 0x0001F000  → 0x20000000 ~ 0x2001EFFF
 *
 * 这里用"包含整个 SRAM / DTCM 区"的宽松边界，只要 SP 落在里面就算合法。
 * 检查的目的不是精确验证，而是"拦住明显荒谬的值"（比如 0xFFFFFFFF、0x00000000）。
 *------------------------------------------------------------------------------------------------*/
#define BOOT_RAM_SRAM_START         (0x20400000UL)
#define BOOT_RAM_SRAM_END           (0x20443FF0UL)      /* 到 ram_rsvd2 之前 */
#define BOOT_RAM_DTCM_START         (0x20000000UL)
#define BOOT_RAM_DTCM_END           (0x20020000UL)

/** @brief Cortex-M 的 VTOR 寄存器地址（SCB->VTOR） */
#define BOOT_REG_VTOR               (*(volatile uint32*)0xE000ED08UL)

/*==================================================================================================
 *                                      内部辅助
 *================================================================================================*/

/** @brief 栈指针是否落在合法 RAM 范围内，且 8 字节对齐（AAPCS 要求） */
static boolean BootApp_IsValidStackPointer(uint32 Sp)
{
    if ((Sp & 0x7UL) != 0U)
    {
        return FALSE;       /* 未 8 字节对齐 —— AAPCS 要求函数入口 SP 8 字节对齐 */
    }

    if ((Sp >= BOOT_RAM_SRAM_START) && (Sp <= BOOT_RAM_SRAM_END))
    {
        return TRUE;
    }

    if ((Sp >= BOOT_RAM_DTCM_START) && (Sp <= BOOT_RAM_DTCM_END))
    {
        return TRUE;
    }

    return FALSE;
}

/** @brief 复位向量是否落在 App 区内，且带 Thumb 位（bit0 = 1） */
static boolean BootApp_IsValidResetVector(uint32 Pc, uint32 AppStart, uint32 AppSize)
{
    uint32 appEnd = AppStart + AppSize;

    /* Cortex-M 的向量表里，函数地址的 bit0 必须是 1（表示 Thumb 指令集）。
     * 如果 bit0 == 0，说明这个"代码地址"根本不是合法入口。 */
    if ((Pc & 0x1UL) == 0U)
    {
        return FALSE;
    }

    /* 去掉 Thumb 位之后必须在 App 区内 */
    Pc &= ~0x1UL;

    return ((Pc >= AppStart) && (Pc < appEnd)) ? TRUE : FALSE;
}

/*==================================================================================================
 *                                        API 实现
 *================================================================================================*/

Std_ReturnType BootApp_CalcCrc(uint32 Address, uint32 Size, uint32* Crc32Out)
{
    uint8  buf[BOOT_APP_CRC_CHUNK];
    uint32 offset = 0U;
    uint32 crc    = CRC32_INIT;

    if (Crc32Out == NULL_PTR)
    {
        return E_NOT_OK;
    }

    if ((Size == 0U) || (Size > BOOT_APP_MAX_SIZE))
    {
        return E_NOT_OK;
    }

    while (offset < Size)
    {
        uint32 chunk = Size - offset;

        if (chunk > (uint32)BOOT_APP_CRC_CHUNK)
        {
            chunk = (uint32)BOOT_APP_CRC_CHUNK;
        }

        if (Fls_Read((Fls_AddressType)(Address + offset), buf,
                     (Fls_LengthType)chunk) != E_OK)
        {
            return E_NOT_OK;
        }

        crc = Crc32_Update(crc, buf, chunk);

        offset += chunk;
    }

    *Crc32Out = Crc32_Final(crc);
    return E_OK;
}

Boot_DecideResultType BootApp_Decide(void)
{
    const Boot_ParamType* param;
    uint32 sp;
    uint32 pc;
    uint32 crcNow = 0U;

    /*--------------------------------------------------------------------------
     * ① 账本结构本身有效吗？
     *------------------------------------------------------------------------*/
    if (BootParam_IsStructValid() == FALSE)
    {
        Uart_Printf("App 校验 : 参数区没有有效账本（从未刷写过 / 已被擦除）\n");
        return BOOT_DECIDE_STAY;
    }

    param = BootParam_Get();

    /*--------------------------------------------------------------------------
     * ② 账本说 App 有效吗？
     *------------------------------------------------------------------------*/
    if (BootParam_IsAppValid() == FALSE)
    {
        Uart_Printf("App 校验 : 账本标记 App 无效（上次刷写没完成，或正在刷写中）\n");
        return BOOT_DECIDE_STAY;
    }

    /*--------------------------------------------------------------------------
     * ③ 账本里的 App 起始地址对吗？
     *    这一条防的是"账本被写成了别的地址"这种极端情况。
     *------------------------------------------------------------------------*/
    if (param->AppStart != BOOT_APP_START)
    {
        Uart_Printf("App 校验 : 账本里的 App 地址 0x%08X 不是期望的 0x%08X\n",
                    param->AppStart, BOOT_APP_START);
        return BOOT_DECIDE_STAY;
    }

    /*--------------------------------------------------------------------------
     * ④ App 长度在合理范围吗？
     *------------------------------------------------------------------------*/
    if ((param->AppSize == 0U) || (param->AppSize > BOOT_APP_MAX_SIZE))
    {
        Uart_Printf("App 校验 : 账本里的 App 长度 %u 不合法（上限 %u）\n",
                    param->AppSize, BOOT_APP_MAX_SIZE);
        return BOOT_DECIDE_STAY;
    }

    /*--------------------------------------------------------------------------
     * ⑤⑥ 读 App 的向量表，检查前两个字
     *      [0] = 初始 MSP，[1] = Reset_Handler 地址
     *
     * 只读 8 字节，但这一步能挡住绝大多数"跳进去跑飞"的情况。
     *------------------------------------------------------------------------*/
    if (Fls_Read((Fls_AddressType)BOOT_APP_START, (uint8*)&sp, 4U) != E_OK)
    {
        Uart_Printf("App 校验 : 读 App 向量表失败\n");
        return BOOT_DECIDE_STAY;
    }

    if (Fls_Read((Fls_AddressType)(BOOT_APP_START + 4U), (uint8*)&pc, 4U) != E_OK)
    {
        Uart_Printf("App 校验 : 读 App 向量表失败\n");
        return BOOT_DECIDE_STAY;
    }

    Uart_Printf("App 校验 : 向量表 SP=0x%08X  PC=0x%08X\n", sp, pc);

    if (BootApp_IsValidStackPointer(sp) == FALSE)
    {
        Uart_Printf("App 校验 : 初始栈指针 0x%08X 不在 RAM 范围（App 区可能是空的）\n", sp);
        return BOOT_DECIDE_STAY;
    }

    if (BootApp_IsValidResetVector(pc, BOOT_APP_START, param->AppSize) == FALSE)
    {
        Uart_Printf("App 校验 : 复位向量 0x%08X 不在 App 区内（0x%08X~0x%08X）\n",
                    pc, BOOT_APP_START, BOOT_APP_START + param->AppSize);
        return BOOT_DECIDE_STAY;
    }

    /*--------------------------------------------------------------------------
     * ⑦ 算 App 区 CRC，跟账本比对
     *
     * 这是唯一能证明"内容完整"的检查 —— 前面几条只能证明"看起来像代码"。
     * 刷到一半断电的情况，只有 CRC 能发现。
     *------------------------------------------------------------------------*/
    if (BootApp_CalcCrc(BOOT_APP_START, param->AppSize, &crcNow) != E_OK)
    {
        Uart_Printf("App 校验 : 计算 App 区 CRC 失败\n");
        return BOOT_DECIDE_STAY;
    }

    if (crcNow != param->AppCrc32)
    {
        Uart_Printf("App 校验 : CRC 不一致！Flash 里 0x%08X，账本记 0x%08X\n",
                    crcNow, param->AppCrc32);
        Uart_Printf("           App 区内容不完整，留在 Bootloader 等重刷\n");
        return BOOT_DECIDE_STAY;
    }

    Uart_Printf("App 校验 : CRC 一致（0x%08X，%u 字节）→ 可以跳转\n",
                crcNow, param->AppSize);

    return BOOT_DECIDE_JUMP;
}

void BootApp_Jump(uint32 AppStart)
{
    uint32 sp;
    uint32 pc;

    /* 读 App 向量表的头两个字：初始 MSP 和 Reset_Handler 地址
     *（调用前 BootApp_Decide 已经检查过它们合法）*/
    sp = *(volatile uint32*)AppStart;
    pc = *(volatile uint32*)(AppStart + 4U);

    /*--------------------------------------------------------------------------
     * ① 关总中断
     *
     * 之后 Bootloader 注册的所有中断都不能再进来 ——
     * 因为它们的处理函数地址在 Bootloader 里，而 App 马上要接管。
     *------------------------------------------------------------------------*/
    __asm volatile ("cpsid i" ::: "memory");

    /*--------------------------------------------------------------------------
     * ② 设向量表偏移
     *
     * Cortex-M 复位后 VTOR = 0，中断向量从 0x00000000 取。
     * 我们不搬向量表，而是告诉内核"向量表在 AppStart"。
     * 这样 App 里发生中断时，取到的就是 App 自己的处理函数。
     *------------------------------------------------------------------------*/
    BOOT_REG_VTOR = AppStart;

    /*--------------------------------------------------------------------------
     * ③④⑤ 换栈 + 屏障 + 跳转，一气呵成
     *
     * 【为什么必须在同一段汇编里做完】
     * 一旦执行了 msr msp，我们自己的局部变量（在 Bootloader 的栈上）
     * 就全都不再可靠了。所以 sp / pc 必须已经在寄存器里，
     * 而且之后只能做寄存器跳转，不能再访问内存。
     *------------------------------------------------------------------------*/
    __asm volatile (
        "msr  msp, %0   \n\t"       /* 换成 App 的初始栈指针 */
        "dsb            \n\t"       /* 数据同步屏障：保证前面的写（VTOR）已经生效 */
        "isb            \n\t"       /* 指令同步屏障：清流水线，让后面取到的是新指令 */
        "bx   %1        \n\t"       /* 跳到 App 的 Reset_Handler（pc 的 bit0=1 表示 Thumb）*/
        :
        : "r" (sp), "r" (pc)
        : "memory"
    );

    /* 正常不会执行到这里；万一 App 的 Reset_Handler 返回了，
     * 停在这里比跑飞好（跑飞可能触发 HardFault 死循环，更难查）*/
    for (;;)
    {
    }
}
