import * as THREE from 'three';

/* ================= helpers ================= */
const clamp = (v, a = 0, b = 1) => Math.min(b, Math.max(a, v));
const seg = (p, a, b) => clamp((p - a) / (b - a));
const ease = t => (t < .5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
const easeOut = t => 1 - Math.pow(1 - t, 3);
const lerp = (a, b, t) => a + (b - a) * t;

/* ================= three.js scene ================= */
const canvas = document.getElementById('bg');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);

const scene = new THREE.Scene();
scene.fog = new THREE.FogExp2(0x050507, 0.04);
const camera = new THREE.PerspectiveCamera(45, innerWidth / innerHeight, 0.1, 100);
camera.position.set(0, 0, 9);

scene.add(new THREE.AmbientLight(0xffffff, 0.35));
const key = new THREE.PointLight(0xfebc2e, 30, 40); key.position.set(4, 3, 5); scene.add(key);
const fill = new THREE.PointLight(0x28c840, 14, 40); fill.position.set(-4, -2, 4); scene.add(fill);

/* ================= the app screen (2D canvas -> texture) ================= */
const W = 1440, H = 900;
const cv = document.createElement('canvas'); cv.width = W; cv.height = H;
const g = cv.getContext('2d');
const tex = new THREE.CanvasTexture(cv);
tex.colorSpace = THREE.SRGBColorSpace;
tex.generateMipmaps = false; tex.minFilter = THREE.LinearFilter; tex.anisotropy = 8;

const C = { bg: '#0c0c11', fg: '#e8e8ee', mut: '#7b7b8a', line: 'rgba(255,255,255,.07)', y: '#febc2e', g: '#28c840', r: '#ff5f57' };
const SANS = 'Inter, "Segoe UI", system-ui, sans-serif';
const MONO = '"JetBrains Mono", Consolas, monospace';
const rr = (x, y, w, h, r) => { g.beginPath(); g.roundRect(x, y, w, h, r); };
const txt = (s, x, y, font, color, align = 'left') => {
  g.font = font; g.fillStyle = color; g.textAlign = align; g.textBaseline = 'alphabetic'; g.fillText(s, x, y);
};

/* ---- demo timeline (p = 0..1, driven by scroll) ---- */
const PROMPT = 'Open the browser and search for Qt 6 docs';
const TYPED = 'Qt 6 docs';
const TOOLS = [
  { name: 'screenshot',    params: '{"format":"png"}',    on: [.050, .075], run: [.075, .140], ms: '118 ms' },
  { name: 'mouse_move',    params: '{"x":1104,"y":412}',  on: [.260, .285], run: [.285, .420], ms: '240 ms' },
  { name: 'mouse_click',   params: '{"button":"left"}',   on: [.500, .525], run: [.525, .580], ms: '12 ms' },
  { name: 'keyboard_type', params: '{"text":"Qt 6 docs"}', on: [.600, .625], run: [.625, .720], ms: '310 ms' },
  { name: 'wait',          params: '{"ms":800}',          on: [.760, .785], run: [.785, .850], ms: '800 ms' },
  { name: 'end_session',   params: '{}',                  on: [.900, .925], run: [.925, .940], ms: 'ok' },
];

/* ---- live pane geometry ---- */
const PX = 820, PY = 140, PW = 600, PH = 740;
const BX = PX + 26, BY = PY + 26, BW = PW - 52, BH = PH - 92;
const SX = BX + 54, SY = BY + 240, SW = BW - 108, SH = 54;
const TARGET = [SX + SW * 0.38, SY + SH / 2];
const START = [PX + PW - 110, PY + PH - 170];

function icon(name, cx, cy, col) {
  g.save(); g.translate(cx, cy);
  g.strokeStyle = col; g.fillStyle = col; g.lineWidth = 2; g.lineCap = 'round'; g.lineJoin = 'round';
  if (name === 'screenshot') {
    const s = 9;
    [[-1, -1], [1, -1], [1, 1], [-1, 1]].forEach(([a, b]) => {
      g.beginPath(); g.moveTo(a * s, b * (s - 6)); g.lineTo(a * s, b * s); g.lineTo(a * (s - 6), b * s); g.stroke();
    });
    g.beginPath(); g.arc(0, 0, 3, 0, 7); g.fill();
  } else if (name === 'mouse_move') {
    g.beginPath(); g.moveTo(-6, -8); g.lineTo(8, 0); g.lineTo(1, 2); g.lineTo(-1, 9); g.closePath(); g.fill();
  } else if (name === 'mouse_click') {
    g.beginPath(); g.arc(0, 0, 9, 0, 7); g.stroke(); g.beginPath(); g.arc(0, 0, 3, 0, 7); g.fill();
  } else if (name === 'keyboard_type') {
    g.beginPath(); g.roundRect(-10, -6, 20, 12, 3); g.stroke();
    [-5, 0, 5].forEach(x => g.fillRect(x - 1, -2, 2, 2)); g.fillRect(-5, 2, 10, 2);
  } else if (name === 'wait') {
    g.beginPath(); g.arc(0, 0, 9, 0, 7); g.stroke();
    g.beginPath(); g.moveTo(0, -5); g.lineTo(0, 0); g.lineTo(4, 3); g.stroke();
  } else {
    g.fillRect(-6, -6, 12, 12);
  }
  g.restore();
}

