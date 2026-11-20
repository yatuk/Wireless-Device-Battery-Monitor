// BatteryHub - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "device_aliases.h"
#include "alert_rules.h"
#include "devices.h"
#include "battery_history.h"
#include "i18n.h"
#include "json_output.h"
#include "labels.h"
#include "report.h"
#include "rename_dialog.h"
#include "widget.h"
#include "claude_usage.h"
#include <cmath>
#include "update_win.h"
#include "update_apply.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_ALERT_ICON = 3;
// "Check battery every" choices in seconds; 60 is the default. A poll resets an awake device's sleep timer,
// so a device only sleeps if its timer is shorter than the interval.
constexpr DWORD POLL_SECONDS[] = { 30, 60, 120, 300, 600, 900, 1800, 3600 };
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT ALERT_ICON_ID = 2;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT, ID_REPORT,
    ID_BRAND_COLORS, ID_STACKED, ID_SHOW_DISCONNECTED, ID_RECORD_HISTORY, ID_DIM_SLEEPING,
    ID_POLL_FIRST = 500,  // "Check battery every": ID_POLL_FIRST + 0..7
    ID_UPDATE = 200, ID_AUTO_UPDATE,
    ID_LANG_AUTO = 400, ID_LANG_ADD, ID_LANG_FIRST,  // "Language": ID_LANG_FIRST + 0..63
    ID_MONITOR_ALL = 300, ID_MONITOR_MAIN, ID_MONITOR_FIRST,  // "Show the widget on": ID_MONITOR_FIRST + 0..31
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT {
    DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_RENAME, DEV_RESET_NAME, DEV_HISTORY,
    DEV_ALERT_FIRST = 6, DEV_ALERT_DEFAULT = DEV_ALERT_FIRST + 8, DEV_ALERT_OFF, DEV_STRIDE = 16
};

const wchar_t* kAppName = L"BatteryHub";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_showDisconnectedDevices = false;  // opt-in, explicit driver-reported offline devices only
bool g_dimSleeping = true;  // dim a sleeping device's last known value (menu option, on by default)
bool g_recordHistory = true;  // save readings for "Battery history..." (menu option, on by default)
std::wstring g_widgetMonitor = L"all";  // which taskbars show the widget: "all", "main" or one monitor id
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_stacked = false;      // icon above the percentage (menu option)
bool g_brandColors = false;  // off by default; known brands get their color (Razer green, Logitech blue, ...)
std::atomic<DWORD> g_pollIntervalMs{ 60 * 1000 };  // set from the menu, read by the worker thread

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
std::map<std::wstring, bool> g_alertLatches;  // survive transient removal of a device
bool g_alertIconAdded = false;  // short-lived tray icon for widget-mode notifications
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;

// User-supplied names are presentation-only, keyed by stable DeviceStatus.key.
// Driver identity, HID polling, brand detection and device diagnostics are unchanged.
aliases::AliasMap g_aliases;

std::wstring DisplayName(const TrayEntry& e) {
    return aliases::DisplayName(g_aliases, e.st.key, e.st.name);
}

void LoadAliases() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS) return;
    if (size < 2 * sizeof(wchar_t) || size > 128 * 1024 || size % sizeof(wchar_t)) return;
    std::vector<wchar_t> data(size / sizeof(wchar_t), 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, data.data(), &size) == ERROR_SUCCESS)
        g_aliases = aliases::Decode(data.data(), size / sizeof(wchar_t));
}

void SaveAliases() {
    auto data = aliases::Encode(g_aliases);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", REG_MULTI_SZ,
                    data.data(), (DWORD)(data.size() * sizeof(wchar_t)));
}

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    if (IsHidden(e.st.key)) return true;
    // An explicitly offline device is not just a stale battery reading.
    if (g_showDisconnectedDevices && !e.st.connected) return false;
    return g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0));
}

