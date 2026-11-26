#!/usr/bin/env python3
"""旁路观察：根据白名单与 bot_signals/ 生成 status.json，并可选挂到局域网。

不调用 Grok，不登录任何 bot。Tab5 只 HTTP GET 生成结果。
"""

from __future__ import annotations

import argparse
import json
import http.server
import socketserver
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXAMPLES = ROOT / "examples"
SIGNALS = EXAMPLES / "bot_signals"
OUT = EXAMPLES / "status.json"
PORT = 8787

# 与 include/bots_roster.h 保持一致
ROSTER = [
    "主脑",
    "书枢",
    "剧枢",
    "卷枢",
    "机枢",
    "膳枢",
    "旅伴",
    "票匣",
    "零工",
    "蜡笔",
]

ALLOWED_STATUS = {"hold", "run", "done", "error"}
TZ_SH = timezone(timedelta(hours=8))


def now_iso() -> str:
    # 固定东八区墙钟，避免 ISO 带 T/+0800 看起来不像北京时间。
    return datetime.now(TZ_SH).strftime("%Y-%m-%d %H:%M:%S")


def load_signal(bot: str) -> dict:
    path = SIGNALS / f"{bot}.json"
    if not path.is_file():
        return {"status": "hold", "task": "", "note": ""}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": "error", "task": "信号文件坏了", "note": str(exc)}
    status = str(data.get("status", "hold")).strip().lower()
    if status not in ALLOWED_STATUS:
        status = "error"
        task = "非法 status"
    else:
        task = str(data.get("task", "") or "")
    note = str(data.get("note", "") or "")
    return {"status": status, "task": task[:80], "note": note[:120]}


def build_document() -> dict:
    items = []
    for bot in ROSTER:
        sig = load_signal(bot)
        items.append(
            {
                "bot": bot,
                "status": sig["status"],
                "task": sig["task"],
                "note": sig["note"],
            }
        )
    return {"updated_at": now_iso(), "items": items}


def items_fingerprint(doc: dict) -> str:
    slim = [
        {
            "bot": it.get("bot", ""),
            "status": it.get("status", ""),
            "task": it.get("task", ""),
            "note": it.get("note", ""),
        }
        for it in doc.get("items", [])
    ]
    return json.dumps(slim, ensure_ascii=False, sort_keys=True)


def write_status(doc: dict) -> bool:
    """每次扫描都刷新 updated_at（北京时间）。

    items 没变时仍然写文件，但板端忽略 updated_at 整屏重画，只刷新时间行。
    返回值表示 bot/status/task 是否变化，供日志使用。
    """
    SIGNALS.mkdir(parents=True, exist_ok=True)
    items_changed = True
    if OUT.is_file():
        try:
            prev = json.loads(OUT.read_text(encoding="utf-8"))
            items_changed = items_fingerprint(prev) != items_fingerprint(doc)
        except (OSError, json.JSONDecodeError):
            pass
    tmp = OUT.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(OUT)
    return items_changed


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(EXAMPLES), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))


def serve(interval: float) -> None:
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("0.0.0.0", PORT), Handler)
    httpd.timeout = 0.5
    print(f"观察目录 {SIGNALS}")
    print(f"写出 {OUT}")
    print(f"本机访问 http://127.0.0.1:{PORT}/status.json")
    print("Tab5 请用这台电脑的局域网 IP，不要用 127.0.0.1。Ctrl+C 结束。")
    last = 0.0
    try:
        while True:
            now = time.monotonic()
            if now - last >= interval:
                doc = build_document()
                changed = write_status(doc)
                last = now
                if changed:
                    print(f"updated_at={doc['updated_at']} items={len(doc['items'])}")
            httpd.handle_request()
    except KeyboardInterrupt:
        print("\nstopped")


def main() -> None:
    parser = argparse.ArgumentParser(description="旁路观察并提供 status.json")
    parser.add_argument("--once", action="store_true", help="只生成一次文件后退出")
    parser.add_argument("--interval", type=float, default=5.0, help="重扫信号间隔秒，默认 5")
    parser.add_argument("--serve", action="store_true", help="挂 HTTP（默认开启，除非 --once）")
    args = parser.parse_args()

    if args.once:
        doc = build_document()
        if write_status(doc):
            print(f"wrote {OUT}")
        else:
            print(f"unchanged {OUT}")
        return

    serve(max(1.0, args.interval))


if __name__ == "__main__":
    main()
