// Claude Limit Sync
//
// A tray-only helper. You sign in to claude.ai yourself in its own window (the password and cookies stay
// inside this app's private browser profile; they are never written to a file, logged or sent anywhere
// else; keepSignIn below only extends their lifetime inside that profile). Every minute it asks
// claude.ai for the same plan-usage numbers its Settings > Usage page shows and writes them to
// %LOCALAPPDATA%\ClaudeLimitWidget\usage.json, which BatteryHub shows on the taskbar.
//
// The usage endpoint is claude.ai's internal one (not a documented API), so a claude.ai change can break it.

const { app, BrowserWindow, Tray, Menu, nativeImage, session } = require('electron');
const fs = require('fs');
const path = require('path');

const PARTITION = 'persist:claude';
const BASE = 'https://claude.ai';
const USAGE_PAGE = BASE + '/settings/usage';
const DATA_DIR = path.join(process.env.LOCALAPPDATA || app.getPath('userData'), 'ClaudeLimitWidget');
const DATA_FILE = path.join(DATA_DIR, 'usage.json');
const POLL_MS = 60 * 1000;
const RELOAD_MS = 60 * 60 * 1000;

let tray = null;
let worker = null;     // hidden window kept on claude.ai; requests run inside it like the site's own
let loginWin = null;
let lastStatus = 'Başlatılıyor…';
let lastOk = 0;
let loadedAt = 0;

if (!app.requestSingleInstanceLock()) app.quit();
app.on('second-instance', () => openLogin());
app.on('window-all-closed', (e) => e.preventDefault());  // stay in the tray

function setStatus(s) {
  lastStatus = s;
  try {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    fs.appendFileSync(path.join(DATA_DIR, 'sync.log'), new Date().toISOString() + ' ' + s + '\n');
  } catch (_) {}
  if (tray) {
    tray.setToolTip('Claude limit: ' + s);
    tray.setContextMenu(buildMenu());
  }
}

function buildMenu() {
  return Menu.buildFromTemplate([
    { label: lastStatus, enabled: false },
    { type: 'separator' },
    { label: 'Şimdi yenile', click: () => poll(true) },
    { label: 'claude.ai’ye giriş yap…', click: () => openLogin() },
    {
      label: 'Windows ile başlat',
      type: 'checkbox',
      checked: app.getLoginItemSettings().openAtLogin,
      click: (mi) => app.setLoginItemSettings({ openAtLogin: mi.checked, args: [] }),
    },
    { type: 'separator' },
    { label: 'Çıkış', click: () => app.exit(0) },
  ]);
}

function toEpoch(v) {
  if (v == null) return 0;
  if (typeof v === 'number') return v > 1e12 ? Math.round(v / 1000) : Math.round(v);
  const t = Date.parse(v);
  return Number.isFinite(t) ? Math.round(t / 1000) : 0;
}

// Accepts both shapes claude.ai has used: a `limits` list ({kind, percent, resets_at}) and flat
// `five_hour` / `seven_day` objects ({utilization, resets_at}).
function parseUsage(u) {
  const out = { seven_day: null, five_hour: null };
  const pick = (pct, reset) =>
    pct == null || !Number.isFinite(Number(pct)) ? null : { used_percentage: Number(pct), resets_at: toEpoch(reset) };
  if (Array.isArray(u && u.limits)) {
    for (const l of u.limits) {
      if (l.kind === 'weekly_all') out.seven_day = pick(l.percent ?? l.utilization, l.resets_at);
      else if (l.kind === 'session') out.five_hour = pick(l.percent ?? l.utilization, l.resets_at);
    }
  }
  if (!out.seven_day && u && u.seven_day) out.seven_day = pick(u.seven_day.utilization ?? u.seven_day.used_percentage, u.seven_day.resets_at);
  if (!out.five_hour && u && u.five_hour) out.five_hour = pick(u.five_hour.utilization ?? u.five_hour.used_percentage, u.five_hour.resets_at);
  return out;
}

function write(data) {
  fs.mkdirSync(DATA_DIR, { recursive: true });
  const tmp = DATA_FILE + '.' + process.pid + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify({ ...data, updated_at: Date.now() / 1000, source: 'claude.ai' }));
  fs.renameSync(tmp, DATA_FILE);
}

// claude.ai hands out its sign-in as session cookies, which Chromium drops when the app exits (a restart).
// Keep them for 30 days in this app's own profile, like "remember me", so a reboot needs no new sign-in.
// Values never leave the cookie store; only how many were kept is logged.
async function keepSignIn() {
  const ses = session.fromPartition(PARTITION);
  const list = await ses.cookies.get({});
  let n = 0;
  for (const c of list) {
    if (!c.session || !/(^|\.)claude\.ai$/.test(c.domain.replace(/^\./, ''))) continue;
    const host = c.domain.replace(/^\./, '');
    const d = {
      url: 'https://' + host + (c.path || '/'), name: c.name, value: c.value, path: c.path, secure: c.secure,
      httpOnly: c.httpOnly, expirationDate: Math.floor(Date.now() / 1000) + 30 * 86400,
    };
    if (!c.hostOnly) d.domain = c.domain;
    if (c.sameSite && c.sameSite !== 'unspecified') d.sameSite = c.sameSite;
    try { await ses.cookies.set(d); n++; } catch (_) {}
  }
  await ses.cookies.flushStore();
  return n;
}

