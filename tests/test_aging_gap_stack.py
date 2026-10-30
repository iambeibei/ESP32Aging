#!/usr/bin/env python3
"""检查实际 Xtensa 构建产物的补查调用链栈帧；先完成 ESP-IDF 编译。

python3 tests/test_aging_gap_stack.py [--build-dir build/mesh-lite]
只读取 compile_commands.json 和目标文件，不替代实机栈低水位验收。
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRAME_LIMIT_BYTES = 512
FUNCTIONS = {
    'Task/appTask.c': ['app_HTTP_Auto_GetProtocol'],
    'Task/aging_gap.c': ['aging_gap_poll', 'gap_log_stack', 'gap_parse_ids'],
    'DataStorage/SqLite/SqLite.c': [
        'QueryStructuredRecordBySession', 'fill_query_result_from_stmt', 'fill_legacy_float_fields',
    ],
    'DataStorage/SqLite/sqllib1.c': [
        'db_build_data_report_json', 'parse_or_convert_value_array', 'build_legacy_value_array',
    ],
    'APP/HTTP/GetProto.c': ['test_http_post_record_query', 'http_request_execute_full'],
    'APP/MQTT/mqtt_app.c': ['app_mqtt_publish', 'mqtt_enqueue_for_session'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/mesh-lite')
    build = parser.parse_args().build_dir.resolve()
    commands_file = build / 'compile_commands.json'
    assert commands_file.is_file(), f'请先完成 ESP-IDF 编译: {commands_file}'
    commands = json.loads(commands_file.read_text())
    frames = {}
    for unit, names in FUNCTIONS.items():
        source = ROOT / 'main' / unit
        entry = next(item for item in commands if Path(item['file']).resolve() == source)
        args = entry.get('arguments') or shlex.split(entry['command'])
        compiler = next(arg for arg in args if arg.endswith('xtensa-esp32s3-elf-gcc'))
        objdump = Path(compiler).with_name('xtensa-esp32s3-elf-objdump')
        obj = Path(entry['directory']) / args[args.index('-o') + 1]
        assert obj.is_file(), f'缺少目标文件，请先编译: {obj}'
        assert obj.stat().st_mtime_ns >= source.stat().st_mtime_ns, f'目标文件过期，请重新编译: {source}'
        for name in names:
            assembly = subprocess.check_output(
                [str(objdump), '-d', '--disassemble=' + name, str(obj)], text=True)
            # 当前工程使用 Xtensa windowed ABI，entry a1,N 给出编译后的实际栈帧。
            match = re.search(r'\bentry\s+a1,\s*(0x[0-9a-fA-F]+|[0-9]+)\b', assembly)
            assert match, f'未找到 {unit}:{name} 栈帧，需检查工具链/优化设置，不能跳过检查'
            value = match.group(1)
            size = int(value, 16 if value.startswith('0x') else 10)
            frames[unit + ':' + name] = size
            assert size <= FRAME_LIMIT_BYTES, f'{unit}:{name} 栈帧 {size} 超过 {FRAME_LIMIT_BYTES} 字节'
            print(f'PASS {unit}:{name} frame={size} bytes')

    config = ROOT / 'main/Task/task_config.h'
    match = re.search(r'^#define\s+LG_STACK_HTTP_AUTO\s+(\d+)\b', config.read_text(), re.M)
    assert match and int(match.group(1)) >= 8192, 'http_auto 栈预算必须至少为 8192 字节'
    app_object = next(item for item in commands if Path(item['file']).resolve() == ROOT / 'main/Task/appTask.c')
    args = app_object.get('arguments') or shlex.split(app_object['command'])
    obj = Path(app_object['directory']) / args[args.index('-o') + 1]
    assert obj.stat().st_mtime_ns >= config.stat().st_mtime_ns, '栈配置变更后须重新编译 appTask.c'
    print(f'PASS {len(frames)} actual Xtensa frames <= {FRAME_LIMIT_BYTES} bytes; http_auto stack={match.group(1)} bytes')
    print('实机仍需验证最低剩余栈 >= 2048 字节；静态检查不包含 SDK/SQLite/cJSON 库的完整调用深度。')


if __name__ == '__main__':
    main()
