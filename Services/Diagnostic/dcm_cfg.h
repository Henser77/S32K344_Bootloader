/*==================================================================================================
 *  Mcal/dcm_cfg.h —— Dcm（UDS 诊断服务层）配置
 *
 *  【Dcm 是什么】
 *  CanTp 把"一条完整的消息"交给 Dcm，Dcm 负责**看懂这条消息是什么命令**，
 *  执行它，然后**组织一条合适的响应**发回去。
 *
 *  例：
 *      CanTp 交给 Dcm：  10 03
 *      Dcm 看懂：        服务 0x10（切换会话），子功能 0x03（扩展会话）
 *      Dcm 动作：        把当前会话切到扩展会话
 *      Dcm 响应：        50 03 00 32 01 F4
 *
 *  【对应标准】
 *  ISO 14229-1（UDS 应用层）—— 服务号、子功能、负响应码全部按标准。
 *  所以任何符合 ISO 14229-1 的诊断仪 / CANoe / 我们后面写的 Python 工具都能对接。
 *================================================================================================*/
#ifndef DCM_CFG_H
#define DCM_CFG_H

/* Std_Types 提供 STD_ON / STD_OFF；boot_version.h 提供 BOOT_RELEASE_MODE。
 * 注意顺序：BOOT_RELEASE_MODE 的判断在下面用到，必须在这里就可见。 */
#include "Std_Types.h"
#include "boot_version.h"

/*==================================================================================================
 *                                    服务号（SID）
 *================================================================================================*/
#define DCM_SID_DIAGNOSTIC_SESSION_CONTROL   (0x10U)
#define DCM_SID_ECU_RESET                    (0x11U)
#define DCM_SID_SECURITY_ACCESS              (0x27U)
#define DCM_SID_TESTER_PRESENT               (0x3EU)
#define DCM_SID_ROUTINE_CONTROL              (0x31U)
#define DCM_SID_REQUEST_DOWNLOAD             (0x34U)
#define DCM_SID_TRANSFER_DATA                (0x36U)
#define DCM_SID_REQUEST_TRANSFER_EXIT        (0x37U)

/** @brief 正响应 = 请求 SID + 0x40 */
#define DCM_POSITIVE_RESPONSE_OFFSET         (0x40U)

/** @brief 负响应固定第一个字节 */
#define DCM_NEGATIVE_RESPONSE_SID            (0x7FU)

/*==================================================================================================
 *                                  负响应码（NRC）
 *
 *  【全部按 ISO 14229-1 定义】
 *  这些数字是标准化的 —— 诊断仪靠它判断"为什么失败"。
 *  自己乱编数字别人就不认了。
 *================================================================================================*/
#define DCM_NRC_GENERAL_REJECT                       (0x10U)
#define DCM_NRC_SERVICE_NOT_SUPPORTED                (0x11U)
#define DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED           (0x12U)
#define DCM_NRC_INCORRECT_LENGTH_OR_FORMAT           (0x13U)
#define DCM_NRC_CONDITIONS_NOT_CORRECT               (0x22U)
#define DCM_NRC_REQUEST_SEQUENCE_ERROR               (0x24U)
#define DCM_NRC_REQUEST_OUT_OF_RANGE                 (0x31U)
#define DCM_NRC_SECURITY_ACCESS_DENIED               (0x33U)
#define DCM_NRC_INVALID_KEY                          (0x35U)
#define DCM_NRC_EXCEED_NUMBER_OF_ATTEMPTS            (0x36U)
#define DCM_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED      (0x37U)
#define DCM_NRC_RESPONSE_PENDING                     (0x78U)
/* 0x72 generalProgrammingFailure：Flash 写/擦失败的标准 NRC。
 * 比用 0x22（conditionsNotCorrect）更贴切 —— 诊断仪一看就知道是"编程失败"。 */
#define DCM_NRC_GENERAL_PROGRAMMING_FAILURE           (0x72U)
#define DCM_NRC_SUB_FUNCTION_NOT_SUPPORTED_IN_SESSION (0x7EU)
#define DCM_NRC_SERVICE_NOT_SUPPORTED_IN_SESSION     (0x7FU)

/*==================================================================================================
 *                                    会话（Session）
 *
 *  【为什么要有会话】
 *  防止"插上 CAN 盒子就能刷 ECU"。
 *  不同会话下允许的服务不同 —— 默认会话只能读基本信息，
 *  要擦写 Flash 必须先切到编程会话，还得先通过安全解锁。
 *================================================================================================*/
#define DCM_SESSION_DEFAULT                  (0x01U)    /**< 默认会话（上电就在这） */
#define DCM_SESSION_PROGRAMMING              (0x02U)    /**< 编程会话（擦写 Flash 必须） */
#define DCM_SESSION_EXTENDED                 (0x03U)    /**< 扩展会话（诊断、读数据用） */