function ensureWorker() {
  if (worker && !worker.isDestroyed() && Date.now() - loadedAt < RELOAD_MS) return Promise.resolve();
  if (!worker || worker.isDestroyed()) {
    worker = new BrowserWindow({ show: false, webPreferences: { partition: PARTITION } });
  }
  loadedAt = Date.now();
  return worker.loadURL(USAGE_PAGE).catch(() => {});
}

// Runs inside claude.ai, so cookies and any browser check are handled exactly as for the site itself.
const FETCH_JS = `(async () => {
  const j = async (u) => { const r = await fetch(u, { credentials: 'include', headers: { accept: 'application/json' } });
                           return { status: r.status, body: r.ok ? await r.json() : null }; };
  const orgs = await j('/api/organizations');
  if (!orgs.body) return { error: 'orgs', status: orgs.status };
  const list = Array.isArray(orgs.body) ? orgs.body : [];
  const org = list.find(o => (o.capabilities || []).includes('chat')) || list[0];
  if (!org) return { error: 'noorg' };
  const usage = await j('/api/organizations/' + org.uuid + '/usage');
  if (!usage.body) return { error: 'usage', status: usage.status };
  return { usage: usage.body };
})()`;

async function poll(manual) {
  try {
    await ensureWorker();
    const url = worker.webContents.getURL();
    if (!url) {  // page did not load (network not ready right after boot): try again shortly
      setStatus('Bağlantı bekleniyor…');
      loadedAt = 0;
      setTimeout(() => poll(false), 15000);
      return;
    }
    if (!url.startsWith(BASE) || /\/login/.test(url)) {
      setStatus('Giriş gerekli');
      if (manual || !lastOk) openLogin();
      return;
    }
    const r = await worker.webContents.executeJavaScript(FETCH_JS, true);
    if (r && r.usage) {
      const d = parseUsage(r.usage);
      if (!d.seven_day && !d.five_hour) { setStatus('Yanıtta limit bulunamadı'); return; }
      write(d);
      if (!lastOk) {
        const kept = await keepSignIn().catch(() => -1);
        setStatus('Oturum kalıcı yapıldı (' + kept + ' çerez)');
      }
      lastOk = Date.now();
      const w = d.seven_day ? `haftalık %${Math.round(d.seven_day.used_percentage)}` : 'haftalık —';
      const f = d.five_hour ? ` · 5 saat %${Math.round(d.five_hour.used_percentage)}` : '';
      setStatus(w + f);
    } else if (r && (r.status === 401 || r.status === 403)) {
      setStatus('Giriş gerekli');
      loadedAt = 0;
      if (manual || !lastOk) openLogin();
    } else {
      setStatus('Okunamadı (' + ((r && (r.error + ' ' + (r.status || ''))) || 'bilinmiyor') + ')');
      loadedAt = 0;
    }
  } catch (e) {
    setStatus('Hata: ' + String(e.message || e).slice(0, 60));
    loadedAt = 0;
  }
}

function openLogin() {
  if (loginWin && !loginWin.isDestroyed()) { loginWin.show(); loginWin.focus(); return; }
  loginWin = new BrowserWindow({
    width: 480, height: 720, title: 'claude.ai’ye giriş yap — Claude Limit Sync', autoHideMenuBar: true,
    webPreferences: { partition: PARTITION },
  });
  loginWin.loadURL(BASE + '/login');
  // Once claude.ai leaves the login pages, the session is in the profile: close and read the usage.
  const check = (_e, url) => {
    if (url.startsWith(BASE) && !/\/(login|magic-link|sso|verify)/.test(url)) {
      setTimeout(() => { if (loginWin && !loginWin.isDestroyed()) loginWin.close(); loadedAt = 0; poll(false); }, 1500);
    }
  };
  loginWin.webContents.on('did-navigate', check);
  loginWin.webContents.on('did-navigate-in-page', check);
  loginWin.on('closed', () => { loginWin = null; });
}

app.on('before-quit', () => { session.fromPartition(PARTITION).cookies.flushStore().catch(() => {}); });

app.whenReady().then(() => {
  app.setAppUserModelId('ClaudeLimitSync');
  const icon = nativeImage.createFromPath(path.join(__dirname, 'icon.png'));
  tray = new Tray(icon);
  tray.on('click', () => tray.popUpContextMenu());
  setStatus('Başlatılıyor…');
  poll(false);
  setInterval(() => poll(false), POLL_MS);
});
