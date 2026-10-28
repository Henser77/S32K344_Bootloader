#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flash_core.py —— 刷写协议核心（命令行和图形界面共用这一份）

【为什么要单独一个模块】
同一套 ISO-TP + UDS 逻辑，如果命令行一份、界面再抄一份，
将来改协议就得改两处，必然漏。所以：

    flash_core.py   协议核心，不打印、不弹窗，只通过回调报告进度
    uds_flash.py    命令行前端（把回调接到 print）
    flash_gui.py    图形界面前端（把回调接到控件）

这样"怎么刷"只有一份实现，"怎么显示"各自负责。
"""

import struct
import time
import zlib

from can_driver import CanDevice

# ===========================================================================
# 与本工程 Bootloader 严格对齐的常量
#   来源：S32K344_Bootloader 的 dcm_cfg.h / cantp_cfg.h / can_cfg.c
#   改 Bootloader 配置时，这里必须同步改
# ===========================================================================

CAN_TX_ID       = 0x7E0        # 诊断仪 -> ECU
CAN_RX_ID       = 0x7E8        # ECU -> 诊断仪
ISOTP_PAD       = 0xCC         # 填充字节（CANTP_PADDING_BYTE）

# 安全访问：key = ((seed ^ 0x5A5A5A5A) + 0x12345678) & 0xFFFFFFFF
SEC_XOR         = 0x5A5A5A5A   # DCM_SEC_KEY_XOR
SEC_ADD         = 0x12345678   # DCM_SEC_KEY_ADD

ALFID           = 0x44         # DCM_ADDR_LEN_FORMAT_ID
APP_START       = 0x00500000   # DCM_DOWNLOAD_START_ADDR
APP_MAX_SIZE    = 0x00100000   # DCM_DOWNLOAD_MAX_SIZE

# 0x36 每块最多 1024 字节（0x34 响应 maxBlockLength=0x0402，减 2 字节 SID+BSC）
BLOCK_SIZE      = 1024

# 实际使用的传输块大小。
#
# 【为什么不用最大值 1024】
# 每块要拆成 1 个首帧 + 147 个连续帧。连续帧越多，中间丢一帧的概率越大 ——
# 而 ECU 侧 CanTp 一旦发现序号错就会**直接中止**，且不回负响应，
# 上位机这边只能看到"等响应超时"，非常难查。
#
# 实测：1024 字节的块在第 2 块、第 12 块各失败一次（偶发丢帧）。
# 改用 256 字节（每块只有 37 帧）后成功率明显提高；再配合失败重试就基本无忧。
TRANSFER_BLOCK_DEFAULT = 1024

RID_ERASE       = 0xFF00
RID_CHECK       = 0xFF01
RID_TRUST       = 0xFF02       # 调试用：信任当前 App 区

NRC_NAMES = {
    0x10: "generalReject 一般拒绝",
    0x11: "serviceNotSupported 服务不支持",
    0x12: "subFunctionNotSupported 子功能不支持",
    0x13: "incorrectMessageLengthOrInvalidFormat 长度/格式错",
    0x21: "busyRepeatRequest 忙，请重发",
    0x22: "conditionsNotCorrect 条件不满足",
    0x24: "requestSequenceError 请求顺序错",
    0x31: "requestOutOfRange 请求超范围",
    0x33: "securityAccessDenied 安全访问被拒",
    0x35: "invalidKey 密钥错误",
    0x36: "exceedNumberOfAttempts 尝试次数超限",
    0x37: "requiredTimeDelayNotExpired 延时未到",
    0x72: "generalProgrammingFailure 编程失败（Flash 擦写失败）",
    0x78: "responsePending 请等待",
    0x7E: "subFunctionNotSupportedInActiveSession 当前会话不支持该子功能",
    0x7F: "serviceNotSupportedInActiveSession 当前会话不支持该服务",
}


class FlashError(Exception):
    pass


class UdsTimeout(FlashError):
    pass


class FlashCancelled(FlashError):
    pass


class UdsNegativeResponse(FlashError):
    def __init__(self, sid, nrc, raw):
        self.sid = sid
        self.nrc = nrc
        self.raw = raw
        name = NRC_NAMES.get(nrc, "未知负响应码")
        FlashError.__init__(self, "服务 0x%02X 被拒绝：NRC 0x%02X (%s)"
                            % (sid, nrc, name))


# ===========================================================================
# 一、ISO-TP（ISO 15765-2）
# ===========================================================================

class IsoTpChannel(object):
    """
    ISO-TP 收发。

    【两个方向的多帧规则不一样，别搞混】
        ECU -> 诊断仪（响应有多帧）：诊断仪是接收方，**诊断仪发流控帧**
        诊断仪 -> ECU（请求有多帧）：ECU 是接收方，**ECU 发流控帧**
    """

    def __init__(self, dev, tx_id=CAN_TX_ID, rx_id=CAN_RX_ID,
                 pad=ISOTP_PAD, trace=None, cf_delay_ms=0.0):
        self.dev = dev
        self.tx_id = tx_id
        self.rx_id = rx_id
        self.pad = pad
        self.trace = trace          # 可选的原始帧回调（界面用来显示每帧）
        # ------------------------------------------------------------------
        # 连续帧之间强制插入的最小间隔（毫秒）
        #
        # 【为什么必须有】
        # ECU 侧只有 1 个接收邮箱。连发时如果两帧在邮箱重新挂载之前到达，
        # 后一帧会被覆盖 —— 表现为 CanTp 的"序号错"然后整条消息中止，
        # 上位机这边就只看到"等响应超时"。
        #
        # 理论帧间隔：500 kbps 下 8 字节帧约 230 µs，USB 适配器会成串发，
        # 所以必须人为放慢。1 ms 已经很保守，32 块总共多花约 5 秒。
        # ------------------------------------------------------------------
        self.cf_delay_ms = cf_delay_ms
        self.stat_tx_frames = 0
        self.stat_rx_frames = 0
        self.cancel_flag = None     # 可选的 threading.Event

    def _canceled(self):
        return self.cancel_flag is not None and self.cancel_flag.is_set()

    def _trace(self, direction, data):
        if self.trace is not None:
            self.trace(direction, data)

    def _pad_frame(self, frame):
        if len(frame) < 8:
            frame = frame + bytes([self.pad]) * (8 - len(frame))
        return frame[:8]

    def _send_fc(self, bs=0x00, stmin=0x00):
        frame = self._pad_frame(bytes([0x30, bs, stmin]))
        self.dev.send(self.tx_id, frame)
        self.stat_tx_frames += 1
        self._trace(">>", frame)

    def _wait_fc(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._canceled():
                raise FlashCancelled("用户取消")
            for (cid, data, _ts) in self.dev.recv(10):
                if cid != self.rx_id or len(data) < 3:
                    continue
                self.stat_rx_frames += 1
                self._trace("<<", data)
                if (data[0] >> 4) == 0x3:
                    return (data[0] & 0x0F, data[1], data[2])
        raise UdsTimeout("等 ECU 流控帧超时（N_Bs 超时）")

    def send(self, payload):
        n = len(payload)
        if n == 0:
            raise ValueError("空消息")
        if n > 2048:
            raise ValueError("消息太长（%d > 2048，超出 CanTp 接收缓冲）" % n)

        if n <= 7:
            frame = self._pad_frame(bytes([n]) + payload)
            if not self.dev.send(self.tx_id, frame):
                raise FlashError("CAN 发送失败")
            self.stat_tx_frames += 1
            self._trace(">>", frame)
            return

        # 首帧
        frame = bytes([0x10 | ((n >> 8) & 0x0F), n & 0xFF]) + payload[:6]
        if not self.dev.send(self.tx_id, frame):
            raise FlashError("CAN 发送失败")
        self.stat_tx_frames += 1
        self._trace(">>", frame)

        offset = 6
        sn = 1
        while offset < n:
            if self._canceled():
                raise FlashCancelled("用户取消")
            fs, bs, stmin = self._wait_fc()
            if fs == 0x01:
                continue
            if fs == 0x02:
                raise FlashError("ECU 回复流控溢出（FS=2），它装不下这条消息")
            if fs != 0x00:
                raise FlashError("ECU 回了未定义的流控状态 FS=%d" % fs)

            max_frames = 0xFFFFFFFF if bs == 0 else bs
            cnt = 0
            while offset < n and cnt < max_frames:
                chunk = payload[offset:offset + 7]
                frame = self._pad_frame(bytes([0x20 | (sn & 0x0F)]) + chunk)
                if not self.dev.send(self.tx_id, frame):
                    raise FlashError("CAN 发送失败")
                self.stat_tx_frames += 1
                self._trace(">>", frame)
                offset += len(chunk)
                sn = (sn + 1) & 0x0F
                cnt += 1

                # 取"对端要求的 STmin"和"我们自己设的保底间隔"里的较大者
                delay_ms = max(stmin, self.cf_delay_ms)
                if delay_ms > 0.0:
                    # 【为什么不直接用 time.sleep】
                    #   Windows 上 time.sleep 的精度只有 1 毫秒左右（甚至 15.6 ms），
                    #   想做到 0.5 ms 这种级别必须忙等。
                    #   策略：>= 2 ms 先粗睡（省 CPU），最后 1 ms 用忙等校准。
                    target = time.perf_counter() + (delay_ms / 1000.0)
                    if delay_ms >= 2.0:
                        time.sleep((delay_ms - 1.0) / 1000.0)
                    while time.perf_counter() < target:
                        pass

    def recv(self, timeout=1.0):
        """收一条完整的 UDS 消息，超时返回 None"""
        deadline = time.time() + timeout
        buf = None
        total = 0
        sn = 1

        while time.time() < deadline:
            if self._canceled():
                raise FlashCancelled("用户取消")
            for (cid, data, _ts) in self.dev.recv(10):
                if cid != self.rx_id or not data:
                    continue
                self.stat_rx_frames += 1
                self._trace("<<", data)

                pci = data[0] >> 4

                if pci == 0x0:
                    ln = data[0] & 0x0F
                    if ln == 0 or ln > 7:
                        continue
                    return bytes(data[1:1 + ln])

                if pci == 0x1:
                    total = ((data[0] & 0x0F) << 8) | data[1]
                    if total < 8 or total > 4095:
                        continue
                    buf = bytearray(data[2:8])
                    sn = 1
                    # ★ 诊断仪是接收方 → 必须自己发流控帧
                    self._send_fc()
                    continue

                if pci == 0x2:
                    if buf is None:
                        continue
                    if (data[0] & 0x0F) != sn:
                        buf = None
                        continue
                    sn = (sn + 1) & 0x0F
                    need = total - len(buf)
                    buf += data[1:1 + min(7, need)]
                    if len(buf) >= total:
                        return bytes(buf[:total])
                    continue

                # pci == 0x3：流控帧，接收方向不该出现，忽略
        return None


# ===========================================================================
# 二、UDS 客户端
# ===========================================================================

class UdsClient(object):
    def __init__(self, tp, log=None):
        self.tp = tp
        self.log = log or (lambda s: None)

    @staticmethod
    def _brief(data, limit=24):
        """日志用的短格式：超过 limit 字节只打前 limit 个 + 总长度"""
        if len(data) <= limit:
            return data.hex(" ").upper()
        return "%s ... （共 %d 字节）" % (data[:limit].hex(" ").upper(), len(data))

    def request(self, payload, timeout=1.0, pending_timeout=20.0, label=""):
        sid = payload[0]
        self.log("-> %s  %s" % (label or "", self._brief(payload)))

        self.tp.send(payload)

        deadline = time.time() + timeout
        pending_deadline = time.time() + pending_timeout

        while True:
            wait = max(0.0, min(deadline, pending_deadline) - time.time())
            resp = self.tp.recv(wait)
            if resp is None:
                raise UdsTimeout("等响应超时（服务 0x%02X）" % sid)

            self.log("<- %s" % self._brief(resp))

            if len(resp) >= 3 and resp[0] == 0x7F:
                if resp[1] != sid:
                    continue
                if resp[2] == 0x78:
                    deadline = time.time() + pending_timeout
                    self.log("   （ECU 回 0x78 请等待，继续等...）")
                    continue
                raise UdsNegativeResponse(sid, resp[2], resp)

            if resp[0] != ((sid + 0x40) & 0xFF):
                continue

            return resp

    def expect(self, resp, prefix, what):
        if not resp.startswith(prefix):
            raise FlashError("%s：响应不符，期望 %s 开头，实际 %s"
                             % (what, prefix.hex(" ").upper(),
                                resp.hex(" ").upper()))
        return resp


def key_from_seed(seed):
    """与 Bootloader 的 Dcm_CalcKey() 完全一致"""
    return ((seed ^ SEC_XOR) + SEC_ADD) & 0xFFFFFFFF


# ===========================================================================
# 三、镜像加载
# ===========================================================================

def load_image(path, base=APP_START, size_override=None, log=None):
    """
    读入镜像。.elf 走 PT_LOAD 段解析，.bin 直接当原始数据。
    返回 (image_bytes, base)
    """
    log = log or (lambda s: None)

    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        log("镜像来源 : %s（原始二进制，%d 字节）" % (path, len(data)))
        return finish_image(bytearray(data), base, size_override, log)

    if data[4] != 1:
        raise FlashError("只支持 32 位 ELF")
    if data[5] != 1:
        raise FlashError("只支持小端 ELF")

    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]

    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_offset, p_vaddr, p_paddr,
         p_filesz, _memsz, _flags, _align) = struct.unpack_from("<IIIIIIII", data, off)
        if p_type != 1 or p_filesz == 0:
            continue
        if not (base <= p_paddr < base + APP_MAX_SIZE):
            continue
        segs.append((p_paddr, p_offset, p_filesz))

    if not segs:
        raise FlashError("ELF 里没有落在 0x%08X 起的段 —— 这个工程链接到 App 区了吗？"
                         % base)

    end = max(a + n for (a, _o, n) in segs)
    img = bytearray(b"\xFF" * (end - base))
    for (paddr, poff, psize) in segs:
        img[paddr - base: paddr - base + psize] = data[poff: poff + psize]

    log("镜像来源 : %s（ELF，%d 个段）" % (path, len(segs)))
    for (paddr, _o, n) in segs:
        log("           段 0x%08X ~ 0x%08X  (%d 字节)" % (paddr, paddr + n - 1, n))
    return finish_image(img, base, size_override, log)


def finish_image(img, base, size_override, log=None):
    log = log or (lambda s: None)

    if size_override:
        if size_override > len(img):
            img = img + bytearray(b"\xFF" * (size_override - len(img)))
        else:
            img = img[:size_override]

    if len(img) % 8:
        img = img + bytearray(b"\xFF" * (8 - len(img) % 8))

    if len(img) > APP_MAX_SIZE:
        raise FlashError("镜像 %d 字节，超过 App 区上限 %d" % (len(img), APP_MAX_SIZE))

    crc = zlib.crc32(bytes(img)) & 0xFFFFFFFF
    log("起始地址 : 0x%08X" % base)
    log("镜像长度 : %d 字节（0x%X）" % (len(img), len(img)))
    log("本地 CRC32: 0x%08X" % crc)
    return bytes(img), base


# ===========================================================================
# 四、刷写流程
# ===========================================================================

def flash(uds, image, base, do_erase=True, verify=True, do_reset=True,
          log=None, on_progress=None, on_step=None, cancel_flag=None,
          block_size=TRANSFER_BLOCK_DEFAULT, retries=3):
    """
    完整刷写流程。每一步失败都抛异常。

    log(msg)                    文本日志
    on_step(idx, total, text)   步骤提示（界面用来更新"当前步骤"）
    on_progress(done, total)    块进度
    cancel_flag                 threading.Event，置位后尽快中止
    """
    log = log or (lambda s: None)
    on_step = on_step or (lambda i, t, s: None)
    on_progress = on_progress or (lambda d, t: None)

    def check_cancel():
        if cancel_flag is not None and cancel_flag.is_set():
            raise FlashCancelled("用户取消")

    total = len(image)
    t_start = time.time()

    # 1 扩展会话
    on_step(1, 8, "进入扩展会话")
    r = uds.request(bytes([0x10, 0x03]), label="10 03")
    uds.expect(r, bytes([0x50, 0x03]), "切换会话")
    check_cancel()

    # 2 请求种子
    on_step(2, 8, "请求安全种子")
    r = uds.request(bytes([0x27, 0x01]), label="27 01")
    uds.expect(r, bytes([0x67, 0x01]), "请求种子")
    if len(r) < 6:
        raise FlashError("种子响应太短：%s" % r.hex(" "))
    seed = struct.unpack(">I", r[2:6])[0]
    log("   种子 = 0x%08X" % seed)

    # 3 发送密钥
    #
    # 【种子为 0 是什么意思】
    # ISO 14229-1 规定：如果 ECU 当前**已经解锁**，请求种子时返回全 0 种子。
    # 这时不需要再发密钥（发了反而会被回 NRC 0x24 请求顺序错）。
    # 中断过一次刷写、或 S3 还没超时的时候很容易遇到，必须处理。
    if seed == 0:
        on_step(3, 8, "已解锁，跳过密钥")
        log("   种子为 0 —— ECU 已经处于解锁状态，跳过发送密钥")
    else:
        on_step(3, 8, "发送密钥")
        key = key_from_seed(seed)
        log("   密钥 = 0x%08X" % key)
        try:
            r = uds.request(bytes([0x27, 0x02]) + struct.pack(">I", key),
                            label="27 02")
            uds.expect(r, bytes([0x67, 0x02]), "发送密钥")
        except UdsNegativeResponse as e:
            if e.nrc == 0x24:
                # 并发/重跑时也可能撞上这个：ECU 认为已经不需要密钥了
                log("   ECU 回 0x24（不需要密钥了），继续")
            else:
                raise
    check_cancel()

    # 4 编程会话
    on_step(4, 8, "进入编程会话")
    r = uds.request(bytes([0x10, 0x02]), label="10 02")
    uds.expect(r, bytes([0x50, 0x02]), "进入编程会话")

    # 5 擦除
    if do_erase:
        on_step(5, 8, "擦除 App 区（约 0.4~3 秒）")
        t0 = time.time()
        r = uds.request(bytes([0x31, 0x01, 0xFF, 0x00]),
                        pending_timeout=60.0, label="31 01 FF00")
        uds.expect(r, bytes([0x71, 0x01, 0xFF, 0x00]), "擦除")
        if len(r) < 5 or r[4] != 0x00:
            raise FlashError("擦除返回失败，状态 = 0x%02X"
                             % (r[4] if len(r) > 4 else 0xFF))
        log("   擦除完成，用时 %.0f ms" % ((time.time() - t0) * 1000))
    else:
        log("   跳过擦除")

    check_cancel()

    # 6 请求下载
    on_step(6, 8, "请求下载")
    req = bytes([0x34, 0x00, ALFID]) + struct.pack(">I", base) + struct.pack(">I", total)
    r = uds.request(req, label="34 00 44 ...")
    uds.expect(r, bytes([0x74]), "请求下载")
    if len(r) >= 4 and r[1] == 0x20:
        max_block = (r[2] << 8) | r[3]
        log("   ECU 允许单块最大 %d 字节（数据 %d 字节）" % (max_block, max_block - 2))

    # 7 传输数据
    bs = max(8, min(block_size, BLOCK_SIZE))
    bs = bs - (bs % 8)                      # Flash 写入要求 8 字节对齐
    nblocks = (total + bs - 1) // bs
    on_step(7, 8, "传输数据（%d 块 × %d 字节）" % (nblocks, bs))
    log("   每块 %d 字节，共 %d 块；失败重试 %d 次" % (bs, nblocks, retries))

    t_data = time.time()
    bsc = 1
    offset = 0
    while offset < total:
        check_cancel()
        chunk = image[offset:offset + bs]
        req = bytes([0x36, bsc]) + chunk

        # 重试：超时就重发同一块（同一 BSC）。
        # ECU 那边如果整条消息没收到，它期望的 BSC 也没变，所以重发是安全的。
        last_err = None
        for attempt in range(retries + 1):
            try:
                r = uds.request(req, timeout=5.0)
                if not r.startswith(bytes([0x76])):
                    raise FlashError("传输数据响应不符：%s" % r.hex(" "))
                if len(r) >= 2 and r[1] != bsc:
                    raise FlashError("块序号不符：期望 %d，ECU 回 %d" % (bsc, r[1]))
                last_err = None
                break
            except UdsTimeout as e:
                last_err = e
                if attempt < retries:
                    log("   块 0x%02X 超时，重试第 %d 次..." % (bsc, attempt + 1))
                    continue
        if last_err is not None:
            raise FlashError("块 0x%02X（偏移 0x%X）重试 %d 次仍超时。"
                             "看 Bootloader 串口是否打印『传输中止』"
                             % (bsc, offset, retries))

        offset += len(chunk)
        bsc = (bsc + 1) & 0xFF
        on_progress(offset, total)

    dt = time.time() - t_data
    log("   共 %d 块，用时 %.2f 秒，速度 %.1f KB/s"
        % (nblocks, dt, total / dt / 1024.0))

    # 8 结束传输
    on_step(8, 8, "结束传输")
    r = uds.request(bytes([0x37]), label="37")
    uds.expect(r, bytes([0x77]), "结束传输")

    # 9 校验
    local_crc = zlib.crc32(image) & 0xFFFFFFFF
    if verify:
        on_step(8, 8, "CRC 校验")
        r = uds.request(bytes([0x31, 0x01, 0xFF, 0x01]),
                        pending_timeout=60.0, label="31 01 FF01")
        uds.expect(r, bytes([0x71, 0x01, 0xFF, 0x01]), "CRC 校验")
        if len(r) < 9:
            raise FlashError("CRC 校验响应太短：%s" % r.hex(" "))
        status = r[4]
        ecu_crc = struct.unpack(">I", r[5:9])[0]
        log("   本地 CRC32 = 0x%08X" % local_crc)
        log("   ECU  CRC32 = 0x%08X" % ecu_crc)
        if status != 0x00:
            raise FlashError("ECU 报告 CRC 不一致（状态 0x%02X）" % status)
        if ecu_crc != local_crc:
            raise FlashError("CRC 数值不一致：本地 0x%08X，ECU 0x%08X"
                             % (local_crc, ecu_crc))
        log("   [OK] CRC 一致，Bootloader 已把 App 标记为有效")
    else:
        log("   [注意] 跳过校验 → Bootloader 不会把 App 标记为有效，复位后仍留在 Bootloader")

    # 10 复位
    if do_reset:
        on_step(8, 8, "复位 ECU")
        try:
            uds.request(bytes([0x11, 0x01]), timeout=2.0, label="11 01")
        except UdsTimeout:
            log("   （没收到 51 01，ECU 可能已经复位，正常）")

    elapsed = time.time() - t_start
    log("=" * 46)
    log("刷写成功，总用时 %.2f 秒" % elapsed)
    log("  镜像 %d 字节，CRC32 0x%08X" % (total, local_crc))
    log("=" * 46)
    return {"size": total, "crc": local_crc, "elapsed": elapsed,
            "blocks": nblocks}
