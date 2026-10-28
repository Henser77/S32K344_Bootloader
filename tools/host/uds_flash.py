#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uds_flash.py —— 命令行版刷写工具（图形界面是 flash_gui.py / 上位机.bat）

【为什么还留一个命令行版】
图形界面日常用；命令行在两种场合更好用：
    1. 排障时能打印每一帧原始数据（--verbose）
    2. 脚本化 / 批量刷写

两者共用同一个协议核心 flash_core.py —— "怎么刷"只有一份实现。

用法：
    python32_full\\python.exe uds_flash.py            # 引导页
    python32_full\\python.exe uds_flash.py --probe    # 连通性自检
    python32_full\\python.exe uds_flash.py --image app.elf
"""

import argparse
import os
import re
import sys
import time

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import flash_core as core
from can_driver import (CanDevice, DEFAULT_DLL, BAUD_TABLE,
                        find_working_timing, check_arch)


def print_quickstart():
    print("=" * 68)
    print("  S32K344 Bootloader  刷写工具（命令行版）")
    print("=" * 68)
    print("""
【日常请用图形界面】
    双击 tools\\上位机.bat      <- 推荐，一个窗口点按钮就行

命令行版适合排障和脚本化：
     --probe                确认能和 ECU 通信
     --probe-timing         自动尝试 8 组波特率时序
     --selftest             适配器自环（不需要板子）
     --image <file>         刷写 .elf 或 .bin
     --dry-run              只解析镜像，不发 CAN
     --verbose              打印每一帧原始数据
