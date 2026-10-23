// Setup page for a routeOS board. Everything it shows comes from the board's own API; this file only asks and renders.
const $ = (id) => document.getElementById(id);
const status = (text) => {
    const line = $('paper-status');
    if (!line) return;
    line.textContent = '';
    requestAnimationFrame(() => { line.textContent = text; });
};

const store = {
    get token() { return localStorage.getItem('routeos-token') || ''; },
    set token(value) { localStorage.setItem('routeos-token', value); },
};

async function call(method, path, body, needsToken = true) {
    const headers = {};
    if (needsToken) headers.Authorization = `Bearer ${store.token}`;
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const response = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
    const text = await response.text();
    let json = null;
    try { json = text ? JSON.parse(text) : null; } catch { /* not JSON: keep the text */ }
    if (!response.ok) {
        const message = (json && json.error) || text || `HTTP ${response.status}`;
        const error = new Error(message);
        error.status = response.status;
        throw error;
    }
    return json ?? text;
}

function badge(ok, text) {
    return `<span class="badge badge--${ok ? 'ok' : 'warn'}">${text}</span>`;
}

function rssiText(rssi) {
    if (!rssi) return '—';
    const bars = rssi > -55 ? 'excellent' : rssi > -65 ? 'good' : rssi > -75 ? 'fair' : 'weak';
    return `${rssi} dBm · ${bars}`;
}

async function loadWifi() {
    const wifi = await call('GET', '/api/wifi');
    $('stLink').innerHTML = wifi.connected ? badge(true, 'connected') : badge(false, 'not connected');
    $('stSsid').textContent = wifi.ssid || wifi.saved_ssid || wifi.builtin_ssid || 'none';
    $('stIp').textContent = wifi.ip || '—';
    $('stRssi').textContent = rssiText(wifi.rssi);
    $('stChannel').textContent = wifi.channel || '—';
    $('stMac').textContent = wifi.mac || '—';
    $('stSource').textContent = wifi.source || '—';
    return wifi;
}

async function loadBoard() {
    const caps = await call('GET', '/api/capabilities', undefined, false);
    const device = caps.device || {};
    const memory = caps.memory || {};
    $('boardTitle').textContent = device.chip || 'board';
    $('bdChip').textContent = `${device.chip || '—'} rev ${device.revision ?? '—'} · ${device.cores || '—'} cores at ${device.cpu_mhz || '—'} MHz`;
    $('bdFw').textContent = `${(caps.firmware && caps.firmware.name) || '—'} ${(caps.firmware && caps.firmware.version) || ''}`.trim();
    $('bdFlash').textContent = device.flash_mb ? `${device.flash_mb} MB` : '—';
    $('bdPsram').textContent = device.psram_kb ? `${Math.round(device.psram_kb / 1024)} MB` : 'none';
    $('bdHeap').textContent = memory.heap_free_kb ? `${memory.heap_free_kb} KB free of ${memory.heap_total_kb} KB` : '—';
    $('bdUptime').textContent = (caps.uptime_s !== undefined) ? `${Math.round(caps.uptime_s)} s` : '—';
    return caps;
}

function showLocked() {
    $('stLink').innerHTML = badge(false, 'locked');
    $('stSsid').textContent = 'enter the token to read this';
    for (const id of ['stIp', 'stRssi', 'stChannel', 'stMac', 'stSource']) $(id).textContent = '—';
}

let unlocked = false;

async function unlock(quiet = false) {
    if (!store.token) {
        if (!quiet) $('unlockState').textContent = 'no token';
        return false;
    }
    try {
        await call('GET', '/api/system');
        unlocked = true;
        $('unlockState').textContent = 'unlocked';
        status('Token accepted. Showing the board.');
        loadWifi().catch(() => {});
        return true;
    } catch (error) {
        unlocked = false;
        showLocked();
        $('unlockState').textContent = error.status === 401 ? 'token rejected' : error.message;
        if (!quiet) status('The board did not accept that token.');
        return false;
    }
}