function avatar(x, y) {
  g.beginPath(); g.arc(x, y, 14, 0, 7); g.fillStyle = C.y; g.fill();
  txt('M', x, y + 5, `800 15px ${SANS}`, '#1a1400', 'center');
}

function drawUser(y) {
  g.font = `400 20px ${SANS}`;
  const w = g.measureText(PROMPT).width + 44;
  rr(780 - w, y, w, 52, 18); g.fillStyle = '#1a1a23'; g.fill();
  txt(PROMPT, 780 - w + 22, y + 33, `400 20px ${SANS}`, C.fg);
}
function drawAgent(y, text, sub) {
  avatar(266, y + 18);
  txt(text, 294, y + 24, `400 20px ${SANS}`, C.fg);
  if (sub) txt(sub, 294, y + 48, `400 15px ${SANS}`, C.mut);
}
function drawTool(tl, y, p, time) {
  const run = seg(p, tl.run[0], tl.run[1]), done = run >= 1;
  rr(250, y, 530, 70, 14); g.fillStyle = '#0f0f15'; g.fill();
  g.strokeStyle = done ? 'rgba(255,255,255,.07)' : 'rgba(254,188,46,.45)'; g.lineWidth = 1.5; g.stroke();
  rr(268, y + 17, 36, 36, 10); g.fillStyle = done ? 'rgba(40,200,64,.12)' : 'rgba(254,188,46,.12)'; g.fill();
  icon(tl.name, 286, y + 35, done ? C.g : C.y);
  txt(tl.name, 320, y + 31, `600 17px ${MONO}`, C.fg);
  txt(tl.params, 320, y + 54, `400 14px ${MONO}`, C.mut);
  if (done) {
    txt('✓ ' + tl.ms, 762, y + 40, `600 14px ${MONO}`, C.g, 'right');
  } else {
    txt('running', 762, y + 40, `600 14px ${MONO}`, C.y, 'right');
    g.strokeStyle = C.y; g.lineWidth = 2.5; g.lineCap = 'round';
    const a0 = time * 6; g.beginPath(); g.arc(690, y + 35, 7, a0, a0 + 4.2); g.stroke();
  }
}

function drawCursor(x, y) {
  g.save(); g.translate(x, y);
  const gl = g.createRadialGradient(0, 0, 0, 0, 0, 34);
  gl.addColorStop(0, 'rgba(254,188,46,.35)'); gl.addColorStop(1, 'rgba(254,188,46,0)');
  g.fillStyle = gl; g.fillRect(-34, -34, 68, 68);
  g.beginPath(); g.moveTo(0, 0); g.lineTo(0, 26); g.lineTo(7, 20); g.lineTo(12, 31); g.lineTo(17, 29); g.lineTo(12, 18); g.lineTo(21, 18); g.closePath();
  g.fillStyle = C.y; g.strokeStyle = '#1a1400'; g.lineWidth = 2; g.fill(); g.stroke();
  rr(20, 28, 74, 22, 11); g.fillStyle = C.y; g.fill();
  txt('M-Cowork', 57, 43, `700 11px ${SANS}`, '#1a1400', 'center');
  g.restore();
}

const RESULTS = [
  ['Qt 6 Documentation', 'doc.qt.io › qt-6', 'Reference docs, tutorials and examples for Qt 6.'],
  ['Qt Widgets — Qt 6', 'doc.qt.io › qt-6 › qtwidgets-index.html', 'The module behind classic desktop UIs in C++.'],
  ['Getting Started with Qt 6', 'doc.qt.io › qt-6 › gettingstarted.html', 'Install Qt and build your first application.'],
];