/*==================================================================================================
 *                                  定时参数
 *================================================================================================*/
/** @brief P2（诊断仪等响应的时间，毫秒）—— ISO 14229-1 规定典型 50 ms */
#define DCM_P2_MS                            (50U)

/** @brief P2*（收到 NRC 0x78 之后诊断仪延长到的超时，毫秒）—— 典型 5000 ms */
#define DCM_P2_STAR_MS                       (5000U)

/** @brief S3（会话保持时间，毫秒）—— 超过这个时间没有 0x3E 就退回默认会话
 *
 *  @details 【为什么要有手工测试模式】
 *           标准值 5000 ms。但手工用 USB-CAN 工具测试时，
 *           填 8 个字节 + 点发送经常就超过 5 秒了，会话会不停掉回 DEFAULT，
 *           然后就得从 0x10 03 重新来过 —— 完全没法测。
 *
 *           打开 DCM_MANUAL_TEST_MODE 把 S3 放宽到 10 分钟，
 *           手工测试就不用抢时间。
 *
 *           ⚠️ 正式产品必须把 DCM_MANUAL_TEST_MODE 设为 STD_OFF。
 *              10 分钟的会话保持时间意味着：诊断仪拔线后 ECU 会一直停在
 *              "编程会话 + 可能已解锁"状态 —— 这是安全隐患。
 */
/* DCM_MANUAL_TEST_MODE 已由 Library/build_cfg.h 派生：
 *   开发版 STD_ON（S3 = 120 秒，手工测试不用抢时间）
 *   发布版 STD_OFF（S3 = 5 秒，符合标准） */

#if (DCM_MANUAL_TEST_MODE == STD_ON)
    /* 时间基准量程 71.6 分钟（软件扩展的微秒计数器），2 分钟完全没问题。 */
    #define DCM_S3_MS                    (120000U)   /* 手工测试：2 分钟 */
#else
    #define DCM_S3_MS                    (5000U)     /* 标准：5 秒 */
#endif

/** @brief 会话保持超时（换算成微秒，与时间基准一致） */
#define DCM_S3_US                            ((uint32)DCM_S3_MS * 1000UL)

/** @brief 安全延时 10 秒 → 微秒（10 秒远小于 26.84 秒上限，安全） */
#define DCM_SEC_DELAY_US                     (10000UL * 1000UL)

/*==================================================================================================
 *                                  安全访问（0x27）
 *
 *  【真实项目里算法是保密的】
 *  车厂和供应商约定一套种子/密钥算法，不公开。
 *  学习项目用一个**简单但确定的**变换，重点是流程完整：
 *      请求种子 → 算出密钥 → 发回 → 校验 → 失败计数 → 超限锁定
 *
 *  ⚠️ 如果将来要给真实项目用，只需要把 Dcm_CalcKey() 换成真实算法，
 *     其余流程一行都不用改 —— 这就是把算法和流程分开的价值。
 *================================================================================================*/
#define DCM_SEC_LEVEL1_SEED_SUB              (0x01U)    /**< 子功能 0x01 = 请求种子 */
#define DCM_SEC_LEVEL1_KEY_SUB               (0x02U)    /**< 子功能 0x02 = 发送密钥 */

#define DCM_SEC_MAX_ATTEMPTS                 (3U)       /**< 允许的错误次数 */
#define DCM_SEC_KEY_XOR                      (0x5A5A5A5AU)
#define DCM_SEC_KEY_ADD                      (0x12345678U)

/*==================================================================================================
 *                                  例程（0x31）标识符
 *================================================================================================*/
#define DCM_RID_ERASE_MEMORY                 (0xFF00U)   /**< 擦除 Flash */
#define DCM_RID_CHECK_INTEGRITY              (0xFF01U)   /**< 校验（算 CRC32） */

/**
 * @brief 「信任当前 App 区」调试例程（0x31 01 FF02 <长度4字节>）
 *
 * @details 用途：把用调试器直接烧进 0x00500000 的 App "认账" ——
 *          算一遍 App 区的 CRC32 写进参数区，让下一次上电能跳过去。
 *
 *          **正式发布必须删掉这个例程**（或者用 DCM_ENABLE_TRUST_APP 开关关掉），
 *          因为它允许"不解锁校验就承认任意内容为合法固件"。
 *          但注意它仍然要求"编程会话 + 安全解锁"，所以不是完全敞开的门。
 */
