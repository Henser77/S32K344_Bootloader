# S32K344 车载 ECU Bootloader（UDS OTA 刷写）

基于 NXP S32K344（Cortex-M7）手写的 Bootloader：通过 **UDS（ISO 14229-1）** 在 **CAN** 上完成应用刷写，
**31 KB 镜像 < 2 秒**（≈20 KB/s，贴着 500 kbps 总线物理极限），断电安全、上电 7 步校验，
并支持"下次上电留在 Bootloader"的门铃机制。

> 个人学习项目。**不包含 NXP RTD 驱动源码**（授权原因，见下文「如何构建」）。

## 特性

| 能力 | 说明 |
|---|---|
| UDS 服务 | `0x10` `0x11` `0x14` `0x19` `0x22` `0x27` `0x2E` `0x31` `0x34` `0x36` `0x37` `0x3E` `0x85` |
| 传输层 | **自行实现** ISO 15765-2：单帧 / 多帧 / 流控 / `N_Bs` `N_Cr` 超时 |
| 刷写速度 | 31 KB 镜像 **< 2 s**（≈20 KB/s） |
| 断电安全 | 32 字节参数账本 + 「擦除前置无效 → 下载 → CRC 通过才置有效 → 复位」，**任意时刻掉电都可重刷** |
| 上电校验 | 7 步：结构 CRC / AppValid / 地址 / 长度 / 向量表 SP / 复位向量 / App 区 CRC |
| 复位 | `0x11 01` 走 **MC_ME**（`MODE_CONF → MODE_UPD → CTL_KEY`，AIRCR 实测无效） |
| 开发/发布版 | 单一开关联动 8 项；发布版小 5.6 KB，ELF 中调试字符串 **0 条** |
| 上位机 | Python + `ctypes` 调 32 位 `ControlCAN.dll`，支持刷写 / 单独擦除 / 单服务自测 / 门铃 |

## 架构（AUTOSAR CP 分层）

```
Application/     启动决策、App 校验（boot_app.c）
Library/         跨模块基础：build_cfg.h（开发/发布开关）、crc32
Services/        CanTp（ISO 15765-2）、Dcm（UDS 服务）
EcuAbstraction/  CanIf（硬件句柄 → PDU 编号翻译）
Mcal/            Can（FlexCAN 邮箱管理）、Uart、Flash（C40 擦写）
generate/ board/ Project_Settings/   CT 生成的硬件配置、启动代码、链接脚本
RTD/             NXP 驱动源码 —— **本仓库不包含**
```

**依赖只允许单向向下**：Application → Services → EcuAbstraction → Mcal。上层不碰寄存器。

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | `main.c`（只做初始化 + 主循环） |
| `Application/` | 上电校验、启动决策、刷写状态机 |
| `Services/` | CanTp / Dcm |
| `EcuAbstraction/` | CanIf |
| `Mcal/` | Can / Uart / Flash 驱动 |
| `Library/` | `build_cfg.h`（开发/发布）、`crc32` |
| `tools/host/` | 上位机刷写工具（`uds_flash.py` + `can_driver.py` + `flash_core.py`） |
| `docs/` | 设计与排错文档 |
| `Project_Settings/` | 链接脚本、启动汇编、调试配置 |

## 如何构建

1. 安装 **S32 Design Studio 3.5**（自带 GCC 10.2 工具链）
2. 在 S32DS 里安装 **RTD 7.0.1 D2602**（Real-Time Drivers，**不随本仓库分发**，需从 NXP 官网获取）
3. `File → Open Projects from File System`，选择本目录导入
4. 按 `docs/` 里的说明补 7 条 include 路径（分层目录各自独立）
5. 选择 `Debug_FLASH` 或 `Release_FLASH` 构建

> 没有 RTD 时工程无法编译 —— 这是有意为之：厂商驱动不进公开仓库。

## 如何刷写

1. 准备 **32 位 Python**（`ControlCAN.dll` 是 32 位的）
2. 接线：USB-CAN 盒 ↔ 板子 CAN0（500 kbps，两端各 120Ω），串口（可选）看日志
3. 刷写：