function drawLive(p, time) {
  g.save(); rr(PX, PY, PW, PH, 18); g.clip();

  // wallpaper
  const wg = g.createLinearGradient(PX, PY, PX + PW, PY + PH);
  wg.addColorStop(0, '#07070b'); wg.addColorStop(1, '#15112a'); g.fillStyle = wg; g.fillRect(PX, PY, PW, PH);
  const rg = g.createRadialGradient(PX + PW * .8, PY + PH * .2, 0, PX + PW * .8, PY + PH * .2, 320);
  rg.addColorStop(0, 'rgba(254,188,46,.09)'); rg.addColorStop(1, 'rgba(254,188,46,0)'); g.fillStyle = rg; g.fillRect(PX, PY, PW, PH);

  // browser window
  rr(BX, BY, BW, BH, 12); g.fillStyle = '#14141b'; g.fill();
  g.save(); rr(BX, BY, BW, BH, 12); g.clip();
  g.fillStyle = '#0d0d12'; g.fillRect(BX, BY, BW, 40);
  [C.r, C.y, C.g].forEach((c, i) => { g.beginPath(); g.arc(BX + 18 + i * 17, BY + 20, 5, 0, 7); g.fillStyle = c; g.fill(); });
  rr(BX + 84, BY + 8, 170, 32, [8, 8, 0, 0]); g.fillStyle = '#14141b'; g.fill();
  txt('Qt Documentation', BX + 98, BY + 29, `400 13px ${SANS}`, C.mut);
  rr(BX + 16, BY + 48, BW - 32, 30, 15); g.fillStyle = '#1b1b24'; g.fill();
  txt('doc.qt.io/qt-6', BX + 40, BY + 68, `400 13px ${MONO}`, C.mut);
  const lb = seg(p, .785, .85);
  if (lb > 0 && lb < 1) { g.fillStyle = C.y; g.fillRect(BX, BY + 82, BW * lb, 3); }

  g.beginPath(); g.arc(BX + BW / 2, BY + 138, 28, 0, 7); g.fillStyle = C.g; g.fill();
  txt('Qt', BX + BW / 2, BY + 148, `800 26px ${SANS}`, '#06140a', 'center');
  txt('Qt 6 Documentation', BX + BW / 2, BY + 210, `700 28px ${SANS}`, C.fg, 'center');

  // search box
  const focus = p > .54;
  rr(SX, SY, SW, SH, 27); g.fillStyle = '#1b1b24'; g.fill();
  g.strokeStyle = focus ? C.y : C.line; g.lineWidth = 2; g.stroke();
  g.strokeStyle = C.mut; g.lineWidth = 2; g.beginPath(); g.arc(SX + 28, SY + 26, 7, 0, 7); g.moveTo(SX + 33, SY + 31); g.lineTo(SX + 39, SY + 37); g.stroke();
  const n = Math.floor(seg(p, .625, .72) * (TYPED.length + .99));
  const blink = Math.floor(time * 2) % 2 === 0;
  if (n === 0 && !focus) txt('Search the docs…', SX + 56, SY + 34, `400 18px ${SANS}`, '#5a5a68');
  else txt(TYPED.slice(0, n) + (focus && blink ? '|' : ''), SX + 56, SY + 34, `400 18px ${SANS}`, C.fg);

  // results
  const rp = seg(p, .83, .89);
  RESULTS.forEach((r, i) => {
    const a = clamp(rp * 3 - i), y = BY + 336 + i * 80 + (1 - a) * 10;
    if (a <= 0) return;
    g.save(); g.globalAlpha = a;
    txt(r[0], SX, y + 20, `600 20px ${SANS}`, '#8ab4f8');
    txt(r[1], SX, y + 42, `400 13px ${MONO}`, '#3fa65a');
    txt(r[2], SX, y + 64, `400 15px ${SANS}`, C.mut);
    g.restore();
  });
  g.restore(); // browser clip

  // taskbar
  g.fillStyle = 'rgba(8,8,12,.92)'; g.fillRect(PX, PY + PH - 44, PW, 44);
  ['#febc2e', '#28c840', '#6c8cff', '#ff5f57', '#8a8a98'].forEach((c, i) => {
    rr(PX + PW / 2 - 78 + i * 32, PY + PH - 34, 24, 24, 7); g.globalAlpha = .8; g.fillStyle = c; g.fill(); g.globalAlpha = 1;
  });

  // cursor position
  const mT = ease(seg(p, .285, .42));
  const idle = [Math.sin(time * .7) * 14, Math.cos(time * .9) * 10];
  const sx = START[0] + idle[0] * (1 - mT), sy = START[1] + idle[1] * (1 - mT);
  const cx = lerp(sx, TARGET[0], mT), cy = lerp(sy, TARGET[1], mT);

  // mouse_move: trail + target crosshair
  if (mT > 0 && mT < 1) {
    g.save(); g.setLineDash([6, 8]); g.strokeStyle = 'rgba(254,188,46,.45)'; g.lineWidth = 2;
    g.beginPath(); g.moveTo(START[0], START[1]); g.lineTo(cx, cy); g.stroke(); g.restore();
  }
  if (p > .26 && p < .43) {
    g.strokeStyle = 'rgba(254,188,46,.7)'; g.lineWidth = 1.5;
    g.beginPath(); g.arc(TARGET[0], TARGET[1], 12, 0, 7);
    g.moveTo(TARGET[0] - 20, TARGET[1]); g.lineTo(TARGET[0] + 20, TARGET[1]);
    g.moveTo(TARGET[0], TARGET[1] - 20); g.lineTo(TARGET[0], TARGET[1] + 20); g.stroke();
  }

  // mouse_click: ripple
  const cp = seg(p, .525, .58);
  if (cp > 0 && cp < 1) {
    g.strokeStyle = `rgba(254,188,46,${1 - cp})`; g.lineWidth = 3;
    g.beginPath(); g.arc(TARGET[0], TARGET[1], 8 + cp * 44, 0, 7); g.stroke();
  }

  drawCursor(cx, cy);

  // screenshot: viewfinder + scan + flash
  const sp = seg(p, .075, .14);
  if (sp > 0 && sp < 1) {
    const a = Math.sin(Math.PI * sp), m = 14, L = 36;
    g.strokeStyle = `rgba(254,188,46,${a})`; g.lineWidth = 3; g.lineCap = 'round';
    [[PX + m, PY + m, 1, 1], [PX + PW - m, PY + m, -1, 1], [PX + PW - m, PY + PH - m, -1, -1], [PX + m, PY + PH - m, 1, -1]].forEach(([x, y, dx, dy]) => {
      g.beginPath(); g.moveTo(x, y + dy * L); g.lineTo(x, y); g.lineTo(x + dx * L, y); g.stroke();
    });
    const ly = PY + sp * PH, sg = g.createLinearGradient(0, ly - 60, 0, ly);
    sg.addColorStop(0, 'rgba(254,188,46,0)'); sg.addColorStop(1, 'rgba(254,188,46,.28)');
    g.fillStyle = sg; g.fillRect(PX, ly - 60, PW, 60);
    g.fillStyle = C.y; g.fillRect(PX, ly - 1, PW, 2);
    txt('screenshot · 1920×1080', PX + PW / 2, PY + 40, `600 13px ${MONO}`, `rgba(254,188,46,${a})`, 'center');
    const f = seg(sp, .75, 1);
    if (f > 0) { g.fillStyle = `rgba(255,255,255,${.3 * Math.sin(Math.PI * f)})`; g.fillRect(PX, PY, PW, PH); }
  }

  // session ended toast
  const et = seg(p, .94, .975);
  if (et > 0) {
    g.save(); g.globalAlpha = et;
    rr(PX + PW / 2 - 110, PY + PH - 100 - et * 4, 220, 40, 20); g.fillStyle = 'rgba(8,8,12,.92)'; g.fill();
    g.strokeStyle = 'rgba(40,200,64,.5)'; g.lineWidth = 1.5; g.stroke();
    txt('✓ session ended', PX + PW / 2, PY + PH - 74 - et * 4, `600 14px ${MONO}`, C.g, 'center');
    g.restore();
  }
  g.restore(); // pane clip

  // pane border
  rr(PX, PY, PW, PH, 18); g.strokeStyle = C.line; g.lineWidth = 1.5; g.stroke();
}