/* DCM_ENABLE_TRUST_APP 已由 Library/build_cfg.h 派生：
 *   开发版 ON（0xFF02 可以直接信任 App 区，省去重刷校验）
 *   发布版 OFF（不允许"不验货就签收"） */
#define DCM_RID_TRUST_APP_AREA               (0xFF02U)   /**< 信任当前 App 区（调试用） */

/**
 * @brief 例程 0xFF03 —— 请求"下次上电留在 Bootloader"
 *
 * @details 请求格式：  31 01 FF 03
 *          响应格式：  71 01 FF 03 00
 *
 *          【用途】发布版的启动窗口是 0（上电直接跳 App），
 *                  如果 App 是好的但你想重刷，就需要这一条"门铃"：
 *                      31 01 FF 03   -> 写参数区 BootRequest = 1
 *                      11 01         -> 复位
 *                      上电 -> Bootloader 看到请求 -> 清掉并留下 -> 可以刷
 *
 *          【和 0xFF02 的区别】0xFF02 是调试后门（发布版关闭）；
 *          这个是真的产品功能，两个构建模式都保留。
 *
 *          【要求】编程会话 + 安全解锁（和 0xFF00 一样）。
 */
#define DCM_RID_REQUEST_BOOT                 (0xFF03U)

/**
 * @brief 例程 0xFF04 —— 取消上面的请求（BootRequest = 0）
 * @details 请求：31 01 FF 04   响应：71 01 FF 04 00
 *          用途：按错了门铃、又不想留在 Bootloader 时撤销。
 */
#define DCM_RID_CLEAR_BOOT_REQUEST           (0xFF04U)

#define DCM_ROUTINE_START                    (0x01U)
#define DCM_ROUTINE_STOP                     (0x02U)
#define DCM_ROUTINE_RESULTS                  (0x03U)

#define DCM_ROUTINE_STATUS_OK                (0x00U)
#define DCM_ROUTINE_STATUS_FAILED            (0x01U)

/*==================================================================================================
 *                                  下载（0x34 / 0x36 / 0x37）
 *================================================================================================*/
/**
 * @brief 允许下载的目标区域
 * @details 只允许写 App 区。**千万不要允许写 Bootloader 自己所在的区域** ——
 *          Fls 层也有保护区检查，这里是第二道防线。
 */
#define DCM_DOWNLOAD_START_ADDR              (0x00500000UL)  /* FLS_APP_START */
#define DCM_DOWNLOAD_MAX_SIZE                (0x00100000UL)  /* FLS_APP_SIZE  = 1 MB */

/**
 * @brief 单次 TransferData 允许的最大消息长度（含 SID 和序号）
 *
 * @details UDS 里这个值叫 maxNumberOfBlockLength，在 0x34 的正响应里告诉诊断仪。
 *          诊断仪会按这个大小切片，每片用一条 0x36 发过来。
 *
 *          0x0402 = 1026 = 1（SID）+ 1（块序号）+ 1024（数据）。
 *          1024 字节数据需要 CanTp 拼 147 个连续帧，我们的接收缓冲 2048 装得下。
 */
#define DCM_MAX_BLOCK_LENGTH                 (0x0402U)

/** @brief 地址/长度域用几个字节（0x44 = 各 4 字节） */
#define DCM_ADDR_LEN_FORMAT_ID               (0x44U)

/*==================================================================================================
 *                                  缓冲区
 *================================================================================================*/
/** @brief 响应缓冲大小（出站消息都很短） */
#define DCM_TX_BUF_SIZE                      (256U)

/** @brief 发送响应时允许的重试次数（CanTp 忙的时候等一会儿再试） */
#define DCM_TX_RETRY_LIMIT                   (200000UL)

/*==================================================================================================
 *                                  CRC32（0xFF01 例程用）
 *
 *  @details 用最常见的反射型 CRC-32（多项式 0xEDB88320，
 *           初值 0xFFFFFFFF，结果取反）—— **和 Python 的 zlib.crc32 完全一致**。
 *           这样后面写的刷写工具可以直接用 zlib.crc32 和 ECU 对账。
 *================================================================================================*/
#define DCM_CRC32_POLY                       (0xEDB88320UL)
#define DCM_CRC32_INIT                       (0xFFFFFFFFUL)
#define DCM_CRC32_FINAL_XOR                  (0xFFFFFFFFUL)

/*==================================================================================================
 *                                  调试开关
 *================================================================================================*/
#ifndef DCM_DEV_ERROR_DETECT
    #ifdef BSW_DEV_ERROR_DETECT
        #define DCM_DEV_ERROR_DETECT    (BSW_DEV_ERROR_DETECT)
    #else
        #define DCM_DEV_ERROR_DETECT    (STD_ON)
    #endif
#endif

#endif /* DCM_CFG_H */
