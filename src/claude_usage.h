#pragma once
// Claude weekly usage limit, shown as one more item in the taskbar strip.
//
// The numbers come from %LOCALAPPDATA%\ClaudeLimitWidget\usage.json, written by either helper in claude/:
//  * claude/claude_limit_statusline.py: a Claude Code statusLine command, from the documented `rate_limits`
//    field of the statusLine payload (Claude Code in a terminal);
//  * claude/desktop-sync: a tray app that reads claude.ai's plan usage after you sign in yourself (works for
//    the Claude desktop app and claude.ai too).
// No credentials are read here.
#include <string>

struct ClaudeUsage {
    bool ok = false;           // file present and has a 7-day window
    double week = -1;          // 0..100, -1 = unknown
    double fiveHour = -1;      // 0..100, -1 = unknown
    long long weekResets = 0;  // unix seconds
    long long fiveResets = 0;
    double updatedAt = 0;      // unix seconds of the last statusLine run
    bool stale = false;        // older than a few hours
};

namespace claude_usage {
// Reads the file again only when it changed. Returns true when the shown values changed since the last call.
bool Poll(ClaudeUsage& out);
// "Claude weekly: 20% · resets Thu 20:00 · 5 hours: 14%"
std::wstring Tooltip(const ClaudeUsage& u);
}