```bat
python tools\host\uds_flash.py --image out\S32K344_Bootloader.elf -y
```

其它用法：

| 命令 | 作用 |
|---|---|
| `--probe` | 探测 ECU 是否在线（读版本/状态） |
| `--erase-only` | 只擦除 App 区 |
| `--request-boot` | 按"下次上电留在 Bootloader"的门铃（`31 01 FF03`）并复位 |
| `--selftest` | 单服务自测（不发完整镜像） |

## 文档

| 文件 | 内容 |
|---|---|
| `docs/刷写流程与UDS服务.md` | 刷写完整时序 + 每个 UDS 服务的实现细节 |
| `docs/启动代码导读.md` | 启动流程、向量表、链接脚本、内存分区 |
| `docs/测试与排错手册.md` | 实测记录 + 踩过的坑 + 排查方法（最有价值的一份） |
| `docs/上位机界面.png` | 上位机截图 |
| 本 README 后半部分 | **开发记录**：定稿状态、实测数据、已知限制 |

> 如果只想快速判断"这个项目靠不靠谱"，直接看 `docs/测试与排错手册.md` 里的实测表格。

## 已知限制（诚实说明）

- 应用层 Dcm **不实现** `0x34 / 0x36 / 0x37` —— 刷写只在 Bootloader 里做
- `0x27` 安全访问是固定 XOR / ADD 算法，**只用于学习**，不是真正的安全机制
- 发布版为了体积把日志全部编掉，所以板上跑的是哪一版只能看 ELF，不能看串口
- 只支持单调刷写（不支持 A/B 双区回滚）

## 许可与免责

个人学习项目，代码可自由参考。NXP RTD、S32DS 生成代码、`ControlCAN.dll` 的版权归各自所有者。
**请勿直接用于量产** —— 安全访问、刷写原子性、诊断一致性都未达到量产要求。

---

## 定稿状态（2026-10-25，v1.16）

**这个工程到此定稿，不再加功能。**

### 已实测通过（真板子 + 真 CAN 盒）

| 能力 | 结果 |
|---|---|
| UDS 十个服务完整闭环 | ✅ `10 03` → `27` → `10 02` → `31 FF00` → `34` → `36`×N → `37` → `31 FF01` → `11 01` |
| **刷写速度** | ✅ **不到 2 秒**（31 KB，约 20 KB/s，贴着 500 kbps 总线物理极限）|
| 刷写稳定性 | ✅ 全程无卡顿、上位机无超时重试 |
| CRC 校验 | ✅ 本地与 ECU 两侧一致 |
| 上电 7 步校验 + 跳转 | ✅ |
| **`11 01` 真复位** | ✅ 走 MC_ME（`MODE_CONF → MODE_UPD → CTL_KEY`，顺序不能变）|
| 刷写别的 App | ✅ 用 `S32K344_Gateway_UDS_FreeRTOS` 验证过（改链接地址即可）|
| 断电安全 | ✅ 任一时刻掉电都留在 Bootloader |
| 发布版编译 | ✅ 71 个 `.c` 0 警告，比开发版小 5.6 KB，ELF 里中文字符串 0 条 |

> ⚠️ **发布版没有任何串口输出**，连版本号字符串都被编译掉了（这是刻意的）。
> 所以**发布版板上跑的是哪一版，只能从 ELF 文件/构建记录确认**，
> 不像开发版那样串口横幅一眼可见。发布包里建议把版本号写进文件名。

### 已实现、但还没在硬件上验证

| 项 | 说明 |
|---|---|
| 发布版 1000 ms 启动窗口（v1.15）| 上次实测的是窗口=0 的那版（有问题），改回 1000 后**还没烧过** |
| BootRequest 门铃 `0xFF03`/`0xFF04`（v1.16）| 代码完整、两版都能编译，**还没在板子上按过** |

> 这两项要变成"已实测"，各烧一次即可（步骤见下面"定稿前的最后一件事"）。

### 已知限制（明确不做的部分）

