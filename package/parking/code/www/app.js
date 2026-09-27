'use strict';

const $ = (s) => document.querySelector(s);
const api = async (method, url, body) => {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(url, opt);
  let data = null;
  try { data = await r.json(); } catch (e) { /* not json */ }
  if (!r.ok) throw new Error((data && data.error) || ('HTTP ' + r.status));
  return data;
};
const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// Spinner on the button while its request runs; ignores repeated clicks.
const working = async (btn, fn) => {
  if (btn.classList.contains('working')) return;
  btn.classList.add('working');
  try { await fn(); } finally { btn.classList.remove('working'); }
};

let msgTimer = 0;
function toast(text, err) {
  const m = $('#msg');
  m.textContent = text;
  m.className = 'msg' + (err ? ' err' : '');
  clearTimeout(msgTimer);
  msgTimer = setTimeout(() => m.classList.add('hidden'), err ? 8000 : 4000);
}

// ---------------------------------------------------------------- tabs
let currentTab = 'status';
document.querySelectorAll('nav button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));
function showTab(t) {
  currentTab = t;
  document.querySelectorAll('nav button').forEach((b) => b.classList.toggle('active', b.dataset.tab === t));
  document.querySelectorAll('main > section').forEach((s) => s.classList.toggle('hidden', s.id !== 'tab-' + t));
  if (t === 'spots') editor.open();
  if (t === 'photos') photos.load();
  if (t === 'net') net.status();
  if (t === 'settings') settings.load();
  if (t === 'models') models.load();
  if (t === 'videos') videos.load();
  try { localStorage.setItem('tab', t); } catch (e) { /* private mode */ }
}

// ---------------------------------------------------------------- drawing helpers
// point connections per PointShape (model.h): 2 plate, 3 hand, 4 COCO body, 5 OpenPose, 6 head axes
const SHAPE_EDGES = {
  2: [[0, 1], [1, 2], [2, 3], [3, 0]],
  3: [[0, 1], [1, 2], [2, 3], [3, 4], [0, 5], [5, 6], [6, 7], [7, 8], [0, 9], [9, 10], [10, 11], [11, 12],
    [0, 13], [13, 14], [14, 15], [15, 16], [0, 17], [17, 18], [18, 19], [19, 20]],
  4: [[15, 13], [13, 11], [16, 14], [14, 12], [11, 12], [5, 11], [6, 12], [5, 6], [5, 7], [6, 8], [7, 9], [8, 10],
    [1, 2], [0, 1], [0, 2], [1, 3], [2, 4], [3, 5], [4, 6]],
  5: [[1, 2], [1, 5], [2, 3], [3, 4], [5, 6], [6, 7], [1, 8], [8, 9], [9, 10], [1, 11], [11, 12], [12, 13], [1, 0],
    [0, 14], [14, 16], [0, 15], [15, 17]],
  6: [[0, 1], [0, 2], [0, 3]],
};
function drawPoints(ctx, w, h, pts, shape) {
  const ok = (k) => 2 * k + 1 < pts.length && pts[2 * k] >= 0;
  const P = (k) => [pts[2 * k] * w, pts[2 * k + 1] * h];
  const axis = ['#ef4444', '#22c55e', '#3b82f6'];
  (SHAPE_EDGES[shape] || []).forEach(([a, b], i) => {
    if (!ok(a) || !ok(b)) return;
    ctx.strokeStyle = shape === 6 ? axis[i % 3] : '#facc15';
    ctx.beginPath();
    ctx.moveTo(...P(a));
    ctx.lineTo(...P(b));
    ctx.stroke();
  });
  if (shape === 6) return;
  ctx.fillStyle = '#facc15';
  const r = Math.max(2, w / (pts.length > 60 ? 600 : 300));
  for (let k = 0; 2 * k + 1 < pts.length; k++) {
    if (!ok(k)) continue;
    ctx.beginPath();
    ctx.arc(...P(k), r, 0, 2 * Math.PI);
    ctx.fill();
  }
}

function drawSpots(ctx, w, h, spots, cam, opts = {}) {
  for (const s of spots) {
    if (s.cam !== cam || s.points.length < 2) continue;
    const busy = s.occupied;
    const col = opts.editor ? (s === opts.selected ? '#3b82f6' : '#f59e0b') : busy ? '#dc2626' : '#16a34a';
    ctx.beginPath();
    s.points.forEach(([x, y], i) => (i ? ctx.lineTo(x * w, y * h) : ctx.moveTo(x * w, y * h)));
    ctx.closePath();
    ctx.fillStyle = col + '44';
    ctx.fill();
    ctx.lineWidth = Math.max(2, w / 400);
    ctx.strokeStyle = col;
    ctx.stroke();
    const cx = s.points.reduce((a, p) => a + p[0], 0) / s.points.length * w;
    const cy = s.points.reduce((a, p) => a + p[1], 0) / s.points.length * h;
    ctx.font = `bold ${Math.max(12, w / 45)}px system-ui`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 3;
    ctx.strokeStyle = '#000';
    ctx.strokeText(s.id, cx, cy);
    ctx.fillStyle = '#fff';
    ctx.fillText(s.id, cx, cy);
    if (opts.editor && s === opts.selected) {
      for (const [x, y] of s.points) {
        ctx.beginPath();
        ctx.arc(x * w, y * h, Math.max(5, w / 150), 0, Math.PI * 2);
        ctx.fillStyle = '#fff';
        ctx.fill();
        ctx.strokeStyle = '#3b82f6';
        ctx.lineWidth = 2;
        ctx.stroke();
      }
    }
  }
}

// ---------------------------------------------------------------- status
const status = {
  data: null,
  camsBuilt: false,
  snapTimer: 0,
  build(cams) {
    const box = $('#status-cams');
    box.innerHTML = '';
    cams.forEach((c, i) => {
      if (!c.enabled) return;
      const d = document.createElement('div');
      d.className = 'cam';
      d.innerHTML = `<img id="st-img${i}" alt=""><canvas id="st-cv${i}"></canvas><div class="label" id="st-lbl${i}">Камера ${i}</div>`;
      box.appendChild(d);
    });
    this.camsBuilt = true;
  },
  refreshSnapshots() {
    if (currentTab !== 'status' || !this.data || document.hidden) return;
    this.data.cams.forEach((c, i) => {
      const img = $('#st-img' + i);
      if (!img || img.dataset.loading === '1') return;
      img.dataset.loading = '1';
      const next = new Image();
      next.onload = () => { img.src = next.src; img.dataset.loading = ''; };
      next.onerror = () => { img.dataset.loading = ''; };
      next.src = `/api/cam/${i}/snapshot.jpg?t=${Date.now()}`;
    });
  },
  render() {
    const d = this.data;
    if (!d) return;
    const spots = d.spots;
    const free = spots.filter((s) => !s.occupied).length;
    const ip = [d.eth0 && 'eth ' + d.eth0, d.wlan0 && 'wifi ' + d.wlan0].filter(Boolean).join(', ') || 'нет сети';
    const trig = d.mode === 'trigger';
    document.body.classList.toggle('mode-trigger', trig);
    $('#app-title').textContent = trig ? 'K510 Триггер' : 'K510 Парковка';
    $('#summary').textContent = `${d.time_str}${d.clock_ok ? '' : ' (время не синхронизировано)'} · ` +
      (trig ? `срабатываний ${d.trigger_count}` : `свободно ${free} из ${spots.length}`) + ` · ${ip} · SD свободно ${d.disk_free_pct}%`;
    if (trig) {
      const rec = d.recording_s >= 0 ? ` · <b>идёт запись ${d.recording_s} с</b>` : '';
      $('#trg-state').innerHTML = `Срабатываний: <b>${d.trigger_count}</b>` +
        (d.trigger_last ? ` · последнее: ${esc(d.trigger_last)}` : ' · пока не было') + rec +
        '<div class="hint">Фото с рамками — во вкладке «Фото», видео — во вкладке «Видео».</div>';
    }
    if (d.message && d.message !== this.lastMsg) toast(d.message);
    this.lastMsg = d.message;
    if (currentTab !== 'status') return;
    if (!this.camsBuilt) this.build(d.cams);
    d.cams.forEach((c, i) => {
      const cv = $('#st-cv' + i);
      if (!cv) return;
      const r = cv.getBoundingClientRect();
      cv.width = r.width * devicePixelRatio;
      cv.height = r.height * devicePixelRatio;
      const ctx = cv.getContext('2d');
      const w = cv.width, h = cv.height;
      drawSpots(ctx, w, h, spots, i);
      ctx.lineWidth = Math.max(2, w / 500);
      ctx.font = `${Math.max(11, w / 60)}px system-ui`;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      for (const det of c.dets) {
        const [x1, y1, x2, y2] = det.box;
        ctx.strokeStyle = '#facc15';
        ctx.strokeRect(x1 * w, y1 * h, (x2 - x1) * w, (y2 - y1) * h);
        ctx.fillStyle = '#facc15';
        ctx.fillText(`${det.name} ${det.score.toFixed(2)}`, x1 * w + 3, y1 * h + 3);
        if (det.text) ctx.fillText(det.text, x1 * w + 3, y2 * h + 3);
        if (det.pts) drawPoints(ctx, w, h, det.pts, det.shape);
      }
      $('#st-lbl' + i).textContent = `Камера ${i} · ${c.running ? c.fps.toFixed(1) + ' к/с · ' + (c.infer_ms + c.post_ms).toFixed(0) + ' мс' : 'не работает'}`;
    });
    const tb = $('#spots-body');
    tb.innerHTML = spots.map((s) => {
      const since = s.since ? new Date(s.since).toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' }) : '';
      return `<tr><td><b>${esc(s.id)}</b></td><td>${s.cam}</td>
        <td class="${s.occupied ? 'state-busy' : 'state-free'}">${s.occupied ? 'занято' : 'свободно'}</td>
        <td>${Math.round(s.coverage * 100)}%</td><td>${since}</td>
        <td><button data-spot="${esc(s.id)}" data-occ="1">Занято</button>
            <button data-spot="${esc(s.id)}" data-occ="0">Свободно</button></td></tr>`;
    }).join('') || '<tr><td colspan="6" class="hint">Мест пока нет — нарисуйте их во вкладке «Места».</td></tr>';
  },
  async poll() {
    try {
      this.data = await api('GET', '/api/status');
      this.render();
      // recording started/stopped from the board key: keep the Video tab in sync
      const was = videos.rec, rs = this.data.recording_s;
      videos.setRec(rs >= 0, rs);
      if (was && rs < 0 && currentTab === 'videos') videos.load();
    } catch (e) { /* board restarting */ }
    setTimeout(() => this.poll(), 1500);
  },
};
$('#spots-body').addEventListener('click', async (ev) => {
  const b = ev.target.closest('button[data-spot]');
  if (!b) return;
  b.disabled = true;
  try {
    const r = await api('POST', '/api/label', { spot: b.dataset.spot, occupied: b.dataset.occ === '1' });
    toast(`Разметка сохранена${r.photo ? ' с фото ' + r.photo : ''}`);
  } catch (e) { toast(e.message, true); }
  b.disabled = false;
});
setInterval(() => status.refreshSnapshots(), 3000);

// ---------------------------------------------------------------- spot editor
const editor = {
  cv: $('#ed-canvas'),
  img: new Image(),
  spots: [],
  draft: [],
  selected: null,
  drag: null,
  cam: 0,
  loaded: false,
  dirty: false,
  async open() {
    if (!this.loaded) {
      try {
        this.spots = (await api('GET', '/api/spots')).spots;
        this.loaded = true;
      } catch (e) { toast(e.message, true); }
    }
    this.refresh();
  },
  refresh() {
    this.img.onload = () => this.draw();
    this.img.onerror = () => toast('Нет кадра с камеры ' + this.cam, true);
    this.img.src = `/api/cam/${this.cam}/snapshot.jpg?t=${Date.now()}`;
  },
  draw() {
    const cv = this.cv;
    const w = this.img.naturalWidth || 1280, h = this.img.naturalHeight || 720;
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    const ctx = cv.getContext('2d');
    ctx.clearRect(0, 0, w, h);
    if (this.img.complete && this.img.naturalWidth) ctx.drawImage(this.img, 0, 0, w, h);
    drawSpots(ctx, w, h, this.spots, this.cam, { editor: true, selected: this.selected });
    if (this.draft.length) {
      ctx.beginPath();
      this.draft.forEach(([x, y], i) => (i ? ctx.lineTo(x * w, y * h) : ctx.moveTo(x * w, y * h)));
      ctx.strokeStyle = '#22d3ee';
      ctx.lineWidth = Math.max(2, w / 400);
      ctx.setLineDash([10, 6]);
      ctx.stroke();
      ctx.setLineDash([]);
      for (const [x, y] of this.draft) {
        ctx.beginPath();
        ctx.arc(x * w, y * h, Math.max(4, w / 200), 0, Math.PI * 2);
        ctx.fillStyle = '#22d3ee';
        ctx.fill();
      }
    }
    $('#ed-delete').disabled = !this.selected;
    $('#ed-rename').disabled = !this.selected;
    const n = this.spots.filter((s) => s.cam === this.cam).length;
    $('#ed-info').textContent = `Мест на камере ${this.cam}: ${n}, всего: ${this.spots.length}` +
      (this.dirty ? ' · есть несохранённые изменения' : '');
  },
  pos(ev) {
    const r = this.cv.getBoundingClientRect();
    return [Math.min(1, Math.max(0, (ev.clientX - r.left) / r.width)), Math.min(1, Math.max(0, (ev.clientY - r.top) / r.height))];
  },
  hitVertex(p) {
    if (!this.selected) return -1;
    const r = this.cv.getBoundingClientRect();
    const tol = 14 / r.width;
    return this.selected.points.findIndex(([x, y]) => Math.hypot(x - p[0], (y - p[1]) * r.height / r.width) < tol);
  },
  hitSpot(p) {
    const inside = (poly, [x, y]) => {
      let c = false;
      for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
        const [xi, yi] = poly[i], [xj, yj] = poly[j];
        if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
      }
      return c;
    };
    return this.spots.find((s) => s.cam === this.cam && inside(s.points, p)) || null;
  },
  nextId() {
    let i = 1;
    const ids = new Set(this.spots.map((s) => s.id));
    while (ids.has('P' + i)) i++;
    return 'P' + i;
  },
  finish() {
    if (this.draft.length < 3) { toast('Нужно минимум 3 точки', true); return; }
    const id = prompt('Название места (например A1):', this.nextId());
    if (!id) return;
    if (this.spots.some((s) => s.id === id)) { toast('Такое название уже есть', true); return; }
    const s = { id: id.slice(0, 32), cam: this.cam, points: this.draft };
    this.spots.push(s);
    this.draft = [];
    this.selected = s;
    this.dirty = true;
    this.draw();
  },
};
editor.cv.addEventListener('pointerdown', (ev) => {
  const p = editor.pos(ev);
  const v = editor.hitVertex(p);
  if (v >= 0 && !editor.draft.length) {
    editor.drag = v;
    editor.cv.setPointerCapture(ev.pointerId);
    return;
  }
  if (!editor.draft.length) {
    const s = editor.hitSpot(p);
    if (s) { editor.selected = s; editor.draw(); return; }
  }
  editor.selected = null;
  editor.draft.push(p);
  editor.draw();
});
editor.cv.addEventListener('pointermove', (ev) => {
  if (editor.drag === null || !editor.selected) return;
  editor.selected.points[editor.drag] = editor.pos(ev);
  editor.dirty = true;
  editor.draw();
});
editor.cv.addEventListener('pointerup', () => { editor.drag = null; });
editor.cv.addEventListener('dblclick', (ev) => {
  ev.preventDefault();
  // the second click of a double click already added a point
  if (editor.draft.length > 3) editor.draft.pop();
  editor.finish();
});
$('#ed-cam').addEventListener('change', (ev) => {
  editor.cam = +ev.target.value;
  editor.draft = [];
  editor.selected = null;
  editor.refresh();
});
$('#ed-refresh').addEventListener('click', () => editor.refresh());
$('#ed-undo').addEventListener('click', () => { editor.draft.pop(); editor.draw(); });
$('#ed-close').addEventListener('click', () => editor.finish());
$('#ed-delete').addEventListener('click', () => {
  if (!editor.selected || !confirm(`Удалить место ${editor.selected.id}?`)) return;
  editor.spots = editor.spots.filter((s) => s !== editor.selected);
  editor.selected = null;
  editor.dirty = true;
  editor.draw();
});
$('#ed-rename').addEventListener('click', () => {
  const s = editor.selected;
  if (!s) return;
  const id = prompt('Новое название:', s.id);
  if (!id || id === s.id) return;
  if (editor.spots.some((o) => o.id === id)) { toast('Такое название уже есть', true); return; }
  s.id = id.slice(0, 32);
  editor.dirty = true;
  editor.draw();
});
$('#ed-save').addEventListener('click', (ev) => working(ev.currentTarget, async () => {
  try {
    const r = await api('PUT', '/api/spots', { spots: editor.spots.map(({ id, cam, points }) => ({ id, cam, points })) });
    editor.dirty = false;
    editor.draw();
    toast(`Сохранено мест: ${r.count}`);
  } catch (e) { toast(e.message, true); }
}));
document.addEventListener('keydown', (ev) => {
  if (currentTab !== 'spots' || ev.target.tagName === 'INPUT') return;
  if (ev.key === 'Backspace') { ev.preventDefault(); editor.draft.pop(); editor.draw(); }
  if (ev.key === 'Enter') editor.finish();
  if (ev.key === 'Escape') { editor.draft = []; editor.selected = null; editor.draw(); }
});
window.addEventListener('beforeunload', (ev) => { if (editor.dirty) { ev.preventDefault(); ev.returnValue = ''; } });