""")
    print("=" * 68)


def warn_if_tool_running():
    """USB_CAN Tool 开着会独占适配器，提前提醒免得白折腾"""
    try:
        import subprocess
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq USB_CAN_Tool.exe"],
            capture_output=True, text=True, errors="replace")
        if "USB_CAN_Tool.exe" in out.stdout:
            print("  [注意] USB_CAN Tool 正在运行，它会独占适配器。请先关掉。")
            print("")
            return True
    except Exception:
        pass
    return False


def make_progress_printer():
    state = {"t0": time.time()}

    def on_progress(done, total):
        pct = 100.0 * done / total if total else 100.0
        elapsed = max(1e-6, time.time() - state["t0"])
        speed = done / elapsed / 1024.0
        n = int(28 * pct / 100.0)
        bar = "#" * n + "." * (28 - n)
        sys.stdout.write("\r    [%s] %5.1f%%  %6d/%-6d  %5.1f KB/s"
                         % (bar, pct, done, total, speed))
        sys.stdout.flush()
        if done >= total:
            sys.stdout.write("\n")
    return on_progress


def _parse_timing(txt):
    t0, t1 = [int(x, 16) for x in re.split(r"[,/\s]+", txt.strip())]
    return (t0, t1)


def do_probe(args):
    print("=" * 66)
    print("连通性自检")
    print("=" * 66)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)
    elif args.probe_timing:
        print("  逐个尝试候选 Timing ...")
        found = find_working_timing(args.devtype, args.devindex, args.channel,
                                    args.dll, core.CAN_TX_ID, core.CAN_RX_ID,
                                    verbose=False)
        if not found:
            print("  没找到能通信的 Timing")
            print("  请检查：板子是否上电 / 串口有没有输出 / CAN 线是否接对")
            return 2
        timing = found
        print("  找到可用时序 0x%02X/0x%02X" % timing)

    warn_if_tool_running()
    print("  DevType=%d Index=%d Channel=%d Timing=0x%02X/0x%02X"
          % (args.devtype, args.devindex, args.channel, timing[0], timing[1]))

    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("  [X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        uds = core.UdsClient(tp, log=lambda s: print("    " + s))
        print("\n  发 TesterPresent (3E 00) ...")
        r = uds.request(bytes([0x3E, 0x00]), timeout=1.0, label="3E 00")
        print("\n  [OK] 通信正常，ECU 应答 %s" % r.hex(" ").upper())
        return 0
    except Exception as e:
        print("\n  [X] %s" % e)
        print("\n  可能原因：")
        print("    1. 板子没上电，或没跑这个工程的 Bootloader（先看串口输出）")
        print("    2. 波特率/Timing 不对（试 --probe-timing）")
        print("    3. CAN_H/CAN_L 接反，或没接 120 欧终端电阻")
        print("    4. 接的是另一路 CAN 口（试 --channel 1）")
        return 2
    finally:
        try:
            dev.close()
        except Exception:
            pass


def do_reset_only(args):
    """
    只发 11 01，并判断"复位到底有没有生效"。

    【怎么判断】
      真正复位之后：芯片重启 -> Bootloader 跑 -> 校验 App -> 1.5 秒启动窗口
                    -> 没人来 -> 跳到 App（App 不认 UDS）
      所以 t+3 秒再发 3E 00：
          不应答  -> 复位成功（已经在跑 App 了）
          还应答  -> 复位没生效（还留在 Bootloader）

    【最终判据还是串口】
      复位成功时串口会重新打印一遍横幅（EcuM_Init 第一步）。
      工具只是在看不到串口时给个快速判断。
    """
    print("=" * 66)
    print("测试复位（服务 0x11 01）")
    print("=" * 66)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)

    warn_if_tool_running()
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("  [X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        uds = core.UdsClient(tp, log=lambda s: print("    " + s))

        print("\n  [1/3] 先确认通信正常（3E 00）")
        r = uds.request(bytes([0x3E, 0x00]), label="3E 00")
        uds.expect(r, bytes([0x7E, 0x00]), "测试连接")
        print("        ECU 在 Bootloader 里")

        print("\n  [2/3] 发 11 01")
        r = uds.request(bytes([0x11, 0x01]), label="11 01")
        uds.expect(r, bytes([0x51, 0x01]), "复位")
        print("        ECU 回了 51 01")

        print("\n  [3/3] 等 3 秒，再试探（3E 00）...")
        time.sleep(3.0)

        still_alive = False
        try:
            r2 = uds.request(bytes([0x3E, 0x00]), timeout=1.5)
            still_alive = (r2[:2] == bytes([0x7E, 0x00]))
        except Exception:
            still_alive = False

        print("")
        if still_alive:
            print("  [结果] 3 秒后 ECU 仍在应答 -> 复位【没有生效】")
            print("         它还在 Bootloader 里。")
            print("         串口应该打印了「复位未生效，改为直接跳转 App」。")
            return 2

        print("  [结果] 3 秒后 ECU 不再应答 -> 复位【很可能生效了】")
        print("         它应该已经重启并跳到 App。")
        print("         ★ 最终确认看串口：有没有重新打印一遍横幅")
        print("           (S32K344 Bootloader  v1.12)")
        return 0
    except core.FlashError as e:
        print("\n  [X] %s" % e)
        return 3
    except KeyboardInterrupt:
        print("\n用户中断")
        return 1
    finally:
        try:
            dev.close()
        except Exception:
            pass


def do_boot_request(args):
    """
    设置/清除"下次上电留在 Bootloader"的标志（例程 0xFF03 / 0xFF04）。

    【解决什么问题】
      发布版的启动窗口只有 1 秒（甚至在 App 支持请求后可以设成 0）。
      想在短时间内连续刷好几次，每次都抓窗口很痛苦。
      按下这个门铃之后，ECU 复位回来会**直接留在 Bootloader**，不用抓窗口。

    【ECU 侧做了什么】
      BootParam_SetBootRequest()  -> 往参数区账本写 BootRequest = 1
      BootParam_ClearBootRequest()-> 写回 0
      Bootloader 每次上电检查这个字段：置位就清掉并留下（只消费一次）。

    【流程】
      10 03 -> 27 01 -> 27 02 -> 10 02 -> 31 01 FF0x
      --request-boot 还会紧接着发 11 01 复位，一步到位。
    """
    want_set = bool(args.request_boot)

    print("=" * 66)
    if want_set:
        print("请求：下次上电留在 Bootloader（例程 0xFF03）")
    else:
        print("取消：下次上电正常跳 App（例程 0xFF04）")
    print("=" * 66)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)

    warn_if_tool_running()
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("  [X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        uds = core.UdsClient(tp, log=lambda s: print("    " + s))

        print("\n  [1/5] 扩展会话")
        r = uds.request(bytes([0x10, 0x03]), label="10 03")
        uds.expect(r, bytes([0x50, 0x03]), "切换会话")

        print("  [2/5] 请求种子")
        r = uds.request(bytes([0x27, 0x01]), label="27 01")
        uds.expect(r, bytes([0x67, 0x01]), "请求种子")
        seed = int.from_bytes(r[2:6], "big") if len(r) >= 6 else 0

        print("  [3/5] 安全解锁")
        if seed == 0:
            print("         种子为 0，ECU 已解锁，跳过")
        else:
            key = core.key_from_seed(seed)
            print("         种子 0x%08X -> 密钥 0x%08X" % (seed, key))
            r = uds.request(bytes([0x27, 0x02]) + key.to_bytes(4, "big"),
                            label="27 02")
            uds.expect(r, bytes([0x67, 0x02]), "发送密钥")

        print("  [4/5] 编程会话")
        r = uds.request(bytes([0x10, 0x02]), label="10 02")
        uds.expect(r, bytes([0x50, 0x02]), "编程会话")

        rid = 0x03 if want_set else 0x04
        print("  [5/5] 例程 0xFF%02X" % rid)
        r = uds.request(bytes([0x31, 0x01, 0xFF, rid]),
                        label="31 01 FF%02X" % rid)
        uds.expect(r, bytes([0x71, 0x01, 0xFF, rid]), "例程控制")
        if len(r) < 5 or r[4] != 0x00:
            print("\n  [X] 例程返回失败，状态 0x%02X"
                  % (r[4] if len(r) > 4 else 0xFF))
            print("      BootRequest 没能写进参数区（Flash 写失败？）")
            return 3

        print("\n" + "=" * 66)
        if want_set:
            print("  [OK] 已写 BootRequest = 1")
            print("=" * 66)
            print("  现在复位，让新的 Bootloader 上电时看到这个请求。")
            try:
                r = uds.request(bytes([0x11, 0x01]), label="11 01")
                uds.expect(r, bytes([0x51, 0x01]), "复位")
                print("  [OK] ECU 回了 51 01，正在复位")
            except Exception:
                print("  [!] 没收到 51 01（可能已经复位了），不影响")
            print("")
            print("  下一步：等 1~2 秒，然后直接刷写（板子会留在 Bootloader）：")
            print("      uds_flash.py --image <你的.elf> -y")
        else:
            print("  [OK] 已写 BootRequest = 0")
            print("=" * 66)
            print("  下次上电：校验通过就直接跳 App（不再等你）。")
        return 0
    except core.FlashError as e:
        print("\n  [X] %s" % e)
        return 3
    except KeyboardInterrupt:
        print("\n用户中断")
        return 1
    finally:
        try:
            dev.close()
        except Exception:
            pass


def do_erase_only(args):
    """
    只擦除 App 区（服务 0x31 01 FF00），不下载任何东西。

    【为什么要单独一个功能】
      刷写流程本身就包含擦除，所以"要刷新固件"时不需要它。
      但下面这些场合需要"只擦不写"：
          - 板子要交出去 / 存档，不想留下旧固件
          - 想验证"没有 App 时是否老实停在 Bootloader"
          - 刷写卡住了，想先清干净再从头来

    【擦完之后板子会怎样】
      Bootloader 的 0x31 FF00 会先调 BootParam_SetInvalid()，把参数区的
      AppValid 置 0，再擦 App 区。所以擦完之后：
          App 区 = 全 0xFF，参数区说"无效"
          -> 下次上电 Bootloader 会直接停在 Bootloader（不再等 1.5 秒窗口）

    【前提】板子必须已经在 Bootloader 里（跑 App 时连不上）。
            可以先跑 --wait 30，给板子上电抓到它，再跑本命令。
    """
    print("=" * 66)
    print("只擦除 App 区（不下载）")
    print("=" * 66)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)

    warn_if_tool_running()
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("  [X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        uds = core.UdsClient(tp, log=lambda s: print("    " + s))

        # ---- 如果同时给了 --wait，就先在这同一个会话里等 ECU 出现 ----
        #     为什么要同一进程：
        #       拆成两条命令的话，--wait 抓到之后进程立刻退出、关闭设备，
        #       而 Bootloader 的 1.5 秒窗口已经过去 -> 板子跳回 App，
        #       第二条命令就连不上了（实测踩到过）。
        if args.wait > 0.0:
            print("  先等 ECU 出现（最长 %.0f 秒）..." % args.wait)
            t0w = time.time()
            got = False
            tries = 0
            while (time.time() - t0w) < args.wait:
                tries += 1
                try:
                    rr = uds.request(bytes([0x3E, 0x00]), timeout=0.15)
                    if rr[:2] == bytes([0x7E, 0x00]):
                        got = True
                        break
                except Exception:
                    pass
            if not got:
                print("  [X] %.0f 秒内没等到 ECU（给板子断电重上电）" % args.wait)
                return 2
            print("  [OK] 抓到 ECU（第 %d 次尝试），继续擦除\\n" % tries)

        print("\n  [1/4] 扩展会话")
        r = uds.request(bytes([0x10, 0x03]), label="10 03")
        uds.expect(r, bytes([0x50, 0x03]), "切换会话")

        print("  [2/4] 请求种子")
        r = uds.request(bytes([0x27, 0x01]), label="27 01")
        uds.expect(r, bytes([0x67, 0x01]), "请求种子")
        seed = int.from_bytes(r[2:6], "big") if len(r) >= 6 else 0

        print("  [3/4] 安全解锁")
        if seed == 0:
            print("         种子为 0，ECU 已解锁，跳过")
        else:
            key = core.key_from_seed(seed)
            print("         种子 0x%08X -> 密钥 0x%08X" % (seed, key))
            r = uds.request(bytes([0x27, 0x02]) + key.to_bytes(4, "big"),
                            label="27 02")
            uds.expect(r, bytes([0x67, 0x02]), "发送密钥")

        print("  [4/4] 编程会话 + 擦除")
        r = uds.request(bytes([0x10, 0x02]), label="10 02")
        uds.expect(r, bytes([0x50, 0x02]), "编程会话")
        t0 = time.time()
        r = uds.request(bytes([0x31, 0x01, 0xFF, 0x00]),
                        pending_timeout=60.0, label="31 01 FF00")
        uds.expect(r, bytes([0x71, 0x01, 0xFF, 0x00]), "擦除")
        if len(r) < 5 or r[4] != 0x00:
            print("\n  [X] 擦除返回失败，状态 0x%02X" % (r[4] if len(r) > 4 else 0xFF))
            return 3

        print("\n" + "=" * 66)
        print("  [OK] 擦除完成，用时 %.0f ms" % ((time.time() - t0) * 1000))
        print("=" * 66)
        print("  App 区现在是全 0xFF，参数区已标记为无效。")
        print("  下次上电：Bootloader 会**直接停在 Bootloader**（不再等 1.5 秒窗口）。")
        print("  想再刷固件：直接跑 --image（板子已经在 Bootloader 里了）。")
        return 0
    except core.FlashError as e:
        print("\n  [X] %s" % e)
        return 3
    finally:
        try:
            dev.close()
        except Exception:
            pass


def do_wait(args):
    """
    循环发 TesterPresent，等 ECU 出现。

    【配合 Bootloader 的启动窗口用】
      Bootloader 上电后有 1.5 秒窗口，这期间收到任意 UDS 请求就留在 Bootloader。
      所以流程是：
          1. 先跑这条命令（它会一直发 3E 00）
          2. 给板子断电、再上电
          3. 板子在上电窗口里收到我们的帧 -> 留在 Bootloader
          4. 本命令收到 7E 00 -> 打印"抓到了"，然后就该去刷写了

    【为什么必须一直发】
      我们不知道板子什么时候上电，只能在窗口可能出现的时间段里持续发。
    """
    print("=" * 66)
    print("等待 ECU 出现（配合启动窗口）")
    print("=" * 66)
    print("  接下来请给板子断电、再上电（Bootloader 只在开头 %.1f 秒响应）"
          % (args.wait if args.wait else 0))
    print("  本命令会持续发 3E 00，最长等 %.0f 秒\n" % args.wait)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)

    warn_if_tool_running()
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("  [X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        uds = core.UdsClient(tp, log=lambda s: None)

        t0 = time.time()
        tries = 0
        while (time.time() - t0) < args.wait:
            tries += 1
            try:
                r = uds.request(bytes([0x3E, 0x00]), timeout=0.05)
                if r[:2] == bytes([0x7E, 0x00]):
                    print("\n  [OK] 抓到了！第 %d 次尝试收到 ECU 应答 %s"
                          % (tries, r.hex(" ").upper()))
                    print("       板子现在在 Bootloader 里，可以开始刷写了。")
                    return 0
            except Exception:
                pass
            sys.stdout.write("\r  已尝试 %d 次，剩余 %.0f 秒 ..."
                             % (tries, max(0.0, args.wait - (time.time() - t0))))
            sys.stdout.flush()

        print("\n  [X] %.0f 秒内没有收到应答" % args.wait)
        print("      检查：板子有没有上电 / 串口有没有 Bootloader 横幅 / CAN 线")
        return 2
    finally:
        try:
            dev.close()
        except Exception:
            pass


def do_selftest(args):
    print("=" * 66)
    print("适配器自环测试（不需要板子）")
    print("=" * 66)

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)
    warn_if_tool_running()

    dev = None
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
        dev.start()

        payload = bytes([0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF])
        ok = dev.send(0x123, payload)
        print("\n  发一帧 ID=0x123  %s" % payload.hex(" ").upper())
        print("  VCI_Transmit %s" % ("成功" if ok else "失败"))
        print("\n  结论：适配器能发帧。")
        print("  真正的判据是板子串口有没有输出，不是这个测试。")
        return 0
    except Exception as e:
        print("\n  [X] %s" % e)
        return 1
    finally:
        if dev is not None:
            try:
                dev.close()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(
        description="S32K344 Bootloader 刷写工具（命令行版）",
        epilog="日常请用图形界面：双击 tools\\上位机.bat")
    ap.add_argument("--image")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=core.APP_START)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--timing", default=None, help="例如 0x00,0x1C")
    ap.add_argument("--probe-timing", action="store_true")
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="循环发 3E 00 等 ECU 出现（秒）。配合 Bootloader 的启动窗口："
                         "先跑这条命令，再给板子断电重上电，就能把它留在 Bootloader")
    ap.add_argument("--block", type=int, default=core.TRANSFER_BLOCK_DEFAULT,
                    help="传输块大小（字节）。ECU 允许最大 1024；越大请求越少、越快。"
                         "默认 %d" % core.TRANSFER_BLOCK_DEFAULT)
    ap.add_argument("--cf-delay", type=float, default=0.0,
                    help="连续帧间隔（毫秒）。默认 0 = 由 CAN 适配器和总线速度自然限速。"
                         "以前设 2ms 是为了绕开一个固件 bug（已在 v1.11 修掉）")
    ap.add_argument("--retries", type=int, default=2,
                    help="每块失败重试次数，默认 2")
    ap.add_argument("--no-erase", action="store_true")
    ap.add_argument("--erase-only", action="store_true",
                    help="只擦除 App 区，不下载。用于清掉现有固件"
                         "（擦完参数区会被标记为无效，板子会停在 Bootloader）")
    ap.add_argument("--request-boot", action="store_true",
                    help="按下\"下次上电留在 Bootloader\"的门铃（例程 0xFF03）并复位。"
                         "用于发布版窗口很短/为 0 时，稳定地回到 Bootloader 重刷")
    ap.add_argument("--clear-boot", action="store_true",
                    help="取消上面的请求（例程 0xFF04），下次上电正常跳 App")
    ap.add_argument("--reset-only", action="store_true",
                    help="只发 11 01 并判断复位有没有生效（不下载）。"
                         "用来验证固件的复位手段（MC_ME / AIRCR / 看门狗）是否有效")
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="打印每一帧原始数据")
    ap.add_argument("--dll", default=DEFAULT_DLL)
    ap.add_argument("--devtype", type=int, default=4)
    ap.add_argument("--devindex", type=int, default=0)
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("-y", "--yes", action="store_true")
    args = ap.parse_args()

    try:
        check_arch()
    except RuntimeError as e:
        print("[X] %s" % e)
        return 1

    # ---- 只擦除（不下载）----
    # ---- 请求/取消"下次上电留在 Bootloader" ----
    if args.request_boot or args.clear_boot:
        return do_boot_request(args)

    # ---- 只测复位（不下载）----
    if args.reset_only:
        return do_reset_only(args)

    if args.erase_only:
        return do_erase_only(args)

    # ---- 等 ECU 出现（配合 Bootloader 的启动窗口）----
    if (args.wait > 0.0) and (args.image is None):
        return do_wait(args)

    if args.selftest:
        return do_selftest(args)
    if args.probe:
        return do_probe(args)
    if not args.image:
        print_quickstart()
        return 0

    print("=" * 66)
    print("S32K344 Bootloader 刷写工具（命令行版）")
    print("=" * 66)

    try:
        image, base = core.load_image(args.image, args.start, args.size,
                                      log=lambda s: print("  " + s))
    except Exception as e:
        print("[X] 读镜像失败：%s" % e)
        return 1

    if args.dry_run:
        import zlib
        print("\n[dry-run] 只解析了镜像，没有发任何 CAN 帧")
        print("  本地 CRC32 = 0x%08X" % (zlib.crc32(image) & 0xFFFFFFFF))
        print("  将执行：10 03 -> 27 01 -> 27 02 -> 10 02 -> 31 01 FF00")
        print("          -> 34 00 44 0x%08X 0x%08X" % (base, len(image)))
        print("          -> 36 x %d 块 -> 37 -> 31 01 FF01 -> 11 01"
              % ((len(image) + core.BLOCK_SIZE - 1) // core.BLOCK_SIZE))
        return 0

    if not args.yes:
        print("\n即将擦除 0x%08X 起的 App 区并写入 %d 字节。" % (base, len(image)))
        try:
            input("按回车继续，Ctrl+C 取消 ... ")
        except KeyboardInterrupt:
            print("\n已取消")
            return 1

    timing = (0x00, 0x1C)
    if args.timing:
        timing = _parse_timing(args.timing)

    warn_if_tool_running()
    print("\n打开 CAN 适配器 ...")
    try:
        dev = CanDevice(args.dll, args.devtype, args.devindex, args.channel,
                        timing, verbose=False).open()
    except Exception as e:
        print("[X] %s" % e)
        return 1

    try:
        dev.start()
        tp = core.IsoTpChannel(dev, cf_delay_ms=args.cf_delay)
        if args.verbose:
            tp.trace = lambda d, x: print("    %s %s" % (d, x.hex(" ").upper()))
        uds = core.UdsClient(tp, log=lambda s: print("  " + s))

        print("  块大小 = %d 字节，连续帧间隔 = %.2f ms，失败重试 = %d 次"
              % (args.block, args.cf_delay, args.retries))

        core.flash(uds, image, base,
                   do_erase=not args.no_erase,
                   verify=not args.no_verify,
                   do_reset=not args.no_reset,
                   log=lambda s: print("  " + s),
                   on_progress=make_progress_printer(),
                   block_size=args.block,
                   retries=args.retries)
        print("\n  CAN 统计：发送 %d 帧，接收 %d 帧"
              % (tp.stat_tx_frames, tp.stat_rx_frames))
        return 0
    except core.FlashError as e:
        print("\n[X] 刷写失败：%s" % e)
        print("  排查建议：")
        print("    1. 先用 --probe 确认通信正常")
        print("    2. 看 Bootloader 串口打印（那里有每一步的详细信息）")
        return 3
    except KeyboardInterrupt:
        print("\n\n用户中断")
        return 1
    finally:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