| 限制 | 为什么不做 |
|---|---|
| `0x27` 安全访问算法是固定公式 | 真实产品要用 AES-CMAC + HSE 密钥 + 随机种子；这里只把**流程**走通 |
| 采样 App 侧"主动请求刷写"未接入 | 需要 App 也有 Flash 写能力（约 80 行）；目前只有诊断仪能按门铃 |
| 没有启用 SWT 看门狗 | 致命错误时只能"停住"，产品应做成"等看门狗复位" |
| 不支持 CAN FD | 换适配器 + 改传输层，收益是刷写再快 5~10 倍 |
| `can.c` 的 FLTCONF 总线错误检测 | 见 `can.c` 里标注的 TODO；不影响刷写功能 |

### 定稿前的最后一件事（建议做）

```
1. S32DS → Debug_FLASH → Build → 烧录
      串口横幅应看到：S32K344 Bootloader  v1.16
                      构建: DEBUG（日志级别 3）
2. 刷一次 App，确认仍然 < 2 秒且无重试
3. uds_flash.py --wait 30 → 断电重上电 → 进 Bootloader
4. uds_flash.py --request-boot       ← 验证门铃
      应看到 71 01 FF 03 00 和 51 01
5. 等 2 秒，串口应打印：
      账本里有 BootRequest -> 已清掉，留在 Bootloader 等刷写
```

做完第 4、5 步，"BootRequest 门铃"就可以从"未验证"移到"已实测"。

## 项目状态（2026-10-25 实测）

**完整闭环已打通**：PC 通过网络刷固件 → 板子校验 → 跳转 App → App 正常运行。

```
10 03 -> 27 01 -> 27 02 -> 10 02 -> 31 01 FF00
      -> 34 -> 36 x 125 -> 37 -> 31 01 FF01 -> 11 01
```

| 指标 | 实测值 |
|---|---|
| 镜像 | 31888 字节（Demo App）/ 约 77 KB（Gateway App）|
| 传输 | 1024 字节块，约 4587 帧 |
| **耗时** | **不到 2 秒**（约 20 KB/s，已贴着 500 kbps 总线的物理极限）|
| CRC | 本地与 ECU 两侧一致 |
| 跳转 | ✅ App 打印横幅与心跳 |
| 复位 | ✅ `11 01` 走 MC_ME 真复位（v1.13 起）|

> 耗时演进：**55 秒 → 12 秒 → 不到 2 秒**。
> 第一跳是修掉 CanTp 时间基准的并发竞态（副产品），
> 第二跳是去掉上位机里那个"为了绕开该 bug 而加"的 2 ms 帧间隔。

详细日志见 [`docs/测试与排错手册.md`](docs/测试与排错手册.md) 第 9 章「验证记录」。

## 附：设计说明

手写的 CAN / Flash 驱动 + ISO 15765-2 传输层 + ISO 14229-1 诊断服务，
配合官方的 NXP RTD 底层驱动（`C40_Ip`、`FlexCAN`、`Lpuart_Uart_Ip`、`Clock_Ip`、`Port`）。

---

## 一、目录结构（与 `S32K344_Autosar_CanGw` 工程保持同一套约定）