// ---------------------------------------------------------------- photos
const photos = {
  day: null,
  async load() {
    try {
      const d = await api('GET', '/api/photos');
      const box = $('#ph-days');
      box.innerHTML = d.days.slice().reverse().map((x) =>
        `<button data-day="${x.date}" class="${x.date === this.day ? 'active' : ''}">${x.date} (${x.count})</button>`).join('') ||
        '<span class="hint">Фото пока нет</span>';
      const host = location.hostname;
      $('#ph-hint').textContent = `SD свободно ${d.disk_free_pct}%. С компьютера: rsync -av root@${host}:/root/data/parking/photos/ ./k510_photos/`;
      if (!this.day && d.days.length) this.show(d.days[d.days.length - 1].date);
      else if (this.day) this.show(this.day);
    } catch (e) { toast(e.message, true); }
  },
  async show(day) {
    this.day = day;
    document.querySelectorAll('#ph-days button').forEach((b) => b.classList.toggle('active', b.dataset.day === day));
    const d = await api('GET', '/api/photos?date=' + day);
    $('#ph-grid').innerHTML = `<div class="toolbar" style="grid-column:1/-1">
        <a class="button" href="/api/photos/archive?from=${day}&to=${day}">Скачать день</a>
        <button id="ph-del" data-day="${day}">Удалить день</button></div>` +
      d.files.slice().reverse().map((f) =>
        `<a href="/photos/${day}/${f}" target="_blank"><img loading="lazy" src="/photos/${day}/${f}" alt=""><span>${f}</span></a>`).join('');
  },
};
$('#ph-days').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-day]');
  if (b) photos.show(b.dataset.day);
});
$('#ph-grid').addEventListener('click', async (ev) => {
  const b = ev.target.closest('#ph-del');
  if (!b || !confirm(`Удалить все фото за ${b.dataset.day}?`)) return;
  try {
    await api('POST', '/api/photos/delete?date=' + b.dataset.day);
    photos.day = null;
    $('#ph-grid').innerHTML = '';
    photos.load();
  } catch (e) { toast(e.message, true); }
});
$('#ph-capture').addEventListener('click', (ev) => working(ev.currentTarget, async () => {
  try {
    const r = await api('POST', '/api/photos/capture');
    toast('Снято: ' + (r.files.join(', ') || 'нет кадров'));
    photos.day = null;
    photos.load();
  } catch (e) { toast(e.message, true); }
}));
const updArchive = () => {
  const q = new URLSearchParams();
  if ($('#ph-from').value) q.set('from', $('#ph-from').value);
  if ($('#ph-to').value) q.set('to', $('#ph-to').value);
  $('#ph-archive').href = '/api/photos/archive' + (q.toString() ? '?' + q : '');
};
$('#ph-from').addEventListener('change', updArchive);
$('#ph-to').addEventListener('change', updArchive);

