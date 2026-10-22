"""Exercise the actual canvas, scaled pointer input, and reconnect in Edge headless."""
import argparse
from pathlib import Path
from playwright.sync_api import sync_playwright, expect

parser = argparse.ArgumentParser()
parser.add_argument('--url', default='http://127.0.0.1:8095/hmi')
parser.add_argument('--output', type=Path, default=Path('build/hmi-browser.png'))
parser.add_argument('--reloads', type=int, default=3)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
with sync_playwright() as p:
    browser = p.chromium.launch(channel='msedge', headless=True)
    page = browser.new_page(viewport={'width': 1000, 'height': 800})
    errors = []
    def error_seen(error):
        errors.append(str(error))
        print('Browser JS error:', error, flush=True)
    page.on('pageerror', error_seen)
    page.on('requestfailed', lambda request: print('Browser request failed:', request.url, request.failure, flush=True))
    page.on('console', lambda message: print('Browser console:', message.text, flush=True) if message.type == 'error' else None)
    page.goto(args.url)
    expect(page.locator('#status')).to_contain_text('Connected', timeout=20000)
    expect(page.locator('#status')).to_contain_text('view only')
    assert page.locator('canvas').evaluate('(c)=>[c.width,c.height]') == [240, 135]
    page.locator('#control').check()
    expect(page.locator('#status')).to_contain_text('pointer control granted', timeout=20000)
    box = page.locator('canvas').bounding_box()
    page.mouse.click(box['x'] + box['width'] * 0.5, box['y'] + box['height'] * (104 / 135))
    page.wait_for_timeout(500)  # allow the device's next LVGL refresh to show the click
    expect(page.locator('#metrics')).to_contain_text('complete batches')
    page.locator('#refresh').click()
    expect(page.locator('#status')).to_contain_text('Connected', timeout=20000)
    page.screenshot(path=str(args.output), full_page=True)
    for _ in range(args.reloads):
        page.reload()
        expect(page.locator('#status')).to_contain_text('Connected', timeout=20000)
    assert not errors, errors
    print('Browser: complete 240x135 screen, pointer grant, scaled click, refresh, reload/reconnect; no JS errors.')
    browser.close()
