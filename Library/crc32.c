/*==================================================================================================
 *  src/crc32.c —— 标准 CRC-32 实现（逐位算法）
 *
 *  【为什么用逐位算法而不是查表】
 *  查表快 8 倍，但需要一张 1 KB 的表（1024 字节 Flash）。
 *  本工程的 CRC 只在两个场合用：
 *      刷写时逐块累加（每块最多 1024 字节）
 *      上电时算 App 区（几十 KB，只在启动时算一次）
 *  都不在热路径上，所以省 1 KB Flash 更划算，代码也更短、更好核对。
 *
 *  如果将来要对几 MB 的区域频繁算 CRC，再换成查表版本即可 ——
 *  **只要多项式和初值不变，两种实现算出来的结果完全一样。**
 *================================================================================================*/

#include "crc32.h"

uint32 Crc32_Update(uint32 Crc, const uint8* Data, uint32 Len)
{
    uint32 i;
    uint32 bit;

    if ((Data == NULL_PTR) || (Len == 0U))
    {
        return Crc;
    }

    for (i = 0U; i < Len; i++)
    {
        Crc ^= (uint32)Data[i];

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((Crc & 1U) != 0U)
            {
                Crc = (Crc >> 1U) ^ CRC32_POLY;
            }
            else
            {
                Crc = (Crc >> 1U);
            }
        }
    }

    return Crc;
}

uint32 Crc32_Final(uint32 Crc)
{
    return Crc ^ CRC32_FINAL_XOR;
}

uint32 Crc32_Calculate(const uint8* Data, uint32 Len)
{
    return Crc32_Final(Crc32_Update(CRC32_INIT, Data, Len));
}
