"""Claude Code statusLine command.

Claude Code pipes a JSON payload to this script on stdin. When you are on a
Pro / Max plan the payload carries `rate_limits` (documented at
https://code.claude.com/docs/en/statusline.md). This script

  1. saves those numbers to %LOCALAPPDATA%\\ClaudeLimitWidget\\usage.json so the
     desktop widget can show them, and
  2. prints a short status line for the terminal.

No credentials are read; only the payload Claude Code hands to us is used.
"""
import json
import os
import sys
import tempfile
import time

DATA_DIR = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "ClaudeLimitWidget")
DATA_FILE = os.path.join(DATA_DIR, "usage.json")


def window(rl, name):
    w = (rl or {}).get(name)
    if not isinstance(w, dict):
        return None
    pct = w.get("used_percentage")
    if pct is None:
        return None
    return {"used_percentage": float(pct), "resets_at": w.get("resets_at")}


def save(data):
    os.makedirs(DATA_DIR, exist_ok=True)
    # Write atomically so the widget never reads half a file.
    fd, tmp = tempfile.mkstemp(dir=DATA_DIR, suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, DATA_FILE)


def main():
    try:
        raw = sys.stdin.buffer.read().decode("utf-8-sig", errors="replace").strip()
        payload = json.loads(raw or "{}")
    except Exception:
        payload = {}

    rl = payload.get("rate_limits") or {}
    week = window(rl, "seven_day")
    five = window(rl, "five_hour")

    if week or five:
        try:
            save({"seven_day": week, "five_hour": five, "updated_at": time.time()})
        except OSError:
            pass

    model = (payload.get("model") or {}).get("display_name") or "Claude"
    parts = [model]
    if week:
        parts.append(f"week {week['used_percentage']:.0f}%")
    if five:
        parts.append(f"5h {five['used_percentage']:.0f}%")
    sys.stdout.write(" | ".join(parts))


if __name__ == "__main__":
    main()