bool ShouldDim(const DeviceStatus& s) {
    // An explicitly offline device keeps its dimmed "x", whatever the sleeping-device choice is.
    if (!s.connected) return true;
    // A sleeping device has no fresh reading: keep the last one, optionally at full brightness.
    if (s.asleep && !g_dimSleeping) return false;
    return s.percent < 0;
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

// A REG_MULTI_SZ with one "device-key<TAB>threshold" entry per override.
// Threshold -1 disables alerts; a missing key uses the default 15%.
std::map<std::wstring, int> g_alertOverrides;

void LoadAlertThresholds() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < 2 * sizeof(wchar_t) || size > 128 * 1024) return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    size_t limit = size / sizeof(wchar_t);
    for (size_t i = 0; i < limit && buf[i];) {
        size_t end = i;
        while (end < limit && buf[end]) ++end;
        if (end == limit) break;
        std::wstring item(buf.data() + i, end - i);
        size_t sep = item.rfind(L'\t');
        if (sep != std::wstring::npos && sep != 0) {
            std::wstring value = item.substr(sep + 1);
            wchar_t* tail = nullptr;
            long n = std::wcstol(value.c_str(), &tail, 10);
            if (tail != value.c_str() && *tail == 0 && n >= -1 && n <= 40 && alerts::ValidOverride((int)n))
                g_alertOverrides[item.substr(0, sep)] = (int)n;
        }
        i = end + 1;
    }
}

