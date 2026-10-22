"""Prepare an Arduino IDE sketch without modifying or flashing routeOS source."""
import argparse
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]


def export(target, destination, control=False):
    destination = destination.resolve()
    # Refuse reuse: exporting again must not overwrite edits made in Arduino IDE.
    destination.mkdir(parents=True, exist_ok=False)
    source = ROOT / ('src' if target == 'production' else 'virtual-hmi/src')
    # Arduino IDE shows only top-level files as tabs, so keep sources flat and
    # make main.cpp the visible .ino. Subfolders (eez_generated/) stay as-is.
    sketch_source = destination
    shutil.copytree(source, destination, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('*.bak', '__pycache__', '.eez-project-build'))
    (destination / 'main.cpp').rename(destination / (destination.name + '.ino'))
    # Arduino compiles subfolders only under src/, so EEZ's generated C must live there.
    generated = destination / 'eez_generated'
    if generated.exists():
        (destination / 'src').mkdir(exist_ok=True)
        generated.rename(destination / 'src' / 'eez_generated')
        for file in destination.glob('*.cpp'):
            text = file.read_text(encoding='utf-8')
            if '"eez_generated/' in text:
                file.write_text(text.replace('"eez_generated/', '"src/eez_generated/'), encoding='utf-8')
    if target == 'hmi':
        shutil.copy2(ROOT / 'virtual-hmi/include/lv_conf.h', sketch_source / 'lv_conf.h')
        # The routeOS Core library is a PlatformIO library; the Arduino IDE builds it as part of the sketch.
        core = destination / 'src' / 'routeos_core'
        shutil.copytree(ROOT / 'modules/routeos-core/src', core)
        for file in [*destination.glob('*.cpp'), *destination.glob('*.ino')]:
            text = file.read_text(encoding='utf-8')
            if '<routeos_core.h>' in text:
                file.write_text(text.replace('<routeos_core.h>', '"src/routeos_core/routeos_core.h"'), encoding='utf-8')
        assets = ['#pragma once', '#include <Arduino.h>']
        web = ROOT / 'virtual-hmi/web'
        for name, symbol in [('index.html', 'hmi_html'), ('wire.js', 'hmi_wire_js'),
                             ('viewer.js', 'hmi_viewer_js')]:
            content = (web / name).read_text(encoding='utf-8')
            if name == 'index.html':
                for script in ('wire.js', 'viewer.js'):
                    content = content.replace(f'<script src="/{script}"></script>',
                                              '<script>' + (web / script).read_text(encoding='utf-8') + '</script>')
            assert ')HMI_ASSET"' not in content, 'Asset contains raw-string delimiter'
            assets.append(f'const char {symbol}[] PROGMEM = R"HMI_ASSET({content})HMI_ASSET";')
        (sketch_source / 'web_assets.h').write_text('\n'.join(assets) + '\n', encoding='utf-8')
        flags = ['-DLV_CONF_INCLUDE_SIMPLE',
                 f'"-DLV_CONF_PATH={(sketch_source / "lv_conf.h").as_posix()}"',
                 '-DWEBSOCKETS_SERVER_CLIENT_MAX=2', '-DWEBSOCKETS_TCP_TIMEOUT=2000',
                 '-DHMI_ALLOW_CONTROL=' + str(int(control)),
                 '-DROUTEOS_MANAGE=' + str(int(control))]       # same as the PlatformIO control environment
        (destination / 'build_opt.h').write_text('\n'.join(flags) + '\n', encoding='utf-8')
    return destination / (destination.name + '.ino')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['production', 'hmi'])
    parser.add_argument('--output', type=Path, help='New sketch folder; must not exist')
    parser.add_argument('--control', action='store_true', help='Enable opt-in pointer/keyboard support for HMI')
    args = parser.parse_args()
    if args.control and args.target != 'hmi':
        parser.error('--control applies only to hmi')
    destination = args.output or ROOT / 'build/arduino' / ('routeOS' if args.target == 'production' else 'routeOS_HMI')
    if destination.exists():
        parser.error('Output already exists; choose a new --output folder to preserve your edits')
    print('Open in Arduino IDE:', export(args.target, destination, args.control))
    print('Wi-Fi secrets were not copied. This command does not upload to the ESP32.')