// ---------------------------------------------------------------- network
const net = {
  async status() {
    try {
      const s = await api('GET', '/api/wifi/status');
      const st = status.data || {};
      $('#net-status').innerHTML = !s.present ? 'Wi-Fi адаптер не найден' :
        `<div>Проводная сеть: <b>${esc(st.eth0 || 'нет')}</b></div>
         <div>Wi-Fi: <b>${s.ap_mode ? 'режим точки доступа для настройки' : esc(s.ssid || 'не подключено')}</b>
         ${s.ip ? '· ' + esc(s.ip) : ''} <span class="hint">(${esc(s.state || '')})</span></div>`;
    } catch (e) { toast(e.message, true); }
  },
  async scan() {
    $('#net-scan').disabled = true;
    $('#net-scan').classList.add('working');
    $('#net-list').innerHTML = '<li class="hint">Поиск…</li>';
    try {
      const r = await api('GET', '/api/wifi/scan');
      $('#net-list').innerHTML = r.networks.map((n) =>
        `<li data-ssid="${esc(n.ssid)}" data-secure="${n.secure ? 1 : 0}"><span>${n.secure ? '🔒' : '🔓'} ${esc(n.ssid)}</span>
         <span class="hint">${n.signal} dBm · ${n.freq > 4000 ? '5' : '2.4'} ГГц</span></li>`).join('') ||
        '<li class="hint">Сети не найдены</li>';
    } catch (e) { toast(e.message, true); }
    $('#net-scan').disabled = false;
    $('#net-scan').classList.remove('working');
  },
};
$('#net-scan').addEventListener('click', () => net.scan());
$('#net-list').addEventListener('click', (ev) => {
  const li = ev.target.closest('li[data-ssid]');
  if (!li) return;
  $('#net-ssid').textContent = li.dataset.ssid;
  $('#net-psk').value = '';
  $('#net-psk').disabled = li.dataset.secure !== '1';
  $('#net-form').classList.remove('hidden');
  $('#net-psk').focus();
});
$('#net-show').addEventListener('change', (ev) => { $('#net-psk').type = ev.target.checked ? 'text' : 'password'; });
$('#net-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const btn = ev.target.querySelector('button');
  btn.disabled = true;
  btn.classList.add('working');
  toast('Подключение… (до 30 секунд; в режиме точки доступа связь со страницей пропадёт)');
  try {
    const r = await api('POST', '/api/wifi/connect', { ssid: $('#net-ssid').textContent, psk: $('#net-psk').value });
    toast('Подключено, IP ' + (r.ip || 'ещё не получен'));
    $('#net-form').classList.add('hidden');
    net.status();
  } catch (e) { toast(e.message, true); }
  btn.disabled = false;
  btn.classList.remove('working');
});