```
src/                           ← 集成层：只有入口
    main.c                        42 行：EcuM_Init() + 超级循环

Application/                   ← 本项目自己的业务逻辑（不属于 AUTOSAR 标准分层）
    boot_param.c / .h             升级标志（参数区账本：magic / AppValid / CRC）
    boot_app.c / .h               App 完整性校验（7 步）+ 跳转（关中断 / VTOR / MSP / PC）
    boot_trace.c / .h             调试输出（帧、UDS 摘要、CAN 计数，可整体关掉）
    boot_version.h                版本号 + 功能清单（构建模式已移到 Library/build_cfg.h）

Library/                       ← 与业务无关的通用算法
    crc32.c / .h                  标准 CRC-32（Dcm 校验和上电校验共用同一份）

Mcal/                          ← MCAL：直接读写 MCU 外设寄存器的驱动
    can.c / can.h                 CAN 控制器（FlexCAN 寄存器）
    can_cfg.c / can_cfg.h         波特率、邮箱、收发硬件对象
    fls.c / fls.h                 Flash 驱动（封装官方 C40_Ip）
    fls_cfg.h                     分区与几何参数
    uart.c / uart.h               UART（LPUART6 寄存器，调试输出）

EcuAbstraction/                ← ECU 抽象层
    canif.c / canif.h             CanIf：CAN ID 过滤、PDU 编号 ↔ 硬件句柄
    canif_cfg.h                   CAN ID / PDU 编号 / 发送硬件对象

Services/
    Communication/             ← 通信服务
        cantp.c / cantp.h         CanTp：I-PDU ↔ N-PDU 拆装（ISO 15765-2）
        cantp_cfg.h               缓冲大小、超时、填充字节
        pdur.c / pdur.h           PduR：唯一知道"哪条 PDU 走哪条路"的地方
        pdur_cfg.h                路由表
    Diagnostic/
        dcm.c / dcm.h             Dcm：UDS 服务解析（ISO 14229-1）
        dcm_cfg.h                 服务号、NRC、超时、下载地址范围
    System/
        ecum.c / ecum.h           EcuM：初始化顺序 + 主循环调度 + 复位 + 启动窗口

board/                         ← CT 生成的引脚/端口配置
generate/                      ← CT 生成的模块配置（含 C40_Ip_Cfg.h 的 4 个开关）
RTD/                           ← NXP RTD 驱动源码（官方，不修改）
Project_Settings/              ← 启动代码、链接脚本
docs/                          ← 测试与排错手册
```

### src / Application / Library 三者的区别

| 目录 | 放什么 | 判断标准 |
|---|---|---|
| `src/` | 入口 | **只有 `main()`** —— 负责装配，自己不含业务逻辑 |
| `Application/` | 本 Bootloader 专有的逻辑 | **换一颗 MCU 就全部要重写**的东西（参数区格式、校验策略、跳转）|
| `Library/` | 通用算法 | **与项目无关**，任何工程都能直接用（CRC32）|

> **为什么 `Application/` 不并入 `Services/`**
> `Services/` 里的模块都**符合 AUTOSAR 标准**（CanTp / Dcm / PduR / EcuM），
> 有标准 API 名和模块 ID。而 `boot_param` / `boot_app` 是**本项目自己发明**的，
> 硬塞进 `Services/` 会让"这个工程到底实现了多少 AUTOSAR"变得含糊。

### 为什么这样分层

| 层 | 判断标准 | 本工程的模块 |
|---|---|---|
| **MCAL** | **直接读写 MCU 外设寄存器** | `Can`、`Fls`、`Uart` |
| **ECU 抽象层** | 把"PDU 编号"翻译成"硬件句柄 + CAN ID" | `CanIf` |
| **服务层 — 通信** | 通信协议逻辑，不碰寄存器 | `CanTp`、`PduR` |
| **服务层 — 诊断** | 诊断协议逻辑 | `Dcm` |
| **服务层 — 系统** | 启动顺序、运行期调度 | `EcuM` |

**判断一个模块该放哪层，只看一件事：它有没有直接碰寄存器。**

- `Fls` 直接写 `MCR` 的 `ERS`/`PGM`/`EHV` 位 → **MCAL**
- `CanTp` 只做"长报文 ↔ 8 字节帧"的拆装 → **服务层**
- `Dcm` 只解析服务号/子功能/NRC → **服务层**
- `CanIf` 做"PDU 编号 ↔ 硬件句柄/CAN ID"的映射 → **ECU 抽象层**

### 各层之间的依赖只允许单向

```
Dcm ──┐
      ├──► PduR ──► CanTp ──► CanIf ──► Can ──► FlexCAN
CanTp ┘                （PduR 同时服务 Dcm 和 CanTp 两个方向）
```

具体表现：

- `cantp.c` **不包含** `can.h` —— 它不知道 CAN ID，也不知道邮箱编号，
  只把 8 字节 PDU 交给 `PduR_CanTpTransmit()`。
  换 CAN FD 或换 MCU，`cantp.c` 一行都不用改。
- `dcm.c` **不包含** `cantp.h` —— 它不知道响应要分几帧发，
  只调 `PduR_DcmTransmit()`。
- `canif.c` 是唯一知道 `0x7E0` / `0x7E8` 的模块（配置在 `canif_cfg.h`）。

---

