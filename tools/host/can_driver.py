#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
can_driver.py —— 创芯科技 USB-CAN 适配器的 Python 封装（ctypes + ControlCAN.dll）

【为什么必须 32 位 Python】
ControlCAN.dll 这一版是 i386（32 位）的。64 位 Python 用 ctypes 加载它会报
WinError 193「不是有效的 Win32 应用程序」。本目录下的 python32\\ 就是配好的
免安装 32 位解释器（Python 3.11.9 embeddable）。

【参数是怎么定下来的】
    DevType = 4      从 USB_CAN Tool 的 VCI_CAN.log 里读到：
                     VCI_Transmit:input=(4,0,0,*p,1)
                     VCI_Receive:input=(4,0,0,*p,2500,20)
    Timing  = 00/1C  从 USB_CAN Tool 的 usb_can.ini 里反推（500 kbps）
    如果换了适配器通信不通，用 find_working_timing() 让它把候选值试一遍。

【重要】USB_CAN Tool 开着的时候会独占适配器，VCI_OpenDevice 会返回 0。
        跑本工具之前必须先关掉它。
"""

import ctypes
import os
import sys
import time

DEFAULT_DLL = r"C:\Program Files (x86)\USB_CAN TOOL\ControlCAN.dll"

# ---------------------------------------------------------------------------
# 数据结构（ZLG / 创芯 ControlCAN 标准布局）
# ---------------------------------------------------------------------------


class VCI_INIT_CONFIG(ctypes.Structure):
    """初始化参数，16 字节"""
    _fields_ = [
        ("AccCode", ctypes.c_uint32),
        ("AccMask", ctypes.c_uint32),
        ("Reserved", ctypes.c_uint32),
        ("Filter", ctypes.c_ubyte),      # 0=全收, 1=只收标准帧, 2=只收扩展帧
        ("Timing0", ctypes.c_ubyte),     # BTR0
        ("Timing1", ctypes.c_ubyte),     # BTR1
        ("Mode", ctypes.c_ubyte),        # 0=正常, 1=只听
    ]


class VCI_CAN_OBJ(ctypes.Structure):
    """一帧报文，24 字节"""
    _fields_ = [
        ("ID", ctypes.c_uint32),
        ("TimeStamp", ctypes.c_uint32),
        ("TimeFlag", ctypes.c_ubyte),
        ("SendType", ctypes.c_ubyte),     # 0=正常, 1=单次(不应答), 2=自发自收
        ("RemoteFlag", ctypes.c_ubyte),   # 0=数据帧, 1=远程帧
        ("ExternFlag", ctypes.c_ubyte),   # 0=标准帧, 1=扩展帧
        ("DataLen", ctypes.c_ubyte),
        ("Data", ctypes.c_ubyte * 8),
        ("Reserved", ctypes.c_ubyte * 3),
    ]


class VCI_BOARD_INFO(ctypes.Structure):
    _fields_ = [
        ("hw_Version", ctypes.c_ushort),
        ("fw_Version", ctypes.c_ushort),
        ("dr_Version", ctypes.c_ushort),
        ("in_Version", ctypes.c_ushort),
        ("irq_Num", ctypes.c_ushort),
        ("can_Num", ctypes.c_ubyte),
        ("str_Serial_Num", ctypes.c_char * 20),
        ("str_hw_Type", ctypes.c_char * 40),
        ("Reserved", ctypes.c_ushort * 4),
    ]


# ---------------------------------------------------------------------------
# 波特率表：Timing0=BTR0, Timing1=BTR1
# ---------------------------------------------------------------------------
BAUD_TABLE = {
    1000000: (0x00, 0x14),
    800000:  (0x00, 0x16),
    500000:  (0x00, 0x1C),
    250000:  (0x01, 0x1C),
    125000:  (0x03, 0x1C),
    100000:  (0x04, 0x1C),
}

# 500 kbps 的候选组合（换适配器时可以逐个试）
PROBE_500K = [
    (0x00, 0x1C), (0x01, 0x1C), (0x00, 0x2F), (0x03, 0x1C),
    (0x04, 0x1C), (0x00, 0x1E), (0x00, 0x16), (0x02, 0x1C),
]


def check_arch():
    """确认是 32 位 Python，否则给出明确提示"""
    bits = ctypes.sizeof(ctypes.c_void_p) * 8
    if bits != 32:
        raise RuntimeError(
            "当前 Python 是 %d 位，而 ControlCAN.dll 是 32 位的。\n"
            "请改用本目录下的 32 位解释器：\n"
            "    python32\\python.exe uds_flash.py ..." % bits
        )
    return bits


def load_dll(dll_path=DEFAULT_DLL):
    if not os.path.exists(dll_path):
        raise FileNotFoundError("找不到 ControlCAN.dll：%s" % dll_path)

    check_arch()
    dll = ctypes.WinDLL(dll_path)
    u32 = ctypes.c_uint32

    dll.VCI_OpenDevice.argtypes = [u32, u32, u32]
    dll.VCI_OpenDevice.restype = u32
    dll.VCI_CloseDevice.argtypes = [u32, u32]
    dll.VCI_CloseDevice.restype = u32
    dll.VCI_InitCAN.argtypes = [u32, u32, u32, ctypes.POINTER(VCI_INIT_CONFIG)]
    dll.VCI_InitCAN.restype = u32
    dll.VCI_StartCAN.argtypes = [u32, u32, u32]
    dll.VCI_StartCAN.restype = u32
    dll.VCI_ResetCAN.argtypes = [u32, u32, u32]
    dll.VCI_ResetCAN.restype = u32
    dll.VCI_ClearBuffer.argtypes = [u32, u32, u32]
    dll.VCI_ClearBuffer.restype = u32
    dll.VCI_Transmit.argtypes = [u32, u32, u32, ctypes.POINTER(VCI_CAN_OBJ), u32]
    dll.VCI_Transmit.restype = u32
    dll.VCI_Receive.argtypes = [u32, u32, u32, ctypes.POINTER(VCI_CAN_OBJ), u32, ctypes.c_int]
    dll.VCI_Receive.restype = u32
    dll.VCI_GetReceiveNum.argtypes = [u32, u32, u32]
    dll.VCI_GetReceiveNum.restype = u32
    dll.VCI_ReadBoardInfo.argtypes = [u32, u32, ctypes.POINTER(VCI_BOARD_INFO)]
    dll.VCI_ReadBoardInfo.restype = u32

    if hasattr(dll, "VCI_ConnectDevice"):
        dll.VCI_ConnectDevice.argtypes = [u32, u32]
        dll.VCI_ConnectDevice.restype = u32

    return dll


class CanDevice(object):
    """一路 CAN 通道"""

    def __init__(self, dll_path=DEFAULT_DLL, devtype=4, devindex=0,
                 channel=0, timing=(0x00, 0x1C), verbose=True):
        self.dll = None
        self.dll_path = dll_path
        self.devtype = devtype
        self.devindex = devindex
        self.channel = channel
        self.timing = timing
        self.verbose = verbose
        self.opened = False
        self.started = False
        self._rxbuf = (VCI_CAN_OBJ * 256)()

    # -- 日志 -------------------------------------------------------------
    def _log(self, msg):
        if self.verbose:
            print(msg)

    # -- 打开 / 关闭 -------------------------------------------------------
    def open(self):
        self.dll = load_dll(self.dll_path)

        # ------------------------------------------------------------------
        # 【必须先 CloseDevice 再 OpenDevice —— 这是第二个真 bug】
        #
        # DLL 内部保留"设备是否已打开"的全局状态。上一次没干净关闭时
        # （脚本异常退出，或上一个进程刚退出），直接再调 VCI_OpenDevice：
        #     返回值仍是 1（看起来成功）
        #     但 VCI_GetReceiveNum 永远是 0 —— 收队列还挂在失效的旧句柄上
        #
        # 证据：diag_sniff.py / test_driver.py 场景1 都先 Close 再 Open，
        #       所以一直能收到；CanDevice.open() 少了这一步，所以收不到。
        #
        # 没打开过时 Close 返回 0，无害，所以关不掉就忽略。
        # ------------------------------------------------------------------
        try:
            self.dll.VCI_CloseDevice(self.devtype, self.devindex)
            # Close 之后必须等一会儿再 Open，让适配器内部的 USB 会话复位完成。
            # 不加间隔：OpenDevice 返回 1，但收队列失效（一帧都收不到）。
            # 这是 test_bisect.py 二分定位出来的第三个 bug。
            time.sleep(0.15)
        except Exception:
            pass

        ret = self.dll.VCI_OpenDevice(self.devtype, self.devindex, 0)
        if not ret:
            raise RuntimeError(
                "VCI_OpenDevice 失败（返回 0）。常见原因：\n"
                "  1. USB_CAN Tool 还开着，它独占适配器 —— 先关掉它\n"
                "  2. 适配器没插好 / 驱动没装\n"
                "  3. DevType 不对（当前 %d，可试 0~8）" % self.devtype
            )
        self.opened = True

        # 创芯扩展：有的版本要求先 ConnectDevice
        # ------------------------------------------------------------------
        # 【这里曾经有一个致命 bug：调用 VCI_ConnectDevice】
        #
        # 创芯的 DLL 里确实导出了 VCI_ConnectDevice（厂商工具也会调它），
        # 但只要在我们这套流程里调用它，通道就会失效：
        #     VCI_Transmit 返回 1（看起来"发出去了"）
        #     但 VCI_GetReceiveNum 永远是 0（一帧都收不到）
        #     板子的响应确实在总线上，只是这一路收不到了
        #
        # 证据：diag_sniff.py 不调用它 -> 立刻收到 02 7E 00 CC CC CC CC CC
        #
        # 所以：**不要调用 VCI_ConnectDevice**。这里保留注释作为记录。
        # ------------------------------------------------------------------
        # if hasattr(self.dll, "VCI_ConnectDevice"):
        #     r = self.dll.VCI_ConnectDevice(self.devtype, self.devindex)
        #     self._log("    VCI_ConnectDevice = %d" % r)

        self._log("    设备已打开 (DevType=%d, Index=%d)"
                  % (self.devtype, self.devindex))
        return self

    def start(self):
        t0, t1 = self.timing
        cfg = VCI_INIT_CONFIG()
        cfg.AccCode = 0x00000000
        cfg.AccMask = 0xFFFFFFFF        # 全收
        cfg.Reserved = 0
        cfg.Filter = 0
        cfg.Timing0 = t0
        cfg.Timing1 = t1
        cfg.Mode = 0

        if not self.dll.VCI_InitCAN(self.devtype, self.devindex,
                                    self.channel, ctypes.byref(cfg)):
            raise RuntimeError("VCI_InitCAN 失败（Timing0=0x%02X Timing1=0x%02X）"
                               % (t0, t1))
        if not self.dll.VCI_StartCAN(self.devtype, self.devindex, self.channel):
            raise RuntimeError("VCI_StartCAN 失败")
        self.started = True

        # 【为什么启动后要等一下再清缓冲】
        # 刚 StartCAN 时控制器还在从"未同步"进入"错误主动"状态，
        # 这段时间的自发自收回显会堆在接收队列里。不清掉的话，
        # 第一次"收响应"会先读到这些垃圾帧。
        time.sleep(0.1)
        self.clear()
        time.sleep(0.05)
        self._log("    通道 %d 已启动 (Timing 0x%02X/0x%02X)"
                  % (self.channel, t0, t1))
        return self

    def close(self):
        if self.dll is not None and self.opened:
            self.dll.VCI_CloseDevice(self.devtype, self.devindex)
            self.opened = False
            self.started = False

    def __enter__(self):
        return self.open()

    def __exit__(self, *a):
        self.close()

    # -- 收发 -------------------------------------------------------------
    def clear(self):
        self.dll.VCI_ClearBuffer(self.devtype, self.devindex, self.channel)

    def send(self, can_id, data, extended=False):
        """发一帧。data 长度 1~8。返回 True/False"""
        obj = VCI_CAN_OBJ()
        obj.ID = can_id
        obj.SendType = 0
        obj.RemoteFlag = 0
        obj.ExternFlag = 1 if extended else 0
        obj.DataLen = len(data)
        for i, v in enumerate(data):
            obj.Data[i] = v
        n = self.dll.VCI_Transmit(self.devtype, self.devindex, self.channel,
                                  ctypes.byref(obj), 1)
        return n == 1

    def recv(self, timeout_ms=100):
        """
        收一组帧，最多等 timeout_ms 毫秒。
        返回 [(can_id, bytes, timestamp), ...]
        """
        n = self.dll.VCI_Receive(self.devtype, self.devindex, self.channel,
                                 self._rxbuf, 256, int(timeout_ms))
        out = []
        if n == 0 or n > 256:
            return out
        for i in range(n):
            o = self._rxbuf[i]
            out.append((o.ID, bytes(o.Data[:o.DataLen]), o.TimeStamp))
        return out

    def board_info(self):
        info = VCI_BOARD_INFO()
        if not self.dll.VCI_ReadBoardInfo(self.devtype, self.devindex,
                                          ctypes.byref(info)):
            return None
        return info


def find_working_timing(devtype=4, devindex=0, channel=0, dll_path=DEFAULT_DLL,
                        tx_id=0x7E0, rx_id=0x7E8, candidates=None, verbose=True):
    """
    逐个尝试 Timing 组合，返回第一个能收到 ECU 响应的组合。
    用来应对"换了适配器、时钟不同"的情况。
    """
    candidates = candidates or PROBE_500K
    for (t0, t1) in candidates:
        try:
            dev = CanDevice(dll_path, devtype, devindex, channel, (t0, t1),
                            verbose=False).open()
        except RuntimeError:
            return None          # 设备打不开，跟 Timing 无关
        try:
            dev.start()
            dev.clear()
            dev.send(tx_id, bytes([0x02, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
            deadline = time.time() + 0.3
            while time.time() < deadline:
                for (cid, data, _ts) in dev.recv(20):
                    if cid == rx_id:
                        if verbose:
                            print("    Testing 0x%02X/0x%02X -> 收到响应" % (t0, t1))
                        return (t0, t1)
        finally:
            dev.close()
        if verbose:
            print("    Testing 0x%02X/0x%02X -> 无响应" % (t0, t1))
    return None


if __name__ == "__main__":
    print("can_driver 自检")
    print("  Python 位数:", ctypes.sizeof(ctypes.c_void_p) * 8)
    try:
        check_arch()
    except RuntimeError as e:
        print(e)
        sys.exit(1)
    d = load_dll()
    print("  DLL 加载成功")