/* ---- build terminal (opens over the screen in the Build section) ---- */
const TERM = [
  { tab: 'PowerShell', prompt: 'PS matricX-cowork> ', lines: [
    ['cmd', '.\\scripts\\install-packages.ps1', 1.0],
    ['out', 'Installing Qt 6\u2026', .35], ['out', 'Installing CMake\u2026', .35], ['out', 'Installing Ninja\u2026', .35],
    ['ok', 'All packages installed', .6],
    ['cmd', '.\\scripts\\build.ps1', .8],
    ['out', '-- Configuring done', .35],
    ['bar', 'Compiling sources', 2.0],
    ['ok', 'Build succeeded', .6],
    ['cmd', '.\\scripts\\run.ps1', .8],
    ['ok', 'M-Cowork started', 1.0],
  ] },
  { tab: 'bash', prompt: '$ ', lines: [
    ['cmd', './scripts/build.sh', .8], ['bar', 'Compiling sources', 2.0], ['ok', 'Build succeeded', .6],
    ['cmd', './scripts/run.sh', .8], ['ok', 'M-Cowork started', 1.0],
  ] },
];
TERM.forEach(t => { let c = 0; t.lines.forEach(l => { l.start = c; c += l[2]; }); t.total = c; });
const TERM_TOTAL = TERM[0].total + TERM[1].total;

