#!/usr/bin/env python3
"""在局域网提供 examples/status.json。只用 Python 标准库。

日常请优先用 tools/observe_status.py：它按白名单旁路观察
examples/bot_signals/，写出 status.json 并挂到 8787。
本脚本只静态托管已有文件，不会推断 bot 状态。

Tab5 与这台电脑需要在同一个局域网。STATUS_URL 写成
http://<这台电脑的局域网 IP>:8787/status.json
不要用 192.168.x.x，那是仓库里的占位符。
"""

import http.server
import socketserver
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "examples"
PORT = 8787


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)


def main() -> None:
    if not (ROOT / "status.json").is_file():
        raise SystemExit(f"找不到 {ROOT / 'status.json'}")

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("0.0.0.0", PORT), Handler) as httpd:
        print(f"提供目录 {ROOT}")
        print(f"本机访问 http://127.0.0.1:{PORT}/status.json")
        print("Tab5 请使用这台电脑的局域网 IP，而不是 127.0.0.1。")
        print("macOS 若弹出防火墙提示，允许传入连接。Ctrl+C 结束。")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