void SaveAlertThresholds() {
    std::wstring multi;
    for (const auto& [key, threshold] : g_alertOverrides) {
        multi += key + L"\t" + std::to_wstring(threshold);
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    if (multi.size() == 1) multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", REG_MULTI_SZ,
                    multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadShowDisconnectedDevices() { return LoadBoolSetting(L"ShowDisconnectedDevices", false); }
void SaveShowDisconnectedDevices(bool on) { SaveBoolSetting(L"ShowDisconnectedDevices", on); }
bool LoadDimSleeping() { return LoadBoolSetting(L"DimSleepingDevices", true); }
void SaveDimSleeping(bool on) { SaveBoolSetting(L"DimSleepingDevices", on); }
bool LoadRecordHistory() { return LoadBoolSetting(L"RecordBatteryHistory", true); }
void SaveRecordHistory(bool on) { SaveBoolSetting(L"RecordBatteryHistory", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }
std::wstring LoadWidgetMonitor() {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
        return buf;
    return LoadBoolSetting(L"OtherTaskbars", true) ? L"all" : L"main";  // the earlier on/off toggle
}
void SaveWidgetMonitor(const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetWidgetMonitor(const std::wstring& v) {
    g_widgetMonitor = v;
    SaveWidgetMonitor(v);
    widget::SetMonitorChoice(v);
}
bool LoadStacked() { return LoadBoolSetting(L"StackedLayout", false); }
void SaveStacked(bool on) { SaveBoolSetting(L"StackedLayout", on); }
bool LoadBrandColors() { return LoadBoolSetting(L"BrandColors", false); }
void SaveBrandColors(bool on) { SaveBoolSetting(L"BrandColors", on); }
DWORD LoadPollSeconds() {
    DWORD v = 60, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return std::clamp<DWORD>(v, 30, 24 * 60 * 60);
}
void SavePollSeconds(DWORD seconds) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", REG_DWORD, &seconds, sizeof(seconds));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

Accent AccentFor(const DeviceStatus& s) { return g_brandColors ? BrandOf(s) : Accent::None; }

HICON IconFor(int pct, bool charging, bool dim, Accent accent) {
    Rgb fg = Foreground();
    BrandColor bc = AccentColor(accent, g_lightTaskbar);
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
            : accent != Accent::None ? Rgb{ bc.r, bc.g, bc.b }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return T(L"off / not connected");
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = T(s.levelText);
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = TF(L"{percent}% (last known)", { { L"percent", std::to_wstring(e.lastPct) } });
    else t = T(L"unknown");
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.full || (s.charging && s.percent >= 100)) t += std::wstring(L", ") + T(L"fully charged");
    else if (s.charging) t += std::wstring(L", ") + T(L"charging");
    if (s.asleep) t += std::wstring(L", ") + T(L"asleep");
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowBatteryAlert(TrayEntry& e) {
    const std::wstring title = T(L"Low battery");
    const std::wstring message = TF(L"{name} is at {percent}%",
                                   { { L"name", DisplayName(e) }, { L"percent", std::to_wstring(e.st.percent) } });
    if (!g_useWidget && e.added) {
        TrayCall(NIM_MODIFY, e.uid, e.icon, DisplayName(e) + L"\n" + StatusText(e), title.c_str(), message);
        return;
    }
    // Windows notification balloons need a tray icon. Keep a temporary one in widget mode
    // long enough for the shell to display the notification, then remove it.
    if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
    TrayCall(g_alertIconAdded ? NIM_MODIFY : NIM_ADD, ALERT_ICON_ID, g_placeholderIcon,
             L"BatteryHub", title.c_str(), message);
    g_alertIconAdded = true;
    SetTimer(g_hwnd, TIMER_ALERT_ICON, 60 * 1000, nullptr);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, std::wstring(L"BatteryHub\n") + T(L"No devices with battery info found"));
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = !g_useWidget && !IsEntryHidden(e) && (s.connected || g_showDisconnectedDevices);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    // Never display an old percentage as a live reading for an explicitly offline device.
    int shown = !s.connected ? -1 : (s.percent >= 0 ? s.percent : e.lastPct);
    bool dim = ShouldDim(s);
    // Fully charged on power: the normal green bar instead of the blue charging one.
    bool charging = s.connected && !s.asleep && s.charging && !s.full && shown < 100;
    Accent accent = AccentFor(s);
    std::wstring sig = std::to_wstring(shown) + (charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"a" + std::to_wstring((int)accent) + (s.connected ? L"|on" : L"|off");
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = s.connected ? IconFor(shown, charging, dim, accent)
                             : RenderIcon(L"\u00D7", -1, Foreground(), Foreground(), true);
        e.iconSig = sig;
    }
    std::wstring tip = DisplayName(e) + L"\n" + StatusText(e);

    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return (e.st.connected || g_showDisconnectedDevices) && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        if (ka != kb) return ka < kb;
        return DisplayName(*a) != DisplayName(*b) ? DisplayName(*a) < DisplayName(*b) : a->st.key < b->st.key;  // same name: keep a fixed order
    });
    return list;
}

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    // Two devices of one kind share an icon: give each a tiny caption so they can be told apart.
    std::vector<LabelInput> labelIn;
    for (auto* e : list) labelIn.push_back({ e->st.kind, DisplayName(*e) });
    std::vector<std::wstring> labels = ShortLabels(labelIn, 3);
    std::vector<WidgetItem> items;
    std::wstring tip;
    // Claude weekly usage limit, first (leftmost) in the strip. Red from 90% on, dimmed when unknown or stale.
    {
        ClaudeUsage cu;
        claude_usage::Poll(cu);
        WidgetItem it;
        it.claude = true;
        if (cu.ok) {
            it.percent = (int)std::lround(cu.week);
            it.low = it.percent >= 90;
        } else {
            it.text = L"–";
        }
        it.dim = !cu.ok || cu.stale;
        items.push_back(it);
        tip = claude_usage::Tooltip(cu);
    }
    labels.insert(labels.begin(), std::wstring());
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        bool offline = !e->st.connected;
        it.percent = offline ? -1 : (e->st.percent >= 0 ? e->st.percent : e->lastPct);
        if (offline) it.text = L"\u00D7";  // clear status, not a stale percentage
        else if (e->st.percent >= 0) it.text = T(e->st.levelText);
        it.charging = !offline && !e->st.asleep && e->st.charging;
        it.full = !offline && (e->st.full || (e->st.charging && it.percent >= 100));
        it.dim = ShouldDim(e->st);
        it.low = !offline && !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        it.label = labels[items.size()];
        it.accent = AccentFor(e->st);
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += DisplayName(*e) + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = std::wstring(L"BatteryHub: ") + T(L"No devices with battery info found");
    widget::SetItems(items, tip);
}