function drawTerminal(v, tp, time) {
  const T = tp * TERM_TOTAL, ti = T < TERM[0].total ? 0 : 1;
  const local = ti ? T - TERM[0].total : T, tm = TERM[ti], e = easeOut(v);
  g.save();
  g.fillStyle = `rgba(0,0,0,${.6 * e})`; g.fillRect(0, 0, W, H);
  g.translate(W / 2, H / 2 + (1 - e) * 24); g.scale(.9 + .1 * e, .9 + .1 * e); g.translate(-W / 2, -H / 2);
  g.globalAlpha = e;
  const x = 110, y = 100, w = 1220, h = 700, F = `400 20px ${MONO}`;
  rr(x, y, w, h, 16); g.fillStyle = '#07070a'; g.fill(); g.strokeStyle = 'rgba(255,255,255,.12)'; g.lineWidth = 1.5; g.stroke();
  g.save(); rr(x, y, w, h, 16); g.clip();
  g.fillStyle = '#0f0f14'; g.fillRect(x, y, w, 46);
  [C.r, C.y, C.g].forEach((c, i) => { g.beginPath(); g.arc(x + 26 + i * 22, y + 23, 7, 0, 7); g.fillStyle = c; g.fill(); });
  TERM.forEach((t, i) => {
    const tx = x + 130 + i * 150;
    rr(tx, y + 8, 140, 38, [10, 10, 0, 0]); g.fillStyle = i === ti ? '#07070a' : 'rgba(255,255,255,0)'; g.fill();
    txt(t.tab, tx + 70, y + 32, `600 14px ${MONO}`, i === ti ? C.fg : C.mut, 'center');
  });
  const lh = 30, blink = Math.floor(time * 2) % 2 === 0;
  let ly = y + 92;
  const caret = cx => { g.fillStyle = C.fg; g.fillRect(cx + 2, ly - 18, 11, 22); };
  tm.lines.forEach(l => {
    if (local < l.start) return;
    const k = clamp((local - l.start) / l[2]);
    if (l[0] === 'cmd') {
      txt(tm.prompt, x + 28, ly, F, C.g);
      const pw = g.measureText(tm.prompt).width;
      const s = l[1].slice(0, Math.floor(k * (l[1].length + .99)));
      txt(s, x + 28 + pw, ly, F, C.fg);
      if (k < 1) caret(x + 28 + pw + g.measureText(s).width);
    } else if (l[0] === 'out') {
      txt(l[1], x + 28, ly, F, C.mut);
    } else if (l[0] === 'ok') {
      txt('\u2713 ' + l[1], x + 28, ly, F, C.g);
    } else {
      rr(x + 28, ly - 14, 360, 12, 6); g.fillStyle = '#1b1b24'; g.fill();
      if (k > .03) { rr(x + 28, ly - 14, 360 * k, 12, 6); g.fillStyle = k < 1 ? C.y : C.g; g.fill(); }
      txt(Math.round(k * 100) + '%  ' + l[1], x + 410, ly, F, k < 1 ? C.y : C.g);
    }
    ly += lh;
  });
  if (local >= tm.total) {
    txt(tm.prompt, x + 28, ly, F, C.g);
    if (blink) caret(x + 28 + g.measureText(tm.prompt).width);
  }
  g.restore();
  g.restore();
}