// ---------------------------------------------------------------- settings
const settings = {
  cfg: null,
  async load() {
    try {
      const c = this.cfg = await api('GET', '/api/config');
      const f = $('#cfg-form');
      f.cam0.checked = c.cam_enabled[0];
      f.cam1.checked = c.cam_enabled[1];
      for (const k of ['ai_fps', 'obj_thresh', 'occupancy_threshold', 'footprint', 'debounce', 'photo_interval_min',
        'jpeg_quality', 'min_free_pct', 'video_max_min', 'trigger_ai_cam', 'trigger_video_s', 'trigger_confirm']) f[k].value = c[k];
      f.trigger_classes.value = (c.trigger_classes || []).join(', ');
      f.vehicle_classes.value = c.vehicle_classes.join(', ');
      f.draw_classes.value = c.draw_classes.join(', ');
      f.photo_res.value = `${c.photo_width}x${c.photo_height}`;
      f.keep_raw.checked = c.keep_raw !== false;
    } catch (e) { toast(e.message, true); }
  },
};
$('#cfg-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = ev.target;
  const list = (s) => s.split(',').map((x) => x.trim()).filter(Boolean);
  const [pw, ph] = f.photo_res.value.split('x').map(Number);
  const c = Object.assign({}, settings.cfg, {
    cam_enabled: [f.cam0.checked, f.cam1.checked],
    obj_thresh: +f.obj_thresh.value,
    vehicle_classes: list(f.vehicle_classes.value),
    draw_classes: list(f.draw_classes.value),
    occupancy_threshold: +f.occupancy_threshold.value,
    footprint: +f.footprint.value,
    debounce: +f.debounce.value,
    ai_fps: +f.ai_fps.value,
    photo_interval_min: +f.photo_interval_min.value,
    photo_width: pw,
    photo_height: ph,
    jpeg_quality: +f.jpeg_quality.value,
    keep_raw: f.keep_raw.checked,
    min_free_pct: +f.min_free_pct.value,
    video_max_min: +f.video_max_min.value,
    trigger_ai_cam: +f.trigger_ai_cam.value,
    trigger_video_s: +f.trigger_video_s.value,
    trigger_confirm: +f.trigger_confirm.value,
    trigger_classes: list(f.trigger_classes.value),
  });
  try {
    const r = await api('PUT', '/api/config', c);
    settings.cfg = c;
    $('#cfg-restart').classList.toggle('hidden', !r.restart_required);
    toast(r.restart_required ? 'Сохранено. Часть настроек применится после перезапуска.' : 'Сохранено');
  } catch (e) { toast(e.message, true); }
});
$('#cfg-restart').addEventListener('click', (ev) => working(ev.currentTarget, async () => {
  try {
    await api('POST', '/api/restart');
    toast('Перезапуск… страница обновится сама');
    $('#cfg-restart').classList.add('hidden');
    status.camsBuilt = false;
  } catch (e) { toast(e.message, true); }
}));

