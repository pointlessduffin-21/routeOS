# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for routeOS Desktop Companion
# Build: pyinstaller routeOS.spec  (from the desktop/ directory)

from PyInstaller.utils.hooks import collect_data_files, collect_dynamic_libs

block_cipher = None

a = Analysis(
    ['main.py'],
    pathex=['.'],
    binaries=[],
    datas=[
        # Bundle Qt plugins needed for macOS rendering
        *collect_data_files('PyQt6', includes=['Qt/plugins/**/**.dylib']),
    ],
    hiddenimports=[
        'PyQt6.QtCore',
        'PyQt6.QtGui',
        'PyQt6.QtWidgets',
        'PyQt6.sip',
        'serial',
        'serial.tools',
        'serial.tools.list_ports',
        'serial.tools.list_ports_posix',
        'requests',
        'urllib3',
        'importlib.metadata',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['tkinter', 'matplotlib', 'numpy', 'pandas', 'scipy'],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='routeOS',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='icon.icns',
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='routeOS',
)

app = BUNDLE(
    coll,
    name='routeOS.app',
    icon='icon.icns',
    bundle_identifier='com.routeos.desktop',
    info_plist={
        'CFBundleDisplayName': 'routeOS',
        'CFBundleShortVersionString': '2.0.0',
        'CFBundleVersion': '2.0.0',
        'NSHighResolutionCapable': True,
        'NSHumanReadableCopyright': 'routeOS Project',
        'NSPrincipalClass': 'NSApplication',
        'NSRequiresAquaSystemAppearance': False,  # allow dark mode
        # USB/serial access
        'NSBluetoothAlwaysUsageDescription': 'routeOS uses Bluetooth for device discovery.',
        'com.apple.security.device.usb': True,
    },
)