void OnResults(std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        // Evaluate even in widget mode: ApplyEntry only manages tray icons.
        // Alert latches are kept by stable device key even if the driver briefly stops reporting it.
        if (const auto it = g_alertLatches.find(s.key); it != g_alertLatches.end())
            e.lowWarned = it->second;
        bool notify = !IsHidden(s.key) &&
                      alerts::ShouldNotify(s, alerts::EffectiveThreshold(g_alertOverrides, s.key), e.lowWarned);
        g_alertLatches[s.key] = e.lowWarned;
        if (g_alertLatches.size() > 256) g_alertLatches.erase(g_alertLatches.begin());
        if (g_recordHistory && s.connected && !s.asleep && s.percent >= 0)
            battery_history::Observe(s.key, s.percent, s.charging);
        ApplyEntry(e);
        if (notify) ShowBatteryAlert(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while
        // (at least two poll intervals).
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen <
                           std::max<ULONGLONG>((ULONGLONG)e.st.graceSec * 1000, 2ULL * g_pollIntervalMs);
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetShowDisconnectedDevices(bool on) {
    g_showDisconnectedDevices = on;
    SaveShowDisconnectedDevices(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetDimSleeping(bool on) {
    g_dimSleeping = on;
    SaveDimSleeping(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

// Switches the UI language ("" = follow Windows) and redraws everything that shows text.
void SetLanguage(const std::wstring& code);

void RedrawAll() {
    if (g_alertIconAdded) {  // alert icon shares the placeholder's HICON
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        TrayDelete(ALERT_ICON_ID);
        g_alertIconAdded = false;
    }
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void SetLanguage(const std::wstring& code) {
    i18n::SetChoice(code);
    RedrawAll();  // tray tooltips and the placeholder text
    UpdateWidget();
}

void SetStacked(bool on) {
    g_stacked = on;
    SaveStacked(on);
    widget::SetStacked(on);
}

void SetBrandColors(bool on) {
    g_brandColors = on;
    SaveBrandColors(on);
    RedrawAll();
    UpdateWidget();
}

void SetPollSeconds(DWORD seconds) {
    g_pollIntervalMs = seconds * 1000;
    SavePollSeconds(seconds);
    SetEvent(g_refreshEvt);  // poll now, so the new interval starts right away
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

void RenameDevice(const std::wstring& key) {
    auto it = g_entries.find(key);
    if (it == g_entries.end()) return;
    const std::wstring originalName = it->second.st.name;
    std::wstring chosen;
    if (!PromptDeviceName(g_hwnd, DisplayName(it->second), chosen)) return;
    if (chosen == originalName) g_aliases.erase(key);
    else if (g_aliases.count(key) || g_aliases.size() < aliases::kMaxAliases) g_aliases[key] = chosen;
    else return;  // bounded registry value; don't silently drop other users' names
    SaveAliases();
    // The modal editor pumps the message queue, so polling may remove the entry.
    // Resolve the iterator again rather than dereferencing a potentially stale one.
    it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void ResetDeviceName(const std::wstring& key) {
    if (!g_aliases.erase(key)) return;
    SaveAliases();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void SetAlertOverride(const std::wstring& key, int threshold) {
    if (threshold == alerts::kUseDefault) g_alertOverrides.erase(key);
    else if (alerts::ValidOverride(threshold)) g_alertOverrides[key] = threshold;
    else return;
    SaveAlertThresholds();
    g_alertLatches.erase(key);
    auto it = g_entries.find(key);
    if (it != g_entries.end()) it->second.lowWarned = false;
    SetEvent(g_refreshEvt);  // reconsider with a fresh reading; never alert from stale data
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"No devices with battery info found"));
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"Point at a device to show, hide or move it"));
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? T(L"Show on the taskbar") : T(L"Show in the tray"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? T(L"Move left") : T(L"Move up"));
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? T(L"Move right") : T(L"Move down"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, base + DEV_RENAME, T(L"Rename device..."));
        AppendMenuW(sub, MF_STRING | (g_aliases.count(k) ? 0 : MF_GRAYED),
                    base + DEV_RESET_NAME, T(L"Reset to original name"));
        HMENU alertMenu = CreatePopupMenu();
        int selectedThreshold = alerts::OverrideFor(g_alertOverrides, k);
        AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == alerts::kUseDefault ? MF_CHECKED : 0),
                    base + DEV_ALERT_DEFAULT, T(L"Use default (15%)"));
        AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == alerts::kDisabled ? MF_CHECKED : 0),
                    base + DEV_ALERT_OFF, T(L"Disable notifications"));
        AppendMenuW(alertMenu, MF_SEPARATOR, 0, nullptr);
        for (int i = 0; i < alerts::kThresholdCount; ++i) {
            int level = alerts::kThresholds[i];
            std::wstring label = std::to_wstring(level) + L"%";
            AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == level ? MF_CHECKED : 0),
                        base + DEV_ALERT_FIRST + i, label.c_str());
        }
        AppendMenuW(sub, MF_POPUP, (UINT_PTR)alertMenu, T(L"Battery alerts"));
        AppendMenuW(sub, MF_STRING, base + DEV_HISTORY, T(L"Battery history..."));
        std::wstring line = DisplayName(*e) + L":  " + StatusText(*e) + L"   [" + T(e->st.via) + L"]";
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, T(L"Hide last known / asleep devices"));
    AppendMenuW(m, MF_STRING | (g_showDisconnectedDevices ? MF_CHECKED : 0), ID_SHOW_DISCONNECTED, T(L"Show disconnected devices"));
    AppendMenuW(m, MF_STRING | (g_dimSleeping ? MF_CHECKED : 0), ID_DIM_SLEEPING, T(L"Dim sleeping devices"));
    AppendMenuW(m, MF_STRING | (g_recordHistory ? MF_CHECKED : 0), ID_RECORD_HISTORY, T(L"Record battery history"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, T(L"Show on the taskbar (icon + percentage)"));
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, T(L"Show as tray icons"));
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(m, bgFlags, ID_WIDGET_BACKGROUND, T(L"Background behind taskbar text (for transparent taskbars)"));
    AppendMenuW(m, MF_STRING | (g_stacked ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_STACKED, T(L"Icons above percentages (narrower)"));
    // With more than one monitor: choose which taskbars show the widget (all, main only, or one monitor).
    auto monitors = widget::OtherMonitors();
    if (monitors.size() > 31) monitors.resize(31);
    if (!monitors.empty()) {
        HMENU sub = CreatePopupMenu();
        UINT f = MF_STRING | (g_wantWidget ? 0 : MF_GRAYED);
        AppendMenuW(sub, f, ID_MONITOR_ALL, T(L"All monitors"));
        AppendMenuW(sub, f, ID_MONITOR_MAIN, T(L"Main monitor only"));
        UINT checked = g_widgetMonitor == L"main" ? ID_MONITOR_MAIN : ID_MONITOR_ALL;
        bool found = g_widgetMonitor == L"all" || g_widgetMonitor == L"main";
        for (UINT i = 0; i < monitors.size(); i++) {
            AppendMenuW(sub, f, ID_MONITOR_FIRST + i, TF(L"{monitor} only", { { L"monitor", monitors[i].label } }).c_str());
            if (monitors[i].id == g_widgetMonitor) { checked = ID_MONITOR_FIRST + i; found = true; }
        }
        if (!found) {  // the chosen monitor is unplugged: the main taskbar shows the widget meanwhile
            AppendMenuW(sub, MF_STRING | MF_GRAYED, ID_MONITOR_FIRST + 31, T(L"Chosen monitor (not connected, shown on main)"));
            checked = ID_MONITOR_FIRST + 31;
        }
        CheckMenuRadioItem(sub, ID_MONITOR_ALL, ID_MONITOR_FIRST + 31, checked, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP | (g_wantWidget ? 0 : MF_GRAYED), (UINT_PTR)sub, T(L"Show the widget on"));
    }
    AppendMenuW(m, MF_STRING | (g_brandColors ? MF_CHECKED : 0), ID_BRAND_COLORS, T(L"Brand colors (Razer green, Logitech blue, ...)"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Language: automatic, every built-in or user-added language (in its own name), and a way to add one.
    i18n::Init();  // picks up language files added since the last time
    auto languages = i18n::Available();
    if (languages.size() > 64) languages.resize(64);
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, ID_LANG_AUTO, T(L"Automatic (Windows language)"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        UINT checked = ID_LANG_AUTO;
        for (UINT i = 0; i < languages.size(); i++) {
            AppendMenuW(sub, MF_STRING, ID_LANG_FIRST + i, languages[i].name.c_str());
            if (!i18n::Choice().empty() && _wcsicmp(languages[i].code.c_str(), i18n::Choice().c_str()) == 0) checked = ID_LANG_FIRST + i;
        }
        CheckMenuRadioItem(sub, ID_LANG_AUTO, ID_LANG_FIRST + 63, checked, MF_BYCOMMAND);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, ID_LANG_ADD, T(L"Add your own language..."));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Language"));
    }
    {
        HMENU sub = CreatePopupMenu();
        const UINT count = ARRAYSIZE(POLL_SECONDS);
        UINT checked = 0;
        for (UINT i = 0; i < count; i++) {
            DWORD s = POLL_SECONDS[i];
            std::wstring label = s < 60 ? TF(L"{seconds} s", { { L"seconds", std::to_wstring(s) } })
                                        : TF(L"{minutes} min", { { L"minutes", std::to_wstring(s / 60) } });
            AppendMenuW(sub, MF_STRING, ID_POLL_FIRST + i, label.c_str());
            if (s * 1000 == g_pollIntervalMs) checked = ID_POLL_FIRST + i;
        }
        if (checked) CheckMenuRadioItem(sub, ID_POLL_FIRST, ID_POLL_FIRST + count - 1, checked, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Check battery every"));
    }
    AppendMenuW(m, MF_STRING, ID_REFRESH, T(L"Refresh now"));
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, T(L"Start with Windows"));
    // Local build: "Request support", "Support the project" and the update entries are removed from the menu.
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, T(L"Exit"));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (i18n::Rtl() ? TPM_LAYOUTRTL : 0), pt.x, pt.y, 0,
                              g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_SHOW_DISCONNECTED: SetShowDisconnectedDevices(!g_showDisconnectedDevices); break;
        case ID_DIM_SLEEPING: SetDimSleeping(!g_dimSleeping); break;
        case ID_RECORD_HISTORY: g_recordHistory = !g_recordHistory; SaveRecordHistory(g_recordHistory); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MONITOR_ALL: SetWidgetMonitor(L"all"); break;
        case ID_LANG_AUTO: SetLanguage(L""); break;
        case ID_LANG_ADD: i18n::OpenUserFolder(); break;
        case ID_MONITOR_MAIN: SetWidgetMonitor(L"main"); break;
        case ID_BRAND_COLORS: SetBrandColors(!g_brandColors); break;
        case ID_STACKED: SetStacked(!g_stacked); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_UPDATE: updates::Activate(g_hwnd); break;
        case ID_AUTO_UPDATE: updates::ToggleAutomatic(); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_POLL_FIRST && cmd < ID_POLL_FIRST + ARRAYSIZE(POLL_SECONDS)) {
                SetPollSeconds(POLL_SECONDS[cmd - ID_POLL_FIRST]);
            } else if (cmd >= ID_LANG_FIRST && cmd < ID_LANG_FIRST + languages.size()) {
                SetLanguage(languages[cmd - ID_LANG_FIRST].code);
            } else if (cmd >= ID_MONITOR_FIRST && cmd < ID_MONITOR_FIRST + monitors.size()) {
                SetWidgetMonitor(monitors[cmd - ID_MONITOR_FIRST].id);
            } else if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_RENAME: RenameDevice(key); break;
                    case DEV_RESET_NAME: ResetDeviceName(key); break;
                    case DEV_ALERT_DEFAULT: SetAlertOverride(key, alerts::kUseDefault); break;
                    case DEV_ALERT_OFF: SetAlertOverride(key, alerts::kDisabled); break;
                    case DEV_HISTORY: {
                        auto it = g_entries.find(key);
                        if (it != g_entries.end())
                            battery_history::Show(g_hwnd, key, DisplayName(it->second));
                        break;
                    }
                    default: {
                        UINT slot = (cmd - ID_DEVICE_FIRST) % DEV_STRIDE;
                        if (slot >= DEV_ALERT_FIRST && slot < DEV_ALERT_FIRST + alerts::kThresholdCount)
                            SetAlertOverride(key, alerts::kThresholds[slot - DEV_ALERT_FIRST]);
                        break;
                    }
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

