"""Drive real Qt keyboard events through iShareScreen into the hardware calculator."""
import os
from pathlib import Path
import time
import urllib.request,json
os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
os.environ.setdefault('QT_QPA_FONTDIR','C:/Windows/Fonts')
os.environ.setdefault('ISS_STYLE','glass')
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from isharescreen.frontend.desktop.rvdp import RVDPController,RVDPSessionWindow
from isharescreen.frontend.desktop.session import shutdown_all
from isharescreen.frontend.desktop.theme import apply_theme

app=QApplication([]);apply_theme(app)
c=RVDPController('ws://192.168.254.123:81/rvdp',request_control=True)
errors=[];c.failed.connect(lambda *e:errors.append(e));c.start()
def wait(check):
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        app.processEvents()
        if check():return
        time.sleep(.01)
    raise AssertionError(errors or 'Timed out')
def state():return json.load(urllib.request.urlopen('http://192.168.254.123/health',timeout=5))
out=Path('build/calculator');out.mkdir(parents=True,exist_ok=True)
def settle():
    c.send('request')
    until=time.monotonic()+2.5
    while time.monotonic()<until:
        app.processEvents();time.sleep(.01)

try:
    wait(lambda:c.is_established)
    assert c.keyboard_available and c.control_granted
    window=RVDPSessionWindow(c,'Orbit calculator / EEZ Studio');window.show()
    QTest.keyClick(window.screen_widget,Qt.Key.Key_Escape)
    QTest.keyClicks(window.screen_widget,'21*2',delay=35)
    QTest.keyClick(window.screen_widget,Qt.Key.Key_Return)
    wait(lambda:state()['result']=='42')
    frames=c.frames;wait(lambda:c.frames>frames)
    settle()
    assert window.grab().save(str(out/'calculator-native.png'))
    frames=c.frames
    QTest.keyClick(window.screen_widget,Qt.Key.Key_Tab)
    wait(lambda:state()['page']=='history');wait(lambda:c.frames>frames)
    settle()
    assert window.grab().save(str(out/'history-native.png'))
    QTest.keyClick(window.screen_widget,Qt.Key.Key_Tab)
    wait(lambda:state()['page']=='notes')
    before=state()['note_length']
    QTest.keyClicks(window.screen_widget,' Native typing works.',delay=35)
    wait(lambda:state()['note_length']==before+21);frames=c.frames;wait(lambda:c.frames>frames)
    settle()
    assert window.grab().save(str(out/'notes-native.png'))
    assert not errors,errors
    window.close();print('PASS: Qt key events calculate 42, navigate all pages, edit notes; live screenshots saved.')
finally:
    c.stop();shutdown_all()
