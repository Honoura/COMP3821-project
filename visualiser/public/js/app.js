(function () {
  const $ = (id) => document.getElementById(id);
  const { LETTERS } = window.Tet;

  const state = {
    frames: [],
    index: 0,
    game: null,         // runner log, when showing a strategy run
    markers: [],
    pcLines: new Map(), // frame index -> lines cleared by that PC
    playing: false,
    source: null,
    sourceName: '',
    seqMode: 'seed',
    logs: { output: '', compile: '' },
    logTab: 'output',
    anim: { start: 0, dur: 0 },
    hover: null,
    online: false,
  };

  // ------------------------------------------------------------ settings ---
  const SETTINGS_KEY = 'pcvis.settings';
  const settingInputs = ['seed', 'length', 'cap', 'timeout', 'customQueue', 'speed'];
  function saveSettings() {
    try {
      const s = { seqMode: state.seqMode, hold: $('hold').checked };
      for (const id of settingInputs) s[id] = $(id).value;
      localStorage.setItem(SETTINGS_KEY, JSON.stringify(s));
    } catch (e) { /* storage unavailable */ }
  }
  function loadSettings() {
    try {
      const s = JSON.parse(localStorage.getItem(SETTINGS_KEY) || 'null');
      if (!s) return;
      for (const id of settingInputs) if (s[id] != null) $(id).value = s[id];
      $('hold').checked = s.hold !== false;
      setSeqMode(s.seqMode || 'seed');
    } catch (e) { /* ignore */ }
  }

  // --------------------------------------------------------------- toast ---
  let toastTimer;
  function toast(msg, bad = false) {
    const t = $('toast');
    t.textContent = msg;
    t.className = 'toast show' + (bad ? ' bad' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (t.className = 'toast' + (bad ? ' bad' : '')), 2600);
  }

  // -------------------------------------------------------------- server ---
  async function checkServer() {
    const el = $('serverStatus');
    try {
      const r = await fetch('api/health', { cache: 'no-store' });
      const j = await r.json();
      state.online = true;
      el.className = 'server-status online';
      el.querySelector('.label').textContent = j.cxxFound ? `Runner online · ${j.cxx}` : `Runner online · compiler "${j.cxx}" not found`;
    } catch (e) {
      state.online = false;
      el.className = 'server-status offline';
      el.querySelector('.label').textContent = 'Runner offline — start visualiser/server.py (fumen still works)';
    }
    updateRunButton();
  }

  function updateRunButton() {
    $('runBtn').disabled = !state.source || !state.online || $('runBtn').classList.contains('busy');
  }

  // ------------------------------------------------------------ strategy ---
  function setSource(text, name) {
    state.source = text;
    state.sourceName = name;
    const lines = text.split('\n').length;
    $('dropzone').classList.add('loaded');
    $('dzTitle').textContent = name;
    $('dzSub').textContent = `${lines} lines · click or drop to replace`;
    updateRunButton();
  }

  function readFile(file) {
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => setSource(String(reader.result), file.name);
    reader.onerror = () => toast('Could not read ' + file.name, true);
    reader.readAsText(file);
  }

  function setSeqMode(mode) {
    state.seqMode = mode;
    document.querySelectorAll('[data-seq]').forEach((b) => b.classList.toggle('active', b.dataset.seq === mode));
    $('seedField').classList.toggle('hidden', mode !== 'seed');
    $('customField').classList.toggle('hidden', mode !== 'custom');
  }

  async function run() {
    if (!state.source) return;
    const btn = $('runBtn');
    btn.classList.add('busy');
    btn.querySelector('.run-label').textContent = 'Compiling & running…';
    updateRunButton();
    stop();
    const body = {
      source: state.source,
      length: Number($('length').value) || 200,
      cap: Number($('cap').value) || 0,
      hold: $('hold').checked,
      timeout: Number($('timeout').value) || 60,
    };
    if (state.seqMode === 'custom') body.queue = $('customQueue').value;
    else body.seed = Number($('seed').value) || 0;
    saveSettings();

    try {
      const r = await fetch('api/run', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const res = await r.json();
      state.logs.compile = res.log || res.compileLog || '';
      state.logs.output = [res.stderr, res.stdout].filter(Boolean).join('\n');
      if (!res.ok) {
        const where = res.stage === 'compile' ? 'compile' : 'output';
        if (where === 'output') state.logs.output = `${res.message}\n\n${state.logs.output}`;
        else state.logs.compile = `${res.message}\n\n${state.logs.compile}`;
        setLogTab(where);
        toast(res.message, true);
      } else {
        setLogTab('output');
        loadGame(res.game, res);
        const g = res.game;
        toast(`${g.pcCount} perfect clear${g.pcCount === 1 ? '' : 's'} · ${g.placed}/${g.queue.length} pieces`);
      }
    } catch (e) {
      toast('Request failed: ' + e.message, true);
      checkServer();
    } finally {
      btn.classList.remove('busy');
      btn.querySelector('.run-label').textContent = 'Run strategy';
      updateRunButton();
    }
  }

  // --------------------------------------------------------------- loads ---
  function computeMarkers() {
    state.markers = [];
    state.pcLines = new Map();
    let lines = 0;
    state.frames.forEach((f, i) => {
      lines += f.clearRows ? f.clearRows.length : 0;
      if (f.isPC) {
        state.pcLines.set(i, lines);
        state.markers.push({ index: i, kind: lines === 2 ? 'l2' : lines === 4 ? 'l4' : 'other' });
        lines = 0;
      }
      if (f.error) state.markers.push({ index: i, kind: 'error' });
    });
  }

  function loadGame(game, meta = {}) {
    state.game = game;
    state.frames = Frames.fromGame(game);
    computeMarkers();
    const pcs = [...state.pcLines.values()];
    const count = (n) => pcs.filter((l) => l === n).length;
    $('statPCs').textContent = game.pcCount;
    $('statPlaced').textContent = `${game.placed} / ${game.queue.length}`;
    $('statRate').textContent = game.pcCount ? (game.placed / game.pcCount).toFixed(2) : '–';
    $('stat2').textContent = count(2);
    $('stat4').textContent = count(4);
    $('statOther').textContent = pcs.length - count(2) - count(4);
    $('statTime').textContent = formatMs(game.solveMs);

    const end = $('endReason');
    end.classList.remove('hidden');
    const kind = game.error ? 'bad' : game.endReason === 'sequence finished' ? 'ok' : 'warn';
    end.className = 'end-reason ' + kind;
    end.textContent = game.error
      ? `Stopped at piece ${game.error.move + 1}: ${game.error.message}`
      : `Ended: ${game.endReason}` + (game.seed != null ? ` · seed ${game.seed}` : '') +
        (game.rules.cap ? ` · cap ${game.rules.cap}` : '') + (game.rules.hold ? '' : ' · no hold');

    $('copyFumen').disabled = false;
    $('openFumen').classList.add('hidden');
    $('fumenError').classList.add('hidden');
    finishLoad();
    show(0, false);
  }

  function loadFumen(text) {
    let frames;
    try {
      frames = Frames.fromFumen(text);
      if (!frames.length) throw new Error('The fumen has no pages');
    } catch (e) {
      $('fumenError').textContent = e.message;
      $('fumenError').classList.remove('hidden');
      return false;
    }
    stop();
    $('fumenError').classList.add('hidden');
    state.game = null;
    state.frames = frames;
    computeMarkers();
    const pcs = [...state.pcLines.values()];
    $('statPCs').textContent = pcs.length;
    $('statPlaced').textContent = `${frames.filter((f) => f.piece && f.lock !== false).length} pieces`;
    $('statRate').textContent = '–';
    $('stat2').textContent = pcs.filter((l) => l === 2).length;
    $('stat4').textContent = pcs.filter((l) => l === 4).length;
    $('statOther').textContent = pcs.filter((l) => l !== 2 && l !== 4).length;
    $('statTime').textContent = '–';
    const end = $('endReason');
    end.className = 'end-reason';
    end.textContent = `Fumen · ${frames.length} page${frames.length === 1 ? '' : 's'}`;
    $('copyFumen').disabled = true;
    const code = /[vmd]11[05]@[A-Za-z0-9+/?]+/i.exec(text);
    if (code) {
      $('openFumen').href = 'https://fumen.zui.jp/?' + code[0];
      $('openFumen').classList.remove('hidden');
    }
    finishLoad();
    show(0, false);
    return true;
  }

  function finishLoad() {
    // Show only as many rows as the game ever uses (plus headroom).
    let top = 0;
    for (const f of state.frames) {
      top = Math.max(top, f.field.stackHeight());
      if (f.piece) for (const [, y] of Tet.pieceCells(f.piece.type, f.piece.rot, f.piece.x, f.piece.y)) top = Math.max(top, y + 1);
    }
    state.rows = Math.max(8, Math.min(23, top + 3));
    renderPCList();
    resize();
  }

  function renderPCList() {
    const list = $('pcList');
    list.innerHTML = '';
    if (!state.pcLines.size) {
      list.innerHTML = '<li class="empty">None yet</li>';
      return;
    }
    let n = 0;
    let prev = -1;
    for (const [i, lines] of state.pcLines) {
      n++;
      const pieces = i - prev;
      prev = i;
      const hold = state.game ? (state.game.pcs[n - 1] || {}).hold : '';
      const li = document.createElement('li');
      li.innerHTML = `<button type="button" data-frame="${i}">
        <span class="n">#${n}</span>
        <span class="d">piece ${i + 1} · ${pieces} pcs${hold ? ` · hold ${hold}` : ''}</span>
        <span class="h ${lines === 2 ? 'l2' : lines === 4 ? 'l4' : ''}">${lines}L</span></button>`;
      list.appendChild(li);
    }
  }

  // ------------------------------------------------------------- display ---
  function formatMs(ms) {
    if (ms == null) return '–';
    return ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(2)} s`;
  }

  function show(i, animate = true) {
    if (!state.frames.length) return;
    i = Math.max(0, Math.min(state.frames.length - 1, i));
    const forward = i === state.index + 1;
    state.index = i;
    const f = state.frames[i];

    const interval = 1000 / Number($('speed').value);
    state.anim = { start: performance.now(), dur: animate && forward ? Math.min(140, interval * 0.6) : 0 };

    $('pcBadge').textContent = `${f.pcTotal} PC${f.pcTotal === 1 ? '' : 's'}`;
    $('frameLabel').textContent = f.label;
    const chips = [];
    if (f.piece) chips.push(`<b>${LETTERS[f.piece.type]}</b> piece`);
    else if (f.error) chips.push('Rejected move');
    else chips.push(state.game ? 'Final board' : 'No piece');
    if (f.usedHold) chips.push('<span class="chip hold">hold</span>');
    if (f.clearRows && f.clearRows.length && !f.isPC) chips.push(`<span class="chip clear">${f.clearRows.length} line${f.clearRows.length > 1 ? 's' : ''}</span>`);
    if (f.isPC) chips.push(`<span class="chip pc">PC #${f.pcTotal} · ${state.pcLines.get(i)}L</span>`);
    $('frameSub').innerHTML = chips.join(' ');

    const comment = f.error ? '' : f.comment || '';
    $('frameComment').textContent = comment;
    $('frameComment').classList.toggle('hidden', !comment);

    const banner = $('banner');
    if (f.error) {
      banner.textContent = f.error;
      banner.className = 'banner error show';
    } else if (f.isPC) {
      banner.textContent = `${state.pcLines.get(i)}-LINE PERFECT CLEAR`;
      banner.className = 'banner show';
    } else {
      banner.className = 'banner';
    }

    Render.drawHold($('holdCanvas'), f.hold, false);
    Render.drawNext($('nextCanvas'), f.next || []);
    document.querySelectorAll('.pc-list button').forEach((b) => {
      b.classList.toggle('current', Number(b.dataset.frame) === i);
    });
    drawFrame();
    drawTimeline();
  }

  function drawFrame() {
    const f = state.frames[state.index];
    const t = state.anim.dur ? Math.min(1, (performance.now() - state.anim.start) / state.anim.dur) : 1;
    const pulse = 0.5 + 0.5 * Math.sin(performance.now() / 180);
    Render.drawBoard($('board'), f, { rows: state.rows || 20, cell: state.cell || 30, dropT: t, pulse });
  }

  function drawTimeline() {
    state.timeline = Render.drawTimeline($('timeline'), state.frames.length, state.index, state.markers, state.hover);
  }

  function resize() {
    const rows = state.rows || 20;
    const narrow = window.matchMedia('(max-width: 560px)').matches;
    const stageW = document.querySelector('.stage').clientWidth - (narrow ? 28 : 40);
    const sideW = narrow ? 0 : 2 * (96 + 18);
    const byWidth = Math.floor((stageW - sideW) / 10);
    const byHeight = Math.floor(Math.min(620, window.innerHeight - 330) / rows);
    state.cell = Math.max(16, Math.min(40, byWidth, Math.max(22, byHeight)));
    if (state.frames.length) show(state.index, false);
    else Render.drawBoard($('board'), null, { rows, cell: state.cell });
    drawTimeline();
  }

  // ------------------------------------------------------------ playback ---
  let lastTick = 0;
  function loop(now) {
    if (state.playing) {
      const interval = 1000 / Number($('speed').value);
      if (now - lastTick >= interval) {
        lastTick = now;
        if (state.index >= state.frames.length - 1) stop();
        else show(state.index + 1);
      }
    }
    const f = state.frames[state.index];
    const animating = state.anim.dur && now - state.anim.start < state.anim.dur + 20;
    if (f && (animating || (f.clearRows && f.clearRows.length))) drawFrame();
    requestAnimationFrame(loop);
  }

  function play() {
    if (!state.frames.length) return;
    if (state.index >= state.frames.length - 1) show(0, false);
    state.playing = true;
    lastTick = performance.now();
    document.querySelector('.transport').classList.add('playing');
  }
  function stop() {
    state.playing = false;
    document.querySelector('.transport').classList.remove('playing');
  }
  function toggle() { state.playing ? stop() : play(); }

  function jumpPC(dir) {
    const pcs = [...state.pcLines.keys()];
    const target = dir > 0 ? pcs.find((i) => i > state.index) : pcs.reverse().find((i) => i < state.index);
    if (target != null) show(target, false);
  }

  const actions = {
    first: () => show(0, false),
    last: () => show(state.frames.length - 1, false),
    prev: () => show(state.index - 1, false),
    next: () => show(state.index + 1),
    play: toggle,
    prevPC: () => jumpPC(-1),
    nextPC: () => jumpPC(1),
  };

  // -------------------------------------------------------------- wiring ---
  function setLogTab(tab) {
    state.logTab = tab;
    document.querySelectorAll('[data-log]').forEach((b) => b.classList.toggle('active', b.dataset.log === tab));
    $('console').textContent = state.logs[tab] || (tab === 'compile' ? 'No compiler output.' : 'No output.');
  }

  function init() {
    loadSettings();
    checkServer();
    setInterval(() => { if (!state.online) checkServer(); }, 5000);

    const dz = $('dropzone');
    $('fileInput').addEventListener('change', (e) => readFile(e.target.files[0]));
    dz.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('fileInput').click(); } });
    ['dragenter', 'dragover'].forEach((t) => dz.addEventListener(t, (e) => { e.preventDefault(); dz.classList.add('drag'); }));
    ['dragleave', 'drop'].forEach((t) => dz.addEventListener(t, (e) => { e.preventDefault(); dz.classList.remove('drag'); }));
    dz.addEventListener('drop', (e) => readFile(e.dataTransfer.files[0]));
    // Dropping a file anywhere else shouldn't navigate away.
    window.addEventListener('dragover', (e) => e.preventDefault());
    window.addEventListener('drop', (e) => e.preventDefault());

    $('useExample').addEventListener('click', async () => {
      try {
        const r = await fetch('source/basic.cpp');
        setSource((await r.json()).source, 'basic.cpp');
      } catch (e) {
        toast('Start visualiser/server.py to load the example', true);
      }
    });

    document.querySelectorAll('[data-seq]').forEach((b) => b.addEventListener('click', () => { setSeqMode(b.dataset.seq); saveSettings(); }));
    $('randomSeed').addEventListener('click', () => { $('seed').value = Math.floor(Math.random() * 1e6); saveSettings(); });
    $('customQueue').addEventListener('input', () => {
      const n = ($('customQueue').value.toUpperCase().match(/[IOTLJSZ]/g) || []).length;
      $('customCount').textContent = `${n} piece${n === 1 ? '' : 's'}`;
    });
    $('hold').addEventListener('change', () => {
      $('hold').parentElement.querySelector('em').textContent = $('hold').checked ? 'Enabled' : 'Disabled';
    });
    $('hold').dispatchEvent(new Event('change'));
    $('customQueue').dispatchEvent(new Event('input'));
    settingInputs.forEach((id) => $(id).addEventListener('change', saveSettings));
    $('runBtn').addEventListener('click', run);

    $('loadFumen').addEventListener('click', () => loadFumen($('fumenInput').value));
    $('fumenInput').addEventListener('keydown', (e) => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) loadFumen($('fumenInput').value); });
    $('copyFumen').addEventListener('click', async () => {
      if (!state.game) return;
      const code = Frames.toFumen(state.game);
      $('openFumen').href = 'https://fumen.zui.jp/?' + code;
      $('openFumen').classList.remove('hidden');
      try {
        await navigator.clipboard.writeText(code);
        toast(`Copied fumen (${code.length} chars)`);
      } catch (e) {
        $('fumenInput').value = code;
        toast('Clipboard blocked — fumen placed in the text box');
      }
    });

    document.querySelectorAll('.transport [data-act]').forEach((b) => b.addEventListener('click', () => actions[b.dataset.act]()));
    $('speed').addEventListener('change', () => { lastTick = performance.now(); });
    $('pcList').addEventListener('click', (e) => {
      const b = e.target.closest('button[data-frame]');
      if (b) { stop(); show(Number(b.dataset.frame), false); }
    });
    document.querySelectorAll('[data-log]').forEach((b) => b.addEventListener('click', () => setLogTab(b.dataset.log)));

    const tl = $('timeline');
    let dragging = false;
    const seek = (e) => {
      if (!state.timeline || !state.frames.length) return;
      const rect = tl.getBoundingClientRect();
      show(state.timeline.indexAt(e.clientX - rect.left), false);
    };
    tl.addEventListener('pointerdown', (e) => { dragging = true; tl.setPointerCapture(e.pointerId); stop(); seek(e); });
    tl.addEventListener('pointermove', (e) => {
      if (dragging) return seek(e);
      if (!state.timeline || !state.frames.length) return;
      state.hover = state.timeline.indexAt(e.clientX - tl.getBoundingClientRect().left);
      tl.title = state.frames[state.hover].label;
      drawTimeline();
    });
    tl.addEventListener('pointerup', () => { dragging = false; });
    tl.addEventListener('pointerleave', () => { state.hover = null; drawTimeline(); });

    document.addEventListener('keydown', (e) => {
      if (e.target.closest('input, textarea, select')) return;
      const k = e.key;
      if (k === ' ') { e.preventDefault(); toggle(); }
      else if (k === 'ArrowRight') { e.preventDefault(); stop(); e.shiftKey ? jumpPC(1) : show(state.index + 1); }
      else if (k === 'ArrowLeft') { e.preventDefault(); stop(); e.shiftKey ? jumpPC(-1) : show(state.index - 1, false); }
      else if (k === 'Home') show(0, false);
      else if (k === 'End') show(state.frames.length - 1, false);
    });

    window.addEventListener('resize', resize);
    resize();
    Render.drawHold($('holdCanvas'), 0);
    Render.drawNext($('nextCanvas'), []);

    // Allow linking straight to a fumen: index.html#v115@... or ?v115@...
    const linked = /[vmd]11[05]@[A-Za-z0-9+/?]+/.exec(decodeURIComponent(location.hash + location.search));
    if (linked) {
      $('fumenInput').value = linked[0];
      loadFumen(linked[0]);
    }
    requestAnimationFrame(loop);
  }

  init();
})();