function drawScreen(p, time, termV = 0, termP = 0) {
  g.clearRect(0, 0, W, H);
  g.save(); rr(0, 0, W, H, 36); g.clip();
  g.fillStyle = C.bg; g.fillRect(0, 0, W, H);

  // title bar
  g.fillStyle = '#09090d'; g.fillRect(0, 0, W, 64);
  [C.r, C.y, C.g].forEach((c, i) => { g.beginPath(); g.arc(36 + i * 26, 32, 9, 0, 7); g.fillStyle = c; g.fill(); });

  // sidebar
  g.fillStyle = '#08080c'; g.fillRect(0, 64, 230, H - 64);
  rr(16, 84, 198, 40, 10); g.strokeStyle = C.line; g.lineWidth = 1.5; g.stroke();
  txt('+  New chat', 36, 110, `600 16px ${SANS}`, C.fg);
  txt('CHATS', 24, 166, `600 12px ${SANS}`, '#555564');
  ['Find Qt 6 docs', 'Fix build script', 'Rename files', 'Screenshot test'].forEach((t, i) => {
    if (i === 0) { rr(12, 180 + i * 48, 206, 40, 10); g.fillStyle = '#16161e'; g.fill(); }
    txt(t, 28, 206 + i * 48, `400 16px ${SANS}`, i === 0 ? C.fg : C.mut);
  });
  g.beginPath(); g.arc(34, 852, 11, 0, 7); g.strokeStyle = C.mut; g.lineWidth = 2; g.stroke();
  g.beginPath(); g.arc(34, 852, 3.5, 0, 7); g.fillStyle = C.mut; g.fill();
  txt('Settings', 56, 858, `400 15px ${SANS}`, C.mut);

  // chat header (outlined title)
  g.font = `800 40px ${SANS}`; g.textAlign = 'center'; g.lineWidth = 1.8; g.strokeStyle = C.y; g.strokeText('M-Cowork', 515, 118);
  g.fillStyle = C.line; g.fillRect(250, 140, 530, 1);

  // chat stream
  g.save(); g.beginPath(); g.rect(230, 146, 570, 656); g.clip();

  // empty state
  const ea = 1 - seg(p, 0, .025);
  if (ea > 0) {
    g.save(); g.globalAlpha = ea;
    txt('What should I do on your computer?', 515, 400, `700 24px ${SANS}`, C.fg, 'center');
    ['Open the browser', 'Organize my downloads', 'Take a screenshot'].forEach((t, i) => {
      g.font = `400 16px ${SANS}`; const w = g.measureText(t).width + 36;
      rr(515 - w / 2, 430 + i * 52, w, 40, 20); g.strokeStyle = C.line; g.lineWidth = 1.5; g.stroke();
      txt(t, 515, 456 + i * 52, `400 16px ${SANS}`, C.mut, 'center');
    });
    g.restore();
  }

  const items = [
    { h: 52, t: seg(p, 0, .03), draw: y => drawUser(y) },
    { h: 36, t: seg(p, .035, .06), draw: y => drawAgent(y, 'On it. Taking a screenshot first.') },
    ...TOOLS.map(tl => ({ h: 70, t: seg(p, tl.on[0], tl.on[1]), draw: y => drawTool(tl, y, p, time) })),
    { h: 60, t: seg(p, .94, .975), draw: y => drawAgent(y, 'Done. Found the Qt 6 documentation.', 'Session ended · 6 tool calls') },
  ];
  let cy = 0;
  items.forEach(it => { it.e = easeOut(it.t); it.y = cy; if (it.e > 0) cy += (it.h + 14) * it.e; });
  const off = Math.max(0, cy - 620);
  items.forEach(it => {
    if (it.e <= 0) return;
    g.save(); g.globalAlpha = it.e; it.draw(160 + it.y - off + (1 - it.e) * 14); g.restore();
  });
  g.restore(); // chat clip

  const fg = g.createLinearGradient(0, 146, 0, 182);
  fg.addColorStop(0, C.bg); fg.addColorStop(1, 'rgba(12,12,17,0)'); g.fillStyle = fg; g.fillRect(230, 146, 570, 36);

  // input bar
  rr(250, 818, 530, 58, 29); g.fillStyle = '#12121a'; g.fill(); g.strokeStyle = C.line; g.lineWidth = 1.5; g.stroke();
  const tn = Math.floor(seg(p, 0, .02) * (PROMPT.length + .99));
  if (tn > 0 && p < .03) txt(PROMPT.slice(0, tn), 276, 853, `400 18px ${SANS}`, C.fg);
  else txt(p > .03 && p < .955 ? 'Agent is working…' : 'Ask M-Cowork anything…', 276, 853, `400 18px ${SANS}`, '#5a5a68');
  g.beginPath(); g.arc(706, 847, 19, 0, 7); g.fillStyle = '#1b1b25'; g.fill();
  g.beginPath(); g.arc(706, 847, 7, 0, 7); g.strokeStyle = C.mut; g.lineWidth = 2; g.stroke();
  g.beginPath(); g.arc(752, 847, 19, 0, 7); g.fillStyle = C.y; g.fill();
  rr(748, 837, 8, 14, 4); g.fillStyle = '#1a1400'; g.fill();
  g.beginPath(); g.arc(752, 849, 8, 0, Math.PI); g.strokeStyle = '#1a1400'; g.lineWidth = 2; g.stroke();

  // live pane header + status
  txt('LIVE SCREEN', PX, 120, `600 12px ${SANS}`, '#555564');
  const active = p > .04 && p < .955;
  const status = active ? 'Agent in control · Esc to abort' : p >= .955 ? 'Session ended' : 'Waiting for a task';
  g.font = `400 13px ${MONO}`;
  const sw = g.measureText(status).width;
  txt(status, PX + PW, 120, `400 13px ${MONO}`, active ? C.g : C.mut, 'right');
  g.beginPath(); g.arc(PX + PW - sw - 14, 115, 4, 0, 7);
  g.fillStyle = active ? C.g : '#444452'; g.globalAlpha = active ? .5 + .5 * Math.sin(time * 5) : 1; g.fill(); g.globalAlpha = 1;

  drawLive(p, time);
  if (termV > .005) drawTerminal(termV, termP, time);
  g.restore();
}

/* ================= window + glow in 3D ================= */
function roundedShape(w, h, r) {
  const s = new THREE.Shape(), x = -w / 2, y = -h / 2;
  s.moveTo(x + r, y); s.lineTo(x + w - r, y); s.quadraticCurveTo(x + w, y, x + w, y + r);
  s.lineTo(x + w, y + h - r); s.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  s.lineTo(x + r, y + h); s.quadraticCurveTo(x, y + h, x, y + h - r);
  s.lineTo(x, y + r); s.quadraticCurveTo(x, y, x + r, y);
  return s;
}
const WW = 6.4, WH = 4.04;
const win = new THREE.Group();
const body = new THREE.Mesh(
  new THREE.ExtrudeGeometry(roundedShape(WW, WH, 0.2), { depth: 0.18, bevelEnabled: true, bevelSize: 0.03, bevelThickness: 0.03, bevelSegments: 4 }),
  new THREE.MeshStandardMaterial({ color: 0x15151b, metalness: 0.8, roughness: 0.35 })
);
body.position.z = -0.1; win.add(body);
const screen = new THREE.Mesh(
  new THREE.PlaneGeometry(6.3, 6.3 / 1.6),
  new THREE.MeshBasicMaterial({ map: tex, transparent: true, toneMapped: false })
);
screen.position.z = 0.125; win.add(screen);
const glow = new THREE.Mesh(
  new THREE.PlaneGeometry(WW * 1.7, WH * 1.9),
  new THREE.ShaderMaterial({
    transparent: true, depthWrite: false,
    vertexShader: 'varying vec2 v;void main(){v=uv;gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.);}',
    fragmentShader: 'varying vec2 v;void main(){float d=distance(v,vec2(.5));float a=smoothstep(.5,.0,d);gl_FragColor=vec4(.99,.74,.18,a*a*.10);}'
  })
);
glow.position.z = -0.45; win.add(glow);
scene.add(win);