async function scan() {
    $('scanState').textContent = 'scanning…';
    $('networks').innerHTML = '';
    try {
        await call('POST', '/api/wifi/scan');
        for (let i = 0; i < 40; i++) {
            const result = await call('GET', '/api/wifi/scan');
            if (!result.running) {
                renderNetworks(result.networks || []);
                $('scanState').textContent = `${(result.networks || []).length} found`;
                return;
            }
            await new Promise((resolve) => setTimeout(resolve, 700));
        }
        $('scanState').textContent = 'scan timed out';
    } catch (error) {
        $('scanState').textContent = error.message;
    }
}

function renderNetworks(networks) {
    const list = $('networks');
    list.innerHTML = '';
    if (!networks.length) {
        list.innerHTML = '<li class="hint">No networks reported. Move the board closer to the router.</li>';
        return;
    }
    for (const network of networks) {
        const item = document.createElement('li');
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'btn btn--sm';
        button.style.width = '100%';
        button.style.justifyContent = 'space-between';
        button.innerHTML = `<span>${network.ssid || '(hidden)'}</span>
            <span class="mono">${network.rssi} dBm${network.connected ? ' · connected' : ''}</span>`;
        button.addEventListener('click', () => {
            $('ssid').value = network.ssid || '';
            $('password').focus();
            status(`Selected ${network.ssid}.`);
        });
        if (!network.supported) {
            button.disabled = true;
            button.title = 'This board cannot join that network type.';
        }
        item.appendChild(button);
        list.appendChild(item);
    }
}

async function join() {
    const ssid = $('ssid').value.trim();
    if (!ssid) { status('Type or pick a network name first.'); return; }
    $('joinHint').textContent = 'Sending the new network…';
    try {
        await call('PUT', '/api/wifi', { ssid, password: $('password').value });
        $('joinHint').textContent = 'Trying it now. The board falls back to the old network if this one fails.';
        status(`Trying ${ssid}.`);
        for (let i = 0; i < 25; i++) {
            await new Promise((resolve) => setTimeout(resolve, 1000));
            try {
                const wifi = await loadWifi();
                if (wifi.connected && wifi.ssid === ssid) {
                    $('joinHint').textContent = `Joined ${ssid} — the board is at ${wifi.ip}.`;
                    status('Joined the new network.');
                    return;
                }
            } catch { /* the radio is restarting, the page will come back */ }
        }
        $('joinHint').textContent = 'No confirmation yet. Reload the page to see what happened.';
    } catch (error) {
        $('joinHint').textContent = error.message;
        status('The board refused the change.');
    }
}

async function test() {
    $('testState').textContent = 'running…';
    $('testResult').hidden = true;
    try {
        const result = await call('POST', '/api/test');
        $('testState').textContent = `${result.ms} ms`;
        $('testResult').hidden = false;
        $('testResult').className = 'banner ' + (result.ok ? 'banner--ok' : 'banner--err');
        $('testText').innerHTML = result.ok
            ? `Reached <span class="mono">https://yeems214.xyz</span> — HTTP ${result.status} in ${result.ms} ms, TLS ${result.tls}.`
            : `Could not reach <span class="mono">https://yeems214.xyz</span> after ${result.ms} ms: ${result.error}.`;
        status(result.ok ? 'The link test passed.' : 'The link test failed.');
    } catch (error) {
        $('testState').textContent = 'failed';
        $('testResult').hidden = false;
        $('testResult').className = 'banner banner--err';
        $('testText').textContent = error.message;
    }
}

$('unlockBtn').addEventListener('click', async () => {
    store.token = $('token').value.trim();
    await unlock();
});
$('token').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') $('unlockBtn').click();
});
$('refreshBtn').addEventListener('click', () => loadWifi().catch((error) => status(error.message)));
$('scanBtn').addEventListener('click', scan);
$('joinBtn').addEventListener('click', join);
$('testBtn').addEventListener('click', test);
$('rebootBtn').addEventListener('click', async () => {
    try {
        await call('POST', '/api/reboot');
        status('The board is restarting.');
        $('rebootBtn').disabled = true;
    } catch (error) {
        status(error.message);
    }
});

// Fill everything that needs no token first, so the page is useful before the token is typed in.
$('year').textContent = new Date().getFullYear();
loadBoard().catch(() => {});
$('token').value = store.token;
unlock(true).then((ok) => { if (!ok) showLocked(); });
