/* Auto-generated from web/index.html.  Do not edit by hand.
 * Regenerate via: tools/embed_html.py                       */
static const char WEB_UI_HTML[] = R"FSUAEWEBUI(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>fs-uae-rpc debugger</title>
<style>
:root {
    --bg: #0a0e14;
    --bg2: #11161e;
    --bg3: #1a1f2a;
    --fg: #d8dee9;
    --dim: #6a737d;
    --accent: #ffb454;
    --good: #aad94c;
    --bad: #ff6b6b;
    --hilite: #59c2ff;
    --mono: 'SF Mono', Menlo, Consolas, monospace;
}
* { box-sizing: border-box; }
body {
    margin: 0;
    padding: 0;
    background: var(--bg);
    color: var(--fg);
    font: 13px var(--mono);
    height: 100vh;
    overflow: hidden;
}
header {
    background: var(--bg2);
    border-bottom: 1px solid var(--bg3);
    padding: 6px 12px;
    display: flex;
    align-items: center;
    gap: 12px;
}
header h1 { margin: 0; font-size: 14px; color: var(--accent); font-weight: 600; }
.state {
    padding: 2px 8px;
    border-radius: 3px;
    font-weight: bold;
    font-size: 11px;
    text-transform: uppercase;
}
.state.running { background: var(--good); color: #000; }
.state.paused  { background: var(--bad);  color: #000; }
.state.dead    { background: var(--dim);  color: #fff; }
.buttons { margin-left: auto; display: flex; gap: 4px; }
button {
    background: var(--bg3);
    color: var(--fg);
    border: 1px solid var(--dim);
    padding: 4px 10px;
    font: 12px var(--mono);
    border-radius: 3px;
    cursor: pointer;
}
button:hover { background: var(--dim); color: #000; }
button:disabled { opacity: 0.4; cursor: not-allowed; }
main {
    display: grid;
    grid-template-columns: 320px 1fr 360px;
    grid-template-rows: 1fr 1fr;
    gap: 1px;
    background: var(--bg3);
    height: calc(100vh - 36px);
}
section {
    background: var(--bg);
    padding: 6px 10px;
    overflow: auto;
}
section h2 {
    margin: 0 0 6px 0;
    font-size: 11px;
    color: var(--accent);
    text-transform: uppercase;
    letter-spacing: 1px;
    font-weight: 600;
}
.regs { display: grid; grid-template-columns: auto 1fr; gap: 1px 8px; font-size: 12px; }
.regs .label { color: var(--dim); }
.regs .value { color: var(--fg); }
.regs .value.hilite { color: var(--hilite); }
.disasm-line { padding: 1px 4px; display: flex; gap: 8px; }
.disasm-line.current { background: var(--bg3); border-left: 2px solid var(--accent); }
.disasm-line .addr { color: var(--accent); width: 80px; }
.disasm-line .bytes { color: var(--dim); width: 100px; }
.disasm-line .mnem { color: var(--fg); flex: 1; }
.disasm-line .sym { color: var(--hilite); font-style: italic; padding-left: 4px; }
.mem-line { display: flex; gap: 8px; font-size: 12px; }
.mem-line .addr { color: var(--accent); width: 84px; }
.mem-line .bytes { color: var(--fg); width: 280px; }
.mem-line .ascii { color: var(--dim); }
.controls { display: flex; gap: 4px; margin-bottom: 8px; flex-wrap: wrap; }
.controls input {
    background: var(--bg2);
    color: var(--fg);
    border: 1px solid var(--dim);
    padding: 3px 6px;
    font: 12px var(--mono);
    border-radius: 3px;
    width: 100px;
}
.custom-row { display: flex; gap: 6px; padding: 1px 0; }
.custom-row .name { color: var(--accent); width: 90px; }
.custom-row .val { color: var(--fg); }
.bplist {}
.bp { display: flex; gap: 6px; padding: 2px 0; align-items: center; }
.bp .addr { color: var(--accent); flex: 1; }
.bp .del { color: var(--bad); cursor: pointer; }
#log {
    grid-column: 1 / 4;
    background: var(--bg2);
    color: var(--dim);
    font-size: 11px;
    padding: 4px 10px;
    overflow: auto;
    border-top: 1px solid var(--bg3);
    max-height: 100px;
}
.log-entry { padding: 1px 0; }
.log-entry .ts { color: var(--dim); }
.log-entry.err { color: var(--bad); }
.log-entry.ok { color: var(--good); }
</style>
</head>
<body>
<header>
    <h1>fs-uae-rpc</h1>
    <span id="state" class="state running">running</span>
    <span style="color: var(--dim); font-size: 11px;">PC: <b id="hdr-pc" style="color: var(--accent);">--</b></span>
    <div class="buttons">
        <button id="btn-pause">⏸ Pause</button>
        <button id="btn-resume">▶ Resume</button>
        <button id="btn-step">⤳ Step</button>
        <button id="btn-step100">⤳ ×100</button>
        <button id="btn-reset">↻ Reset</button>
    </div>
</header>

<main>
    <section>
        <h2>CPU registers</h2>
        <div id="regs" class="regs"></div>
    </section>
    <section style="grid-row: span 2;">
        <h2>Disassembly &mdash; <input id="disasm-addr" type="text" placeholder="addr (or 'pc')" style="background: var(--bg2); color: var(--fg); border: 1px solid var(--dim); padding: 2px 6px; font: 12px var(--mono); width: 110px;"> <button id="disasm-go" style="padding: 2px 8px;">Go</button></h2>
        <div id="disasm"></div>
    </section>
    <section>
        <h2>Chipset state</h2>
        <div id="custom"></div>
    </section>
    <section>
        <h2>Memory &mdash; <input id="mem-addr" type="text" value="0x0" style="background: var(--bg2); color: var(--fg); border: 1px solid var(--dim); padding: 2px 6px; font: 12px var(--mono); width: 110px;"> <button id="mem-go" style="padding: 2px 8px;">Go</button></h2>
        <div id="mem"></div>
    </section>
    <section>
        <h2>Breakpoints</h2>
        <div class="controls">
            <input id="bp-addr" type="text" placeholder="0xFC0234">
            <button id="bp-add">+ BP</button>
            <button id="bp-clear">Clear</button>
        </div>
        <div id="bplist" class="bplist"></div>
        <h2 style="margin-top: 12px;">Watchpoints</h2>
        <div class="controls">
            <input id="wp-addr" type="text" placeholder="0xC0">
            <input id="wp-size" type="text" placeholder="size" style="width: 50px;" value="4">
            <select id="wp-rwi" style="background: var(--bg2); color: var(--fg); border: 1px solid var(--dim); padding: 2px;">
                <option value="W">W</option>
                <option value="R">R</option>
                <option value="RW">RW</option>
            </select>
            <button id="wp-add">+ WP</button>
            <button id="wp-clear">Clear</button>
        </div>
        <div id="wplist" class="bplist"></div>
        <div id="wp-last" style="color: var(--hilite); margin-top: 6px; font-size: 11px;"></div>
    </section>
</main>
<div id="log"></div>

<script>
const $ = s => document.querySelector(s);
const log = (msg, cls='') => {
    const el = $('#log');
    const t = new Date().toLocaleTimeString();
    el.insertAdjacentHTML('afterbegin',
        `<div class="log-entry ${cls}"><span class="ts">${t}</span> ${msg}</div>`);
    while (el.children.length > 30) el.removeChild(el.lastChild);
};

let lastCpuState = {};
let symbolTable = {};

async function api(method, path, opts={}) {
    const res = await fetch(path, {method, ...opts});
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    return res.json();
}

async function refresh() {
    try {
        const state = await api('GET', '/v1/state');
        const cpu = await api('GET', '/v1/cpu');
        const custom = await api('GET', '/v1/custom');
        renderState(state, cpu);
        renderRegs(cpu);
        renderCustom(custom);
        if (state.state === 'paused') {
            try {
                const disasm = await api('GET', `/v1/disasm?addr=${cpu.pc}&count=20&annotate=1`);
                renderDisasm(disasm, cpu.pc);
            } catch(e) {}
        }
        await refreshBpWp();
    } catch (e) {
        $('#state').textContent = 'dead';
        $('#state').className = 'state dead';
        log(`refresh failed: ${e.message}`, 'err');
    }
}

function renderState(state, cpu) {
    const el = $('#state');
    el.textContent = state.state;
    el.className = `state ${state.state}`;
    $('#hdr-pc').textContent = cpu.pc;
}

function renderRegs(cpu) {
    const hilite = (k, v) => lastCpuState[k] && lastCpuState[k] !== v ? 'hilite' : '';
    const rows = [
        ['PC', cpu.pc], ['SR', cpu.sr],
        ['USP', cpu.usp], ['ISP', cpu.isp],
        ['D0', cpu.d0], ['D1', cpu.d1],
        ['D2', cpu.d2], ['D3', cpu.d3],
        ['D4', cpu.d4], ['D5', cpu.d5],
        ['D6', cpu.d6], ['D7', cpu.d7],
        ['A0', cpu.a0], ['A1', cpu.a1],
        ['A2', cpu.a2], ['A3', cpu.a3],
        ['A4', cpu.a4], ['A5', cpu.a5],
        ['A6', cpu.a6], ['A7', cpu.a7],
    ];
    $('#regs').innerHTML = rows.map(([k, v]) =>
        `<div class="label">${k}</div><div class="value ${hilite(k.toLowerCase(), v)}">${v}</div>`
    ).join('');
    lastCpuState = {pc: cpu.pc, sr: cpu.sr, usp: cpu.usp, isp: cpu.isp,
        d0: cpu.d0, d1: cpu.d1, d2: cpu.d2, d3: cpu.d3,
        d4: cpu.d4, d5: cpu.d5, d6: cpu.d6, d7: cpu.d7,
        a0: cpu.a0, a1: cpu.a1, a2: cpu.a2, a3: cpu.a3,
        a4: cpu.a4, a5: cpu.a5, a6: cpu.a6, a7: cpu.a7};
}

function renderDisasm(disasm, currentPc) {
    const html = disasm.lines.map(line => {
        // The line may already contain an "; exec.Func()" suffix from
        // the server-side annotator.  Split it off so we render the
        // mnemonic and the annotation separately.
        let annot = '';
        const annIdx = line.indexOf('; exec.');
        if (annIdx >= 0) {
            annot = line.slice(annIdx);
            line = line.slice(0, annIdx).replace(/\s+$/, '');
        }
        // Parse "ADDR HEXBYTES MNEMONIC" pattern
        const m = line.match(/^([0-9A-F]+)\s+([0-9a-f\s]+?)\s+(.+)$/);
        let addr = '?', bytes = '', mnem = line;
        if (m) {
            addr = '0x' + m[1].toLowerCase();
            bytes = m[2];
            mnem = m[3];
        }
        const cls = addr === currentPc ? 'disasm-line current' : 'disasm-line';
        // Address-table annotation: if mnem references a known addr,
        // highlight it.  (Used in addition to the FD annotation.)
        let sym = annot;
        if (!sym) {
            const symMatch = mnem.match(/\$([0-9a-fA-F]{1,8})/);
            if (symMatch) {
                const key = '0x' + symMatch[1].toLowerCase().padStart(8, '0');
                if (symbolTable[key]) sym = `; ${symbolTable[key]}`;
            }
        }
        return `<div class="${cls}"><span class="addr">${addr}</span>` +
               `<span class="bytes">${bytes}</span>` +
               `<span class="mnem">${mnem}</span>` +
               `<span class="sym">${sym}</span></div>`;
    }).join('');
    $('#disasm').innerHTML = html;
}

function renderCustom(c) {
    const rows = [
        ['DMACON', c.dmacon],   ['INTENA', c.intena],
        ['INTREQ', c.intreq],   ['BPLCON0', c.bplcon0],
        ['VPOSR', c.vposr],     ['VHPOSR', c.vhposr],
        ['ADKCONR', c.adkconr], [' ', ' '],
        ['DIWSTRT', c.diwstrt], ['DIWSTOP', c.diwstop],
        ['DDFSTRT', c.ddfstrt], ['DDFSTOP', c.ddfstop],
        ['COP1LC', c.cop1lc],   ['COP2LC', c.cop2lc],
    ];
    const dmaBits = decodeDmacon(parseInt(c.dmacon, 16));
    let html = rows.map(([n, v]) =>
        `<div class="custom-row"><span class="name">${n}</span><span class="val">${v}</span></div>`).join('');
    html += `<div class="custom-row" style="margin-top: 4px;"><span style="color: var(--dim);">DMA:</span> <span style="color: var(--hilite);">${dmaBits}</span></div>`;
    html += `<div class="custom-row"><span class="name">BPLPT</span><span class="val">${(c.bplpt||[]).slice(0,3).join(' ')}</span></div>`;
    $('#custom').innerHTML = html;
}

function decodeDmacon(v) {
    const bits = ['AUD0', 'AUD1', 'AUD2', 'AUD3', 'DSK', 'SPR', 'BLT', 'COP', 'BPL', 'DMAEN', 'BLTPRI'];
    return bits.map((b, i) => v & (1 << i) ? b : null).filter(Boolean).join(' ') || '(all off)';
}

async function refreshMem() {
    const addr = $('#mem-addr').value || '0x0';
    try {
        const r = await api('GET', `/v1/mem?addr=${addr}&len=256`);
        const bytes = r.hex;
        const lines = [];
        for (let i = 0; i < bytes.length; i += 32) {
            const lineAddr = (parseInt(r.addr, 16) + i/2).toString(16).padStart(8, '0');
            const hex = bytes.substr(i, 32).match(/../g).join(' ');
            const ascii = bytes.substr(i, 32).match(/../g).map(b => {
                const c = parseInt(b, 16);
                return c >= 32 && c < 127 ? String.fromCharCode(c) : '.';
            }).join('');
            lines.push(`<div class="mem-line"><span class="addr">0x${lineAddr}</span>` +
                       `<span class="bytes">${hex}</span>` +
                       `<span class="ascii">${ascii}</span></div>`);
        }
        $('#mem').innerHTML = lines.join('');
    } catch (e) {
        $('#mem').innerHTML = `<div style="color: var(--bad);">${e.message}</div>`;
    }
}

async function refreshBpWp() {
    try {
        const bps = await api('GET', '/v1/breakpoints');
        $('#bplist').innerHTML = bps.breakpoints.map(b =>
            `<div class="bp"><span class="addr">${b.addr}</span>` +
            `<span class="del" data-addr="${b.addr}" data-kind="bp">×</span></div>`).join('') || '<div style="color: var(--dim);">(none)</div>';
        const wps = await api('GET', '/v1/watchpoints');
        $('#wplist').innerHTML = wps.watchpoints.map(w =>
            `<div class="bp"><span class="addr">${w.addr} size=${w.size}</span>` +
            `<span class="del" data-kind="wp">×</span></div>`).join('') || '<div style="color: var(--dim);">(none)</div>';
        const last = await api('GET', '/v1/watchpoints/last');
        $('#wp-last').innerHTML = last.hit
            ? `<b>Last WP hit:</b> addr=${last.addr} pc=${last.pc} rwi=${last.rwi} val=${last.value}`
            : '';
    } catch (e) {}
}

async function loadSymbols() {
    try {
        const r = await api('GET', '/v1/symbols');
        r.symbols.forEach(s => symbolTable[s.addr] = s.name);
        log(`loaded ${r.symbols.length} symbols`, 'ok');
    } catch (e) {
        log('symbol load failed', 'err');
    }
}

// Event wiring
$('#btn-pause').onclick = async () => { await api('POST', '/v1/pause'); refresh(); };
$('#btn-resume').onclick = async () => { await api('POST', '/v1/resume'); refresh(); };
$('#btn-step').onclick = async () => { await api('POST', '/v1/step?n=1'); setTimeout(refresh, 50); };
$('#btn-step100').onclick = async () => { await api('POST', '/v1/step?n=100'); setTimeout(refresh, 100); };
$('#btn-reset').onclick = async () => { if (confirm('Hard reset?')) { await api('POST', '/v1/reset?hard=1'); setTimeout(refresh, 200); } };
$('#mem-go').onclick = refreshMem;
$('#mem-addr').onkeydown = e => { if (e.key === 'Enter') refreshMem(); };
$('#disasm-go').onclick = async () => {
    const a = $('#disasm-addr').value;
    if (!a) return;
    const addr = (a === 'pc') ? lastCpuState.pc : a;
    const d = await api('GET', `/v1/disasm?addr=${addr}&count=20&annotate=1`);
    renderDisasm(d, lastCpuState.pc);
};
$('#bp-add').onclick = async () => {
    const a = $('#bp-addr').value;
    if (!a) return;
    await api('POST', `/v1/breakpoints?addr=${a}`);
    $('#bp-addr').value = '';
    refreshBpWp();
};
$('#bp-clear').onclick = async () => { await api('POST', '/v1/breakpoints/clear'); refreshBpWp(); };
$('#wp-add').onclick = async () => {
    const a = $('#wp-addr').value;
    const sz = $('#wp-size').value || '1';
    const rwi = $('#wp-rwi').value;
    if (!a) return;
    await api('POST', `/v1/watchpoints?addr=${a}&size=${sz}&rwi=${rwi}&mustchange=1`);
    $('#wp-addr').value = '';
    refreshBpWp();
};
$('#wp-clear').onclick = async () => { await api('POST', '/v1/watchpoints/clear'); refreshBpWp(); };

// Delete buttons via event delegation
document.body.onclick = async (e) => {
    if (e.target.classList.contains('del')) {
        const kind = e.target.dataset.kind;
        if (kind === 'bp') { await api('POST', '/v1/breakpoints/clear'); refreshBpWp(); }
        else if (kind === 'wp') { await api('POST', '/v1/watchpoints/clear'); refreshBpWp(); }
    }
};

// Boot — try a WebSocket first, fall back to polling.
function connectWS() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(`${proto}//${location.host}/v1/events`);
    ws.onopen = () => log('WebSocket connected', 'ok');
    ws.onmessage = ev => {
        try {
            const m = JSON.parse(ev.data);
            log(`event: ${JSON.stringify(m)}`, 'ok');
            // Any state-changing event triggers a refresh.
            if (m.event === 'paused' || m.event === 'running' || m.event === 'wp_hit') {
                refresh();
                if (m.event === 'wp_hit') refreshBpWp();
            }
        } catch(e) { log(`bad ws frame: ${ev.data}`, 'err'); }
    };
    ws.onerror = () => log('WebSocket error — falling back to polling', 'err');
    ws.onclose = () => { log('WebSocket closed; retrying in 3 s', 'err'); setTimeout(connectWS, 3000); };
}

loadSymbols();
refresh();
refreshMem();
connectWS();
// Polling fallback at slower cadence (in case ws delivery fails silently)
setInterval(refresh, 2000);
log('debugger frontend loaded', 'ok');
</script>
</body>
</html>
)FSUAEWEBUI";