## 二、数据流

```
诊断仪 (0x7E0)
    ↓ CAN 帧
FlexCAN ──中断──► can.c (Can_IsrBridge)
                    ↓
                 CanIf_RxIndication      按 CAN ID 过滤 → 转 PduInfoType
                    ↓
                 PduR_CanIfRxIndication  路由
                    ↓
                 CanTp_RxIndication      拼 SF/FF/CF、发流控帧
                    ↓ （拼好的完整消息）
                 PduR_MainFunction       取消息 → 交给 Dcm
                    ↓
                 Dcm_RxIndication        解析 UDS 服务
                    ↓
              Fls_Erase / Fls_Write / Fls_Read
                    ↓
                 C40_Ip（官方驱动）
                    ↓
                Flash 控制器
```

响应方向：`Dcm → PduR_DcmTransmit → CanTp_Transmit（拆帧）→ PduR_CanTpTransmit → CanIf_Transmit → Can_Write → 发送中断 → CanIf_TxConfirmation → PduR → CanTp_TxConfirmation`。

---

## 三、Flash 内存分区

```
0x00400000 ~ 0x004FFFFF   BLOCK_0   Bootloader（代码所在地，擦写禁区）
0x00500000 ~ 0x005FFFFF   BLOCK_1   App 区（刷写目标）
0x00600000 ~ 0x00601FFF   BLOCK_2   升级标志 / 期望 CRC
0x00602000 ~ 0x00603FFF   BLOCK_2   Flash 测试扇区（可以被随便擦写）
0x00700000 ~ 0x007CDFFF   BLOCK_3   可用
0x007CE000 ~ ...          HSE 保留区（禁止触碰）
```

**RWW（Read-While-Write）只在跨 Block 时有效** —— Bootloader 在 BLOCK_0、App 在 BLOCK_1，
擦写 App 区时 CPU 仍能从 BLOCK_0 取指，所以不需要把擦写代码搬到 RAM 里执行。

---

## 四、CT 里必须打开的配置（缺一不可）

```
C40_Ip → MemGeneral：
  ☑ Mem ECC Enable Check     → C40_IP_ECC_CHECK
  ☑ Erase Verification       → C40_IP_ERASE_VERIFICATION_ENABLED
  ☑ Program Verification     → C40_IP_PROGRAM_VERIFICATION_ENABLED
  ☑ Synchronize Cache        → C40_IP_SYNCRONIZE_CACHE

Cache Ip 组件：
  ☑ 启用                      → CACHE_IP_IS_AVAILABLE
```

**为什么这四项缺一不可**

本工程同时满足两件事：D-Cache 是开着的，且 MPU 把 Program Flash 配成
`Write-Back/Allocate`（**可缓存**）。所以 CPU 读到的 Flash 内容是**缓存副本**，
Flash 被擦写后 CPU 并不知道。

| 开关 | 管哪一层缓冲 |
|---|---|
| `Mem ECC Enable Check` | Flash 控制器内部的**预取缓冲** |
| `Synchronize Cache` + `Cache Ip` | CPU 的 **D-Cache** |
| `Erase Verification` | 擦除结果**是否可信**（驱动自己查空）|
| `Program Verification` | 写入结果**是否可信**（驱动自己校验）|

---

## 五、编译注意：需要手动加的 include 路径

工程有 **7 个**自定义 include 路径（从零重建工程时需要手动加）：

```
Project Properties → C/C++ Build → Settings → Tool Settings
    → 编译器 → Includes → Include paths：

        ${ProjDirPath}/src                  ← main.c 所在（也是下面两个的头文件目录）
        ${ProjDirPath}/Application          ← boot_param / boot_app / boot_trace / boot_version
        ${ProjDirPath}/Library              ← crc32
        ${ProjDirPath}/EcuAbstraction       ← canif
        ${ProjDirPath}/Services/Communication   ← cantp / pdur
        ${ProjDirPath}/Services/Diagnostic      ← dcm
        ${ProjDirPath}/Services/System          ← ecum
```

并且这三个目录都要在 **C/C++ General → Paths and Symbols → Source Location** 里
被标记为源文件夹（否则里面的 `.c` 不会被编译）：