/* Clawd — the orange Claude mascot — peeks from behind the screen */
const clawd = new THREE.Group(); clawd.visible = false; win.add(clawd);

const clawdMat = new THREE.MeshStandardMaterial({ color: 0xd97757, roughness: .55, metalness: .05 });
const clawdEyeMat = new THREE.MeshStandardMaterial({ color: 0x171310, roughness: .3, metalness: .15 });

function clawdBox(w, h, d, r, mat, x, y, z, parent) {
  const geo = new THREE.ExtrudeGeometry(roundedShape(w, h, r), {
    depth: d, bevelEnabled: true, bevelSize: .012, bevelThickness: .012, bevelSegments: 3, curveSegments: 10
  });
  geo.center();
  const m = new THREE.Mesh(geo, mat);
  m.position.set(x, y, z);
  (parent || clawd).add(m);
  return m;
}

const CW = 1.06, CH = .66, CD = .5, CLEG = .2, CBODY = CLEG + CH / 2;
clawdBox(CW, CH, CD, .17, clawdMat, 0, CBODY, 0);
[-.35, -.19, .19, .35].forEach(lx => clawdBox(.13, CLEG + .02, .19, .055, clawdMat, lx, CLEG / 2, 0));
const eyeL = clawdBox(.07, .22, .07, .03, clawdEyeMat, -.25, CBODY + .015, CD / 2);
const eyeR = clawdBox(.07, .22, .07, .03, clawdEyeMat, .25, CBODY + .015, CD / 2);
const armL = new THREE.Group(); armL.position.set(-(CW / 2 + .065), CBODY + .16, 0); clawd.add(armL);
clawdBox(.13, .24, .22, .06, clawdMat, 0, -.115, 0, armL);
const armR = new THREE.Group(); armR.position.set(CW / 2 + .065, CBODY + .16, 0); clawd.add(armR);
clawdBox(.13, .24, .22, .06, clawdMat, 0, -.115, 0, armR);

const bc = document.createElement('canvas'); bc.width = 512; bc.height = 170;
const bx = bc.getContext('2d');
bx.fillStyle = '#f4f6fb'; bx.beginPath(); bx.roundRect(8, 8, 496, 120, 40); bx.fill();
bx.beginPath(); bx.moveTo(430, 120); bx.lineTo(486, 120); bx.lineTo(480, 160); bx.closePath(); bx.fill();
bx.fillStyle = '#0b0b10'; bx.font = '700 46px Inter, "Segoe UI", sans-serif'; bx.textAlign = 'center'; bx.fillText('Hi! Pick any model.', 256, 82);
const bubbleTex = new THREE.CanvasTexture(bc); bubbleTex.colorSpace = THREE.SRGBColorSpace;
const bubble = new THREE.Mesh(new THREE.PlaneGeometry(2.1, 2.1 * 170 / 512), new THREE.MeshBasicMaterial({ map: bubbleTex, transparent: true, opacity: 0, toneMapped: false }));
bubble.position.set(.15, 2.9, .3); bubble.visible = false; win.add(bubble);

/* dark grid floor + dim particles */
const grid = new THREE.GridHelper(70, 70, 0x1c1c25, 0x101016);
grid.position.y = -3.6; scene.add(grid);

const N = 800, pos = new Float32Array(N * 3), col = new Float32Array(N * 3);
const pal = [0x3a3a48, 0x3a3a48, 0x3a3a48, 0xfebc2e, 0x28c840].map(c => new THREE.Color(c));
for (let i = 0; i < N; i++) {
  pos[i * 3] = (Math.random() - .5) * 44; pos[i * 3 + 1] = (Math.random() - .5) * 30; pos[i * 3 + 2] = (Math.random() - .5) * 40 - 8;
  const c = pal[(Math.random() * pal.length) | 0]; col.set([c.r, c.g, c.b], i * 3);
}
const pg = new THREE.BufferGeometry();
pg.setAttribute('position', new THREE.BufferAttribute(pos, 3));
pg.setAttribute('color', new THREE.BufferAttribute(col, 3));
const points = new THREE.Points(pg, new THREE.PointsMaterial({ size: .035, vertexColors: true, transparent: true, opacity: .55, depthWrite: false }));
scene.add(points);