// ---------------------------------------------------------------- videos
const fmtSize = (b) => b > 1e9 ? (b / 1e9).toFixed(1) + ' ГБ' : (b / 1e6).toFixed(1) + ' МБ';
const videos = {
  timer: 0,
  async load() {
    try {
      const d = await api('GET', '/api/videos');
      this.setRec(d.recording);
      // group files by recording (HHMMSS), then by camera
      $('#vd-list').innerHTML = d.days.map(({ day, files }) => {
        const recs = {};
        for (const f of files) {
          const m = f.name.match(/^(\d{6})_cam(\d)\.(mp4|jpg)$/);
          if (!m) continue;
          const r = recs[m[1]] = recs[m[1]] || {};
          const c = r[m[2]] = r[m[2]] || {};
          if (m[3] === 'mp4') c.size = f.size; else c.poster = true;
        }
        const names = Object.keys(recs).sort().reverse();
        if (!names.length) return '';
        return `<h2>${esc(day)}</h2>` + names.map((n) => {
          const t = `${n.slice(0, 2)}:${n.slice(2, 4)}:${n.slice(4, 6)}`;
          const cams = Object.keys(recs[n]).sort().filter((c) => recs[n][c].size !== undefined);
          return `<div class="card"><div class="toolbar"><b>${t}</b>
              <button data-vdel="${day}/${n}">Удалить</button></div>
            <div class="cams">${cams.map((c) => {
              const base = `/videos/${day}/${n}_cam${c}`;
              return `<div><video controls preload="none" style="width:100%;border-radius:8px;background:#000"
                  ${recs[n][c].poster ? `poster="${base}.jpg"` : ''} src="${base}.mp4"></video>
                <div class="hint">Камера ${c} · ${fmtSize(recs[n][c].size)} ·
                  <a href="${base}.mp4" download>скачать</a></div></div>`;
            }).join('')}</div></div>`;
        }).join('');
      }).join('') || '<p class="hint">Видео пока нет</p>';
    } catch (e) { toast(e.message, true); }
  },
  setRec(on, seconds) {
    this.rec = on;
    $('#vd-rec').textContent = on ? '■ Остановить запись' : '● Начать запись';
    $('#vd-rec').classList.toggle('primary', !on);
    $('#vd-state').textContent = on && seconds >= 0
      ? `идёт запись ${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}` : '';
  },
};
$('#vd-rec').addEventListener('click', (ev) => working(ev.currentTarget, async () => {
  try {
    await api('POST', videos.rec ? '/api/video/stop' : '/api/video/start');
    toast(videos.rec ? 'Видео сохранено' : 'Запись началась');
    await videos.load();
  } catch (e) { toast(e.message, true); }
}));
$('#vd-list').addEventListener('click', async (ev) => {
  const b = ev.target.closest('button[data-vdel]');
  if (!b) return;
  const [date, name] = b.dataset.vdel.split('/');
  if (!confirm(`Удалить видео ${date} ${name.slice(0, 2)}:${name.slice(2, 4)}:${name.slice(4)} (все камеры)?`)) return;
  try {
    await api('POST', `/api/videos/delete?date=${date}&name=${name}`);
    videos.load();
  } catch (e) { toast(e.message, true); }
});

