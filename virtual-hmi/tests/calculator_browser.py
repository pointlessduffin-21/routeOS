"""Real browser keyboard, pointer, page, and arithmetic acceptance on the ESP32."""
import argparse
from pathlib import Path
from playwright.sync_api import sync_playwright, expect

parser = argparse.ArgumentParser()
parser.add_argument('--host', default='192.168.254.123')
args = parser.parse_args()
out = Path('build/calculator');out.mkdir(parents=True, exist_ok=True)
with sync_playwright() as p:
    browser=p.chromium.launch(channel='msedge',headless=True)
    page=browser.new_page(viewport={'width':1000,'height':850})
    errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(f'http://{args.host}/hmi')
    expect(page.locator('#status')).to_contain_text('Connected',timeout=20000)
    page.locator('#control').check()
    expect(page.locator('#status')).to_contain_text('keyboard ready',timeout=20000)
    canvas=page.locator('canvas');canvas.focus()
    def state():return page.request.get(f'http://{args.host}/health').json()
    def wait(check):
        for _ in range(80):
            current=state()
            if check(current):return current
            page.wait_for_timeout(50)
        raise AssertionError(current)
    def type_expression(text,result):
        canvas.focus();page.keyboard.press('Escape')
        page.keyboard.type(text,delay=25);page.keyboard.press('Enter')
        s=wait(lambda s:s['result']==result)
        assert s['page']=='calculator',s
        print(text,'=',s['result'],flush=True)
    for text,result in [('12+7*3','33'),('(10-2)/4','2'),('50%*200','100'),
                        ('2^3^2','512'),('-2^2','-4'),('2^-2','0.25'),
                        ('1/0','ERR'),('1e999','ERR'),('123.45+67.89','191.34')]:
        type_expression(text,result)
    page.wait_for_timeout(2200)
    page.screenshot(path=str(out/'calculator-browser.png'),full_page=True)
    page.keyboard.press('Tab');wait(lambda s:s['page']=='history');page.wait_for_timeout(2200)
    page.screenshot(path=str(out/'history-browser.png'),full_page=True)
    page.keyboard.press('Tab');wait(lambda s:s['page']=='notes')
    page.keyboard.press('End')
    for _ in range(state()['note_length']):
        page.keyboard.press('Backspace',delay=20)
    wait(lambda s:s['note_length']==0)
    page.keyboard.type('Made on an ESP32.',delay=40)
    page.keyboard.press('Enter')
    page.keyboard.type('Keyboard + touch!',delay=40)
    wait(lambda s:s['note_length']==35);page.wait_for_timeout(2200)
    page.screenshot(path=str(out/'notes-browser.png'),full_page=True)
    # Pointer navigation and the calculator button matrix, not keyboard shortcuts.
    def tap(x,y):
        box=canvas.bounding_box();page.mouse.click(box['x']+box['width']*x/240,box['y']+box['height']*y/135)
        page.wait_for_timeout(120)
    before=state()['note_length']
    tap(34,74) # QWERTY q: actual LVGL on-screen keyboard
    wait(lambda s:s['note_length']==before+1)
    canvas.focus();page.keyboard.press('Backspace')
    wait(lambda s:s['note_length']==before)
    tap(160,10);wait(lambda s:s['page']=='calculator')
    tap(128,34) # AC
    tap(129,55) # 7
    tap(220,97) # +
    tap(158,55) # 8
    tap(220,118) # =
    wait(lambda s:s['result']=='15')
    assert not errors,errors
    print('PASS: arithmetic, precedence, errors, physical keyboard, 3 pages and touch keypad.',flush=True)
    browser.close()
