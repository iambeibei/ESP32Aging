#!/usr/bin/env python3
"""
batch_erase.py - 一键并行擦除多个 ESP32 设备
适用环境: ESP-IDF venv (Windows / Linux / macOS)
依赖: pyserial (esptool 已自带)
"""

import argparse
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    from serial.tools import list_ports
except ImportError:
    print("错误: 缺少 pyserial，请运行: pip install pyserial")
    sys.exit(1)

# ESP 设备常见 USB VID：Espressif 原生 USB-JTAG、CP210x、CH340/CH341、FTDI
ESP_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}


def detect_ports(filter_esp=True):
    """自动扫描串口，默认只保留疑似 ESP 设备的端口"""
    ports = []
    for p in list_ports.comports():
        if filter_esp and p.vid not in ESP_VIDS:
            continue
        ports.append(p.device)
    return sorted(ports)


def run_erase(port, chip, baud, extra_args):
    """对单个端口执行擦除，实时打印输出"""
    cmd = [sys.executable, "-m", "esptool"]
    if chip:
        cmd += ["--chip", chip]
    cmd += ["-p", port, "-b", str(baud), "erase_flash"]
    if extra_args:
        cmd += extra_args

    start = time.time()
    lines = []
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                print(f"[{port}] {line}")
                lines.append(line)
        proc.wait()
        elapsed = time.time() - start
        if proc.returncode == 0:
            return port, True, elapsed, ""
        tail = lines[-1] if lines else "unknown error"
        return port, False, elapsed, tail
    except Exception as e:
        return port, False, time.time() - start, str(e)


def main():
    parser = argparse.ArgumentParser(description="并行擦除多个 ESP 设备")
    parser.add_argument("--ports", help="逗号分隔的端口，如 COM3,COM4；不填则自动检测")
    parser.add_argument("--chip", default="esp32s3",
                        help="芯片类型，默认 esp32s3；传空字符串则自动检测")
    parser.add_argument("--baud", type=int, default=460800,
                        help="波特率，默认 460800（不稳定可降到 230400）")
    parser.add_argument("--max-parallel", type=int, default=0,
                        help="最大并行数，0 表示全部并行")
    parser.add_argument("--all-ports", action="store_true",
                        help="不按 ESP VID 过滤，使用所有串口")
    args = parser.parse_args()

    ports = ([p.strip() for p in args.ports.split(",") if p.strip()]
             if args.ports else detect_ports(filter_esp=not args.all_ports))

    if not ports:
        print("未发现任何串口。请连接设备，或用 --ports COM3,COM4 手动指定。")
        sys.exit(1)

    print(f"发现 {len(ports)} 个端口: {', '.join(ports)}")
    print(f"芯片: {args.chip or '自动检测'} | 波特率: {args.baud}")
    print("-" * 60)

    max_workers = len(ports) if args.max_parallel <= 0 else min(args.max_parallel, len(ports))
    results = []

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {
            pool.submit(run_erase, p, args.chip or None, args.baud, None): p
            for p in ports
        }
        for fut in as_completed(futures):
            results.append(fut.result())

    print("-" * 60)
    print("结果汇总:")
    success = 0
    for port, ok, elapsed, err in sorted(results):
        status = "OK  " if ok else "FAIL"
        suffix = "" if ok else f"  原因: {err}"
        print(f"  [{status}] {port:12s}  {elapsed:6.1f}s{suffix}")
        if ok:
            success += 1

    print(f"\n完成: {success}/{len(ports)} 成功")
    sys.exit(0 if success == len(ports) else 2)


if __name__ == "__main__":
    main()