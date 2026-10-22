"""Hardware/simulator integration through the actual iShareScreen Qt window.

Run with iShareScreen's Python, which provides Qt and its editable source install.
"""
import argparse
import os
from pathlib import Path
import time
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
if os.name == 'nt':
    os.environ.setdefault('QT_QPA_FONTDIR', str(Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts'))
from PySide6.QtWidgets import QApplication
from isharescreen.frontend.desktop.rvdp import RVDPController, RVDPSessionWindow
from isharescreen.frontend.desktop.session import shutdown_all
from isharescreen.frontend.desktop.theme import apply_theme

parser = argparse.ArgumentParser()
parser.add_argument('--url', default='ws://127.0.0.1:8095/rvdp')
parser.add_argument('--output', type=Path, default=Path('build/hmi-native.png'))
parser.add_argument('--control', action='store_true')
args = parser.parse_args()
app = QApplication([])
apply_theme(app)
controller = RVDPController(args.url, request_control=args.control)
failures = []
controller.failed.connect(lambda kind, text: failures.append((kind, text)))
controller.start()
def wait(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return False
try:
    assert wait(lambda: controller.is_established or failures), 'No complete screen'
    assert not failures, failures
    window = RVDPSessionWindow(controller, 'routeOS EEZ / RVDP')
    window.show()
    assert wait(lambda: window.is_live)
    assert window.screen_widget.image.width() == 240
    assert window.screen_widget.image.height() == 135
    if args.control:
        assert controller.control_granted
        controller.send('pointer', 120, 104, 1)
        time.sleep(0.07)
        controller.send('pointer', 120, 104, 0)
    frames = controller.frames
    controller.send('request', True)
    assert wait(lambda: controller.frames > frames)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    assert window.grab().save(str(args.output))
    assert not failures, failures
    permission = controller.control_granted
    window.close()
    print(f'Native window: 240x135, complete refresh, pointer permission={permission}; saved {args.output}')
finally:
    controller.stop()
    shutdown_all()