```
src/   Application/   Library/
```

（`Mcal`、`generate/include`、`RTD/include`、`board` 原本就在列表里。）

---

## 六、升级与启动流程（Bootloader 的核心）

### 6.1 参数区（升级标志）

地址 `0x00600000`，独占一个 8 KB 扇区。结构见 `Application/boot_param.h`：

| 字段 | 作用 |
|---|---|
| `Magic` | 魔数 `"SK3B"`，区分"初始化过"和"空白 Flash" |
| **`AppValid`** | **1 = App 完整可跳转；0 = 无效** |
| `AppStart` | App 起始地址（`0x00500000`）|
| `AppSize` | App 有效字节数（算 CRC 用）|
| `AppCrc32` | App 区的 CRC32 |
| `StructCrc32` | 结构自身的 CRC，防止账本被写坏 |

### 6.2 ★ 断电安全顺序（最重要的一条纪律）

```
刷写阶段                      AppValid        上电结果
──────────────────────────────────────────────────────────
0x31 FF00 擦除 App 区之前      置 0            → 留在 Bootloader
擦除 / 0x36 写入 / 校验中       保持 0          → 留在 Bootloader
0x31 FF01 CRC 一致（验货通过）  置 1 + CRC/Size → 跳 App
```

**顺序反了会怎样**：先写"有效"再擦除 → 刷到一半断电 → 账本说"有效" →
跳进半新半旧的 App → 跑飞，而且诊断仪再也连不上（因为没停留在 Bootloader）。

### 6.3 上电 7 步校验（`Application/boot_app.c`）

```
1. 账本结构有效？（魔数 + 结构 CRC）
2. 账本说 App 有效？
3. AppStart == 0x00500000？
4. AppSize 在 1 ~ 1MB 内？
5. 向量表[0] 的 SP 落在 RAM 内（SRAM / DTCM）且 8 字节对齐？
6. 向量表[1] 的 Reset 向量在 App 区内且 bit0=1（Thumb）？
7. App 区 CRC32 与账本一致？
        ↓ 全部通过
      EcuM 停掉 CAN 控制器 → BootApp_Jump() → 关中断 / 设 VTOR / 换 MSP / 跳
```

**任何一步失败都留在 Bootloader** —— 这永远比"冒险跳一次"划算。

### 6.4 刷写服务的完整顺序

| 服务 | 作用 | 关键行为 |
|---|---|---|
| `10 02` | 进编程会话 | 必须 |
| `27 01/02` | 安全解锁 | 必须 |
| `31 01 FF00` | 擦除 App 区 | **先置 `AppValid=0`，再擦** |
| `34` | 请求下载 | 校验地址/长度 |
| `36` | 传输数据 | 逐块写 Flash + 累加 CRC |
| `37` | 结束传输 | |
| `31 01 FF01` | **CRC 校验** | **一致时写 `AppValid=1` + Size + CRC** |
| `11 01` | 复位 | 复位后自动校验并跳 App |

### 6.5 调试例程 `0xFF02`：信任当前 App 区（**发布必须关**）

用调试器把 App 直接烧进 `0x00500000` 时，参数区还是空的，上电不会跳。
这个例程"事后认账"：

```
请求：31 01 FF 02 <长度 4 字节大端>
例：  31 01 FF 02 00 00 80 00        → 认账 32KB

响应：71 01 FF 02 <状态> <CRC 4 字节>
```

`dcm_cfg.h` 里的 `DCM_ENABLE_TRUST_APP`（默认 `STD_ON`）。
**正式发布改成 `STD_OFF`。**

---

## 七、开发版 / 发布版

### 7.1 只有一个开关

`Library/build_cfg.h` 里改一行（或者用 IDE 的构建配置传 `-D`，见 7.2）：

```c
#define BOOT_BUILD_MODE     BOOT_BUILD_DEBUG     /* 改成 BOOT_BUILD_RELEASE */
```

**改这一行，下面 8 项全部自动切换**（唯一派生点，别处不许再单独定义）：

