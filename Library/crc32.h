/*==================================================================================================
 *  src/crc32.h —— 共享的 CRC32 实现
 *
 *  【为什么要单独一个模块】
 *  本工程有两处需要 CRC32，而且**两处算出来的值必须是同一个**：
 *      ① Dcm 在接收 TransferData 时逐块累加，用于跟 Flash 里读出来的值比对
 *      ② Bootloader 上电时算整个 App 区的 CRC，用于决定"这个 App 完不完整"
 *
 *  以前 dcm.c 里有一份私有的 CRC32 实现。如果上电校验再用另一份，
 *  两处一旦有一个字节不同（多项式 / 初值 / 输出异或 写错），
 *  就会出现"刷写时说一致、上电说坏"这种极难排查的问题。
 *  所以抽成一个模块，两处共用。
 *
 *  【用的是哪个标准】
 *  标准 CRC-32（和 zlib.crc32 / Python 的 zlib.crc32 完全一致）：
 *      多项式    0xEDB88320（反射形式）
 *      初值      0xFFFFFFFF
 *      输入反射  是
 *      输出反射  是
 *      输出异或  0xFFFFFFFF
 *  所以可以用 Python 独立验证：
 *      import zlib; hex(zlib.crc32(bytes.fromhex("11223344")))
 *================================================================================================*/
#ifndef CRC32_H
#define CRC32_H

#include "Std_Types.h"

/** @brief 标准 CRC-32 的多项式（反射形式） */
#define CRC32_POLY                  (0xEDB88320UL)

/** @brief 初值 */
#define CRC32_INIT                  (0xFFFFFFFFUL)

/** @brief 输出异或值 */
#define CRC32_FINAL_XOR             (0xFFFFFFFFUL)

/**
 * @brief 累加计算（可以分块调用）
 *
 * @param[in] Crc  上一块的中间值（第一次传 CRC32_INIT）
 * @param[in] Data 数据
 * @param[in] Len  长度
 *
 * @return 更新后的中间值（**还没有做输出异或**）
 *
 * @note 分块累加和一次性算是等价的，所以刷写时可以边收边算。
 */
uint32 Crc32_Update(uint32 Crc, const uint8* Data, uint32 Len);

/**
 * @brief 一次性算完（内部做初值和输出异或）
 * @return 最终的 CRC32 值，可直接与 zlib.crc32() 比对
 */
uint32 Crc32_Calculate(const uint8* Data, uint32 Len);

/**
 * @brief 把中间值转成最终值（输出异或）
 */
uint32 Crc32_Final(uint32 Crc);

#endif /* CRC32_H */