/* ================= scroll -> demo progress ================= */
const demo = document.getElementById('demo');
// section index (0..3 = the four demo steps) -> demo timeline position
const KEYS = [[-.7, 0], [0, .13], [1, .40], [2, .66], [3, .93], [3.7, 1]];
function mapS(s) {
  if (s <= KEYS[0][0]) return 0;
  for (let i = 0; i < KEYS.length - 1; i++) {
    const [a, pa] = KEYS[i], [b, pb] = KEYS[i + 1];
    if (s <= b) return lerp(pa, pb, (s - a) / (b - a));
  }
  return 1;
}
const modelsEl = document.getElementById('models'), buildEl = document.getElementById('build');
let demoT = 0, demoP = 0, peekT = 0, peek = 0, termVT = 0, termV = 0, termPT = 0, termP = 0;
function onScroll() {
  const top = demo.getBoundingClientRect().top + scrollY;
  demoT = mapS((scrollY - top) / innerHeight);
  const vh = innerHeight;
  const ms = (scrollY - (modelsEl.getBoundingClientRect().top + scrollY)) / vh;
  peekT = clamp((.85 - Math.abs(ms)) / .35);
  const bs = (scrollY - (buildEl.getBoundingClientRect().top + scrollY)) / vh;
  termVT = seg(bs, -.6, -.2) * (1 - seg(bs, 1.6, 2.0));
  termPT = seg(bs, -.2, 1.6);
}
addEventListener('scroll', onScroll, { passive: true });

/* ================= text reveal on the left ================= */
document.querySelectorAll('[data-split]').forEach(el => {
  const words = el.textContent.trim().split(/\s+/);
  el.innerHTML = words.map((w, i) => `<span class="w"><span style="transition-delay:${i * 70 + 80}ms">${w}</span></span>`).join(' ');
});
// Reveal a section once its copy reaches the middle band of the viewport.
// A fixed ratio threshold would never fire for blocks taller than the screen
// (e.g. the stacked tool list on phones), so gate on intersection instead.
const io = new IntersectionObserver(es => es.forEach(e => e.target.closest('.step').classList.toggle('in', e.isIntersecting)), { threshold: 0, rootMargin: '-10% 0px -10% 0px' });
document.querySelectorAll('.step .copy').forEach(s => io.observe(s));

/* ================= interaction + loop ================= */
const mouse = { x: 0, y: 0 };
addEventListener('pointermove', e => { mouse.x = e.clientX / innerWidth - .5; mouse.y = e.clientY / innerHeight - .5; });
addEventListener('resize', () => {
  renderer.setSize(innerWidth, innerHeight);
  camera.aspect = innerWidth / innerHeight; camera.updateProjectionMatrix(); onScroll();
});
onScroll();

const clock = new THREE.Clock();
let frameN = 0;
function frame() {
  const t = clock.getElapsedTime();
  demoP += (demoT - demoP) * .1;
  peek += (peekT - peek) * .08; termV += (termVT - termV) * .12; termP += (termPT - termP) * .1;

  const visH = 2 * camera.position.z * Math.tan(THREE.MathUtils.degToRad(camera.fov / 2));
  const visW = visH * camera.aspect;
  const narrow = innerWidth < 900;
  let sc, x, y;
  if (narrow) { sc = Math.min(1, visW * .94 / WW); x = 0; y = visH * .2; }
  else { sc = Math.min(1, visW * .5 * .88 / WW); x = visW * .25; y = 0; }
  win.scale.setScalar(sc);
  win.position.set(x, y + Math.sin(t * .8) * .06, 0);
  win.rotation.y = (narrow ? 0 : -.26) + mouse.x * .12;
  win.rotation.x = mouse.y * .08;

  const pe = ease(peek);
  clawd.visible = pe > .01;
  clawd.position.set(1.58, lerp(.8, 1.62, pe) + Math.sin(t * 2.4) * .025 * pe, -.42);
  clawd.rotation.y = .2 + Math.sin(t * .8) * .07 * pe;
  clawd.rotation.z = Math.sin(t * 1.6) * .02 * pe;
  armL.rotation.z = -(.12 + Math.sin(t * 1.7) * .06 * pe);
  armR.rotation.z = .12 + Math.sin(t * 1.7 + 1) * .06 * pe + pe * (2.05 + Math.sin(t * 8) * .32);
  const eb = (t % 3.4) > 3.26 ? .08 : 1; eyeL.scale.y = eyeR.scale.y = eb;
  const ba = clamp((pe - .5) * 2); bubble.material.opacity = ba; bubble.scale.setScalar(.8 + .2 * ba); bubble.visible = ba > .01;

  if (frameN++ % 2 === 0) { drawScreen(demoP, t, termV, termP); tex.needsUpdate = true; }

  points.rotation.y = t * .01 + mouse.x * .08;
  points.rotation.x = mouse.y * .05;
  key.position.x = 4 + Math.sin(t * .5) * 3;

  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}
frame();