| 派生的开关 | 开发版 | 发布版 | 关掉后省了什么 |
|---|---|---|---|
| `BOOT_LOG_LEVEL` | `VERBOSE` | **`NONE`** | `Uart_Printf`/`Uart_SendString` 整体编译掉 |
| `BSW_DEV_ERROR_DETECT` | `STD_ON` | **`STD_OFF`** | Can/Fls/Uart/CanTp/Dcm 的 DET 一起关 |
| `BOOT_TRACE_ENABLE` | `STD_ON` | **`STD_OFF`** | 调试追踪模块（约 430 行）整体编译掉 |
| `BOOT_TRACE_SHOW_KEY` | `STD_ON` | **`STD_OFF`** | 不打印"应回密钥"（等于不公开解锁方法）|
| `DCM_ENABLE_TRUST_APP` | `STD_ON` | **`STD_OFF`** | 关掉 `0xFF02`（不验货就签收 App 的后门）|
| `CANTP_MANUAL_TEST_MODE` | `STD_ON`（60 s）| **`STD_OFF`**（1 s）| 掉线后能快速脱身 |
| `DCM_MANUAL_TEST_MODE` | `STD_ON`（120 s）| **`STD_OFF`**（5 s）| 会话不会挂太久 |
| `BOOT_HOLD_WINDOW_MS` | `1500` | **`1000`** | 窗口短 1/3（**不能设成 0，见 7.2**）|

### 7.2 ⚠️ 发布版必须保留"进 Bootloader 的路"（实测踩过的坑）

2026-10-25 把发布版的 `BOOT_HOLD_WINDOW_MS` 设成 `0`（理由："上电直接跳，启动最快"），
烧进去之后的现象：

- 上位机 `--wait` 连不上
- **断电重上电还是连不上**
- 看起来"Bootloader 没了"

**原因**：App 校验通过时 `EcuM_Init` 会调 `EcuM_WaitBootHoldWindow()`，
窗口为 0 时它立刻返回 `FALSE` → 直接 `BootApp_Jump()`。
**开机 0 毫秒就跳走，Bootloader 的 UDS 从来没机会响应** —— 板子其实在正常跑 App。

**结论**：这个窗口是"软件进入 Bootloader"的**唯一入口**。
把它关掉 = 一旦 App 刷坏或需要重刷，只能上 J-Link 救砖。

**✅ v1.16 已经把这个机制做出来了**（BootRequest 门铃）：

| 谁来按门铃 | 怎么做 |
|---|---|
| **诊断仪** | 编程会话+解锁后发 `31 01 FF 03` → 写账本 `BootRequest = 1`；再 `11 01` 复位 |
| **上位机** | `uds_flash.py --request-boot`（一条命令完成置位 + 复位）|
| **取消** | `31 01 FF 04` 或 `uds_flash.py --clear-boot` |
| **App** | 调用 `BootParam_SetBootRequest()` 后复位（需 App 能写 Flash）|

Bootloader 每次上电检查这个字段：**置位就清掉并留在 Bootloader**（只消费一次）。

所以现在有两条入口：
1. 启动窗口（1 秒，兜底 —— 不依赖 App）
2. BootRequest 门铃（App 或诊断仪主动按）

> 等 App 侧也接了门铃，就可以把发布版窗口设成 0，做到"上电秒跳 + 永远刷得回来"。

> **正确做法（产品级）**：让 App 能主动请求进 Bootloader ——
> App 收到 `10 02` 或某个自定义例程时，往参数区的 `BootRequest` 字段写 1 并复位；
> Bootloader 启动先看这个字段：置位就清掉并留下，未置位就直接跳（完全不需要窗口）。
> 这样既"上电秒跳"又永远刷得回来。
> 本工程目前只有窗口这一条路，所以发布版保留 1000 ms。

### 7.3 推荐做法：用 IDE 的两个构建配置，不改源码

S32DS 自动给每个工程生成四个构建配置：

| 配置 | 代码跑在哪 | 用途 |
|---|---|---|
| `Debug_FLASH` | P-Flash | **日常开发 + 烧录**（一直用这个）|
| `Debug_RAM` | 内存（掉电即失）| 需要极快执行 / Flash 被擦时用来救场 |
| `Release_FLASH` | P-Flash | **发布版**（编译参数目前和 Debug 相同，只是名字）|
| `Release_RAM` | 内存 | 同上，RAM 运行版 |

