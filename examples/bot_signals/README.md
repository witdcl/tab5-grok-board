# bot_signals

旁路观察服务读取本目录里的信号文件，生成 `examples/status.json`。

每个被监测的 bot 可放一个同名 JSON，例如 `主脑.json`：

```json
{"status": "run", "task": "复检三线闸", "note": "可选，板上第一版不显示"}
```

`status` 只能是 `hold` / `run` / `done` / `error`。  
没有信号文件时，该 bot 默认为 `hold`、空 task。

不要让 Grok Bot 为了看板专门写这些文件；由本机脚本/钩子从已有产物推断后写入。