// `--demo`: one sample device of every brand instead of real ones, for screenshots and for checking the
// brand colors without owning the hardware. Keys, vendor ids and names are mixed on purpose, so every way
// of recognising a brand gets used.
bool g_demo = false;

std::vector<DeviceStatus> DemoDevices() {
    auto make = [](const wchar_t* key, const wchar_t* name, DeviceKind kind, int pct, unsigned short vid, bool charging) {
        DeviceStatus s;
        s.key = key;
        s.name = name;
        s.kind = kind;
        s.percent = pct;
        s.vid = vid;
        s.charging = charging;
        s.via = L"demo";
        return s;
    };
    std::vector<DeviceStatus> devices = {
        make(L"razer:demo", L"Razer DeathAdder V3 Pro", DeviceKind::Mouse, 78, 0x1532, false),
        make(L"demo:logi", L"Logitech G PRO X 2 LIGHTSPEED", DeviceKind::Headset, 64, 0x046D, false),
        make(L"steelseries:demo", L"SteelSeries Arctis Nova 7", DeviceKind::Headset, 55, 0, false),
        make(L"hsc:1b1c:0a6b", L"Corsair HS80 RGB Wireless", DeviceKind::Headset, 81, 0x1B1C, false),
        make(L"hyperx:demo", L"HyperX Cloud III Wireless", DeviceKind::Headset, 47, 0, false),
        make(L"rog:demo", L"ROG Harpe Ace", DeviceKind::Mouse, 90, 0, false),
        make(L"zowie:demo", L"ZOWIE EC2-CW", DeviceKind::Mouse, 66, 0x04A5, false),
        make(L"bt:demo-ps", L"Wireless Controller", DeviceKind::Gamepad, 38, 0x054C, false),
        make(L"xbox:demo", L"Xbox Wireless Controller", DeviceKind::Gamepad, 72, 0x045E, false),
        make(L"bt:demo-buds", L"Galaxy Buds3 Pro", DeviceKind::Headset, 59, 0, false),
        make(L"airpods:demo", L"AirPods Pro", DeviceKind::Headset, 85, 0, false),
        make(L"jabra:demo", L"Jabra Evolve2 65", DeviceKind::Headset, 23, 0x0B0E, false),
        make(L"jbl:demo", L"JBL Quantum 910", DeviceKind::Headset, 70, 0x0ECB, false),
        make(L"coolermaster:demo", L"Cooler Master MM712", DeviceKind::Mouse, 12, 0x2516, false),
        make(L"steam:demo", L"Steam Controller", DeviceKind::Gamepad, 50, 0x28DE, true),
        make(L"bt:demo-plain", L"Bluetooth Headphones", DeviceKind::Headset, 100, 0, true),  // charging at 100%: fully charged
    };
    DeviceStatus offline = make(L"demo:offline-receiver", L"Offline wireless headset", DeviceKind::Headset, -1, 0, false);
    offline.via = L"2.4 GHz receiver";
    offline.connected = false;
    devices.push_back(offline);  // hidden by default, shown when the opt-in setting is enabled
    return devices;
}