> 实测：这四个配置的编译参数**完全一样**（都是 `-Os`，同样的 `-D`），
> 真正的差别只有链接脚本（`linker_flash` / `linker_ram`）。

**给 `Release_FLASH` 加一个宏定义，就变成发布版**（源码一行都不用动）：

```
工程右键 → Properties → C/C++ Build → Settings
  → Tool Settings → Cross ARM C Compiler → Preprocessor
  → Defined symbols (-D) 里加：    BOOT_BUILD_MODE=1
```

这样：
- `Debug_FLASH` 编译出来 = 开发版（DET + 全部打印）
- `Release_FLASH` 编译出来 = 发布版（什么都没有）
- 不会出现"改了忘了改回来"

### 7.4 实测效果（2026-10-25）

| | text | bss | 合计 |
|---|---|---|---|
| 开发版 | 79976 | 25556 | 105536 |
| 发布版 | **74288** | 25380 | **99672** |
| 差值 | **−5688（5.6 KB，7.1%）** | −176 | −5864 |

**更强的证据**：发布版 ELF 里**中文字符串 = 0 条**（开发版 71 条）。
说明所有调试文字真的从固件里消失了，不是"存在但不执行"。

两版都是 **71 个 `.c`、0 警告**。

### 7.5 发布版里的"致命错误"怎么办

`EcuM_Fatal()` 在开发版是"打印 + 死循环"，一句话就能看懂原因。

发布版没有串口，所以改成**不打印、直接停住**：

```c
#if (UART_PRINTING_DISABLED == STD_OFF)
    Uart_Printf("\n[致命错误] %s\n", Reason);
#else
    (void)Reason;      /* 不打印，也不刷屏 */
#endif
    for (;;) { }
```

**为什么是"停住"而不是"复位"**：复位会无限重启，如果故障是持续性的（比如时钟配错），
串口就变成一屏滚动的横幅，反而看不到线索。停住至少让状态是静止的。

**真实产品应该做成**：关掉 CAN 控制器（别在总线上发垃圾帧）+ 等看门狗复位 +
通过 DTC / LED 报告。本工程没有启用 SWT 看门狗，所以只做到"停住"。

### 7.6 已知的安全缺口（写在这里免得误以为它安全）

`0x27` 安全访问的算法是 `key = (seed ^ 0x5A5A5A5A) + 0x12345678`：

- 公式固定且公开，拿到一组 `(seed,key)` 就能反推
- 种子用计数器生成，每次上电序列一样，可预测

**真实产品要用 AES-128-CMAC + HSE 密钥区 + 硬件随机种子 + SecOC。**
本工程只把**流程**走通（含 NRC 0x36/0x37 的失败锁定），不提供实际安全性。

### 7.7 上位机不在本仓库里

它是 PC 端工具，和 ECU 固件是两类东西，放在一起会让"这个仓库是什么"变得含糊。
它在 `D:\tools\`，单独维护：

    D:\tools\上位机.bat          图形界面
    D:\tools\uds_flash.py        命令行
    D:\tools\python32_full\      自带的 32 位 Python（ControlCAN.dll 是 32 位的）

---

## 八、文档

- [`docs/测试与排错手册.md`](docs/测试与排错手册.md)
  手工测试十步（含每一帧的字节和期望响应）、NRC 错误码对照表、
  以及本次排查 Flash 驱动时踩过的坑和最终结论。

- [`docs/启动代码导读.md`](docs/启动代码导读.md)
  **`src/main.c` + `Application/` + `Library/` + `ecum.c` 的通读导读。**
  上电时间线、逐文件精读（附行号）、三个关键设计点、常见误解澄清、
  动手验证清单、面试自测 8 问。

- [`docs/刷写流程与UDS服务.md`](docs/刷写流程与UDS服务.md)
  **一次完整刷写按顺序涉及的 10 个 UDS 服务。**
  每步的请求/响应逐字节解释、前置条件、常见 NRC、
  断电安全分析、实测日志、为什么顺序不能变。
