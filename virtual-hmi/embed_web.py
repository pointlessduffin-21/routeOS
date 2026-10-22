"""Embed the browser assets from the same sources used by the host simulator."""
from pathlib import Path
Import('env')

project = Path(env.subst('$PROJECT_DIR'))
output = Path(env.subst('$BUILD_DIR')) / 'generated'
output.mkdir(parents=True, exist_ok=True)
assets = ['#pragma once', '#include <Arduino.h>']
for name, symbol in [('index.html', 'hmi_html'), ('wire.js', 'hmi_wire_js'), ('viewer.js', 'hmi_viewer_js')]:
    text = (project / 'web' / name).read_text(encoding='utf-8')
    if name == 'index.html':
        # One HTTP response avoids parallel asset fetches competing with tile
        # writes on the small synchronous device HTTP server during reload.
        for script in ('wire.js', 'viewer.js'):
            code = (project / 'web' / script).read_text(encoding='utf-8')
            text = text.replace(f'<script src="/{script}"></script>', f'<script>{code}</script>')
    assets.append(f'const char {symbol}[] PROGMEM = R"HMI_ASSET({text})HMI_ASSET";')
target = output / 'web_assets.h'
content = '\n'.join(assets)
if not target.exists() or target.read_text(encoding='utf-8') != content:
    target.write_text(content, encoding='utf-8')
env.Append(CPPPATH=[str(output)])
env.Append(CPPDEFINES=[('LV_CONF_PATH', (project / 'include' / 'lv_conf.h').as_posix())])
