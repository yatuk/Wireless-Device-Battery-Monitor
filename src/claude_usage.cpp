#include "claude_usage.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

constexpr double kStaleSeconds = 6 * 3600;

std::wstring DataFile() {
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    return std::wstring(base) + L"\\ClaudeLimitWidget\\usage.json";
}

bool ReadAll(const std::wstring& path, std::string& out, FILETIME& mtime) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    GetFileTime(h, nullptr, nullptr, &mtime);
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 64 * 1024;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

// Tiny lookup for the flat file the statusLine script writes:
// {"seven_day": {"used_percentage": 41.2, "resets_at": 1795457010}, "five_hour": {...}, "updated_at": 1795197810.7}
// A window is either an object or null.
bool Number(const std::string& s, size_t from, size_t to, const char* key, double& v) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos || p >= to) return false;
    const char* start = s.c_str() + p + 1;
    char* end = nullptr;
    double d = strtod(start, &end);
    if (end == start) return false;
    v = d;
    return true;
}

bool Window(const std::string& s, const char* name, double& pct, long long& resets) {
    std::string k = std::string("\"") + name + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    size_t open = s.find_first_not_of(" \t\r\n:", p + k.size());
    if (open == std::string::npos || s[open] != '{') return false;  // null
    size_t close = s.find('}', open);
    if (close == std::string::npos) return false;
    double v = -1, r = 0;
    if (!Number(s, open, close, "used_percentage", v)) return false;
    Number(s, open, close, "resets_at", r);
    pct = v;
    resets = (long long)r;
    return true;
}

const wchar_t* kDays[] = { L"Paz", L"Pzt", L"Sal", L"Çar", L"Per", L"Cum", L"Cmt" };

std::wstring ResetText(long long ts) {
    if (ts <= 0) return L"";
    long long left = ts - (long long)time(nullptr);
    if (left <= 0) return L"sıfırlandı";
    wchar_t buf[64];
    if (left < 86400) {
        int h = (int)(left / 3600), m = (int)(left % 3600 / 60);
        if (h) swprintf(buf, 64, L"%d sa %d dk sonra sıfırlanır", h, m);
        else swprintf(buf, 64, L"%d dk sonra sıfırlanır", m);
        return buf;
    }
    time_t t = (time_t)ts;
    tm lt{};
    localtime_s(&lt, &t);
    swprintf(buf, 64, L"%ls %02d:%02d sıfırlanır", kDays[lt.tm_wday], lt.tm_hour, lt.tm_min);
    return buf;
}

FILETIME g_lastMtime{};
bool g_haveFile = false;
ClaudeUsage g_cached;

}  // namespace

namespace claude_usage {

bool Poll(ClaudeUsage& out) {
    std::wstring path = DataFile();
    std::string body;
    FILETIME mt{};
    bool have = !path.empty() && ReadAll(path, body, mt);
    ClaudeUsage u = g_cached;
    if (!have) {
        u = ClaudeUsage{};
    } else if (!g_haveFile || CompareFileTime(&mt, &g_lastMtime) != 0) {
        u = ClaudeUsage{};
        long long r = 0;
        double v = -1;
        if (Window(body, "seven_day", v, r)) { u.week = v; u.weekResets = r; u.ok = true; }
        v = -1; r = 0;
        if (Window(body, "five_hour", v, r)) { u.fiveHour = v; u.fiveResets = r; }
        double up = 0;
        if (Number(body, 0, body.size(), "updated_at", up)) u.updatedAt = up;
        g_lastMtime = mt;
    }
    g_haveFile = have;

    // Windows that rolled over since the last statusLine run start again from zero.
    long long now = (long long)time(nullptr);
    if (u.ok && u.weekResets > 0 && u.weekResets <= now) u.week = 0;
    if (u.fiveHour >= 0 && u.fiveResets > 0 && u.fiveResets <= now) u.fiveHour = 0;
    u.stale = u.ok && u.updatedAt > 0 && (double)now - u.updatedAt > kStaleSeconds;

    bool changed = u.ok != g_cached.ok || std::lround(u.week) != std::lround(g_cached.week) || u.stale != g_cached.stale ||
                   std::lround(u.fiveHour) != std::lround(g_cached.fiveHour) || u.weekResets != g_cached.weekResets;
    g_cached = u;
    out = u;
    return changed;
}

std::wstring Tooltip(const ClaudeUsage& u) {
    if (!u.ok) return L"Claude haftalık limit: veri yok (Claude Code'u terminalde bir kez çalıştır)";
    wchar_t buf[96];
    swprintf(buf, 96, L"Claude haftalık limit: %%%ld", std::lround(u.week));
    std::wstring t = buf;
    std::wstring r = ResetText(u.weekResets);
    if (!r.empty()) t += L" · " + r;
    if (u.fiveHour >= 0) {
        swprintf(buf, 96, L" · 5 saat: %%%ld", std::lround(u.fiveHour));
        t += buf;
    }
    if (u.stale) t += L" (eski veri)";
    return t;
}

}  // namespace claude_usage