DWORD WINAPI Worker(LPVOID) {
    // At sign-in a USB receiver may enumerate before its wireless link is ready.
    constexpr DWORD startupPollDelays[] = { 3000, 7000, 15000 };
    size_t startupPoll = 0;
    HANDLE events[2] = { g_stopEvt, g_refreshEvt };
    for (;;) {
        auto* v = new std::vector<DeviceStatus>(g_demo ? DemoDevices() : PollAllDevices());
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        DWORD delay = g_pollIntervalMs;
        if (startupPoll < ARRAYSIZE(startupPollDelays)) delay = startupPollDelays[startupPoll++];
        if (WaitForMultipleObjects(2, events, FALSE, delay) == WAIT_OBJECT_0) return 0;
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        g_alertIconAdded = false;
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshSoon(5000);
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_WIDGET) {
                Reconcile();
                updates::Tick(h);
                ClaudeUsage cu;
                static int claudeTick = 0;
                if (++claudeTick % 5 == 0 && claude_usage::Poll(cu)) UpdateWidget();  // every 5 s
            }
            if (wp == TIMER_ALERT_ICON) {
                KillTimer(h, TIMER_ALERT_ICON);
                if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            updates::Shutdown();
            KillTimer(h, TIMER_WIDGET);
            KillTimer(h, TIMER_ALERT_ICON);
            if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `BatteryHub.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"BatteryHub-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        Accent accent = BrandOf(s);
        std::wstring line = s.name + L"  [" + s.via + L"]  " +
                            (accent != Accent::None ? L"{" + std::wstring(StyleOf(accent).name) + L"}  " : L"");
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.full || (s.charging && s.percent >= 100)) line += L", fully charged";
        else if (s.charging) line += L", charging";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

// Machine-readable device list, shared with the Linux entry point (src/json_output.h).
int ListJson() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    std::string text = DevicesToJson(PollAllDevices());  // UTF-8
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, text.data(), (DWORD)text.size(), &written, nullptr);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--json")) return ListJson();
    if (cmdLine && wcsstr(cmdLine, L"--help")) {
        MessageBoxW(nullptr,
                    L"BatteryHub options:\n\n"
                    L"--list   Print devices and battery levels\n"
                    L"--json   Print a JSON array of devices and battery levels\n"
                    L"--dump   Print every HID collection and device\n"
                    L"--report Open the \"Request support for a device\" window\n"
                    L"--demo   Show sample devices\n"
                    L"--quit   Quit a running instance",
                    L"BatteryHub", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int applyResult = argv ? updates::RunApplyMode(argc, argv) : -1;
    if (argv) LocalFree(argv);
    if (applyResult >= 0) return applyResult;
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        i18n::Init();
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            HANDLE running = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
            if (!running) return 1;
            PostMessageW(w, WM_CLOSE, 0, 0);
            DWORD exited = WaitForSingleObject(running, 30000);
            CloseHandle(running);
            if (exited != WAIT_OBJECT_0) return 1;
        }
        return 0;
    }
    g_inst = inst;
    g_demo = cmdLine && wcsstr(cmdLine, L"--demo");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    updates::Initialize();

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_showDisconnectedDevices = LoadShowDisconnectedDevices();
    g_dimSleeping = LoadDimSleeping();
    g_recordHistory = LoadRecordHistory();
    g_widgetBackground = LoadWidgetBackground();
    g_brandColors = LoadBrandColors();
    g_stacked = LoadStacked();
    g_pollIntervalMs = LoadPollSeconds() * 1000;
    widget::SetStacked(g_stacked);
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    i18n::Init();
    g_widgetMonitor = LoadWidgetMonitor();
    widget::SetMonitorChoice(g_widgetMonitor);
    LoadHidden();
    LoadOrder();
    LoadAliases();
    LoadAlertThresholds();
    battery_history::Initialize(g_demo);
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