// ---------------------------------------------------------------- models
const models = {
  async load() {
    try {
      const [r, st] = await Promise.all([api('GET', '/api/models'), api('GET', '/api/status')]);
      const bench = {};
      for (const b of (r.bench && r.bench.models) || []) bench[b.id] = b;
      const cams = st.cams.filter((c) => c.running);
      const ms = cams.length ? cams.map((c) => (c.infer_ms + c.post_ms).toFixed(0) + ' мс').join(' / ') : '—';
      $('#md-current').innerHTML = `Сейчас работает: <b>${esc(st.model_title || r.running || '—')}</b> · на кадр ${ms}` +
        (r.configured && r.configured !== r.running ? `<div class="hint">В настройках выбрана ${esc(r.configured)}, ` +
          'но она не загрузилась — см. лог.</div>' : '');
      const found = (b) => {
        if (!b) return '<span class="hint">нет замера</span>';
        const n = {};
        for (const d of b.dets) n[d.name] = (n[d.name] || 0) + 1;
        const texts = b.dets.map((d) => d.text).filter(Boolean);
        return esc(Object.entries(n).map(([k, v]) => v > 1 ? `${k} ×${v}` : k).join(', ') || '—') +
          (texts.length ? `<div class="hint">${esc(texts.join(' · '))}</div>` : '');
      };
      $('#md-table tbody').innerHTML = r.models.map((m) => {
        const b = bench[m.id];
        const cur = m.id === r.running;
        return `<tr${cur ? ' class="state-free"' : ''}><td>${esc(m.title)}<div class="hint">${esc(m.what)}` +
          `${m.note ? ' · <b>' + esc(m.note) + '</b>' : ''}</div><div class="hint">${esc(m.id)}</div></td>
          <td>${(m.size / 1e6).toFixed(1)} МБ</td>
          <td>${b ? b.infer_ms.toFixed(1) : ''}</td><td>${b ? b.post_ms.toFixed(1) : ''}</td>
          <td>${b ? (1000 / (b.infer_ms + b.post_ms)).toFixed(1) : ''}</td><td>${found(b)}</td>
          <td>${cur ? '✓ работает' : `<button data-use="${esc(m.id)}">Включить</button>`}</td></tr>`;
      }).join('');
      const withPic = r.models.filter((m) => bench[m.id]);
      $('#md-bench-title').classList.toggle('hidden', !withPic.length);
      const t = r.bench ? r.bench.ts : 0;
      $('#md-bench').innerHTML = withPic.map((m) =>
        `<a href="/bench/${encodeURIComponent(m.id)}.jpg?t=${t}" target="_blank"><img loading="lazy" src="/bench/${encodeURIComponent(m.id)}.jpg?t=${t}">` +
        `<span>${esc(m.title)} · ${bench[m.id].infer_ms.toFixed(0)} мс</span></a>`).join('');
    } catch (e) { toast(e.message, true); }
  },
};
$('#md-table').addEventListener('click', (ev) => {
  const b = ev.target.closest('button[data-use]');
  if (!b) return;
  working(b, async () => {
    try {
      const cfg = await api('GET', '/api/config');
      cfg.model = b.dataset.use;
      await api('PUT', '/api/config', cfg);
      await api('POST', '/api/restart');
      toast('Модель выбрана, приложение перезапускается (~15 с)…');
      setTimeout(() => { status.camsBuilt = false; models.load(); }, 15000);
    } catch (e) { toast(e.message, true); }
  });
});

// launcher page (app switching) lives on :8080 of the same host
$('#apps-link').href = `http://${location.hostname}:8080/`;

// ---------------------------------------------------------------- start
status.poll();
let startTab = 'status';
try { startTab = localStorage.getItem('tab') || 'status'; } catch (e) { /* private mode */ }
showTab(startTab);
