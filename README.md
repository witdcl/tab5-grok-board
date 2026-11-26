# Tab5 状态板骨架

这是 M5Stack Tab5（ESP32-P4 主控 + ESP32-C6 Wi-Fi 协处理器）加官方 Tab5 Keyboard 的 PlatformIO 固件骨架。屏幕先画出 “Hello Tab5”，再尝试键盘和 Wi-Fi。键盘没装、Wi-Fi 没填，烧录后的程序仍然留在画面上，并写明缺什么。

Tab5 定时拉取一份 JSON，把任务列在屏幕上。JSON 由你自己的程序写好，放在局域网里。这是本仓库定的应用层字段，**不是 Grok Bot 的公开任务板 API**。Grok Bot 没有这样的公开接口。

## 硬件注意

官方说明，按这个做：

- 烧录和充电用 **USB Type-C**。机身 **USB Type-A 只是 Host**，不能用来烧录。
- 线必须能传数据。仅充电线不会在 Mac 上出现串口。
- 单击电源开机，双击关机。关机时不充电；要先开机并完成初始化才会充。
- 如果没关机就断电，等 5 秒再开机，否则可能因为电压异常导致 IMU 初始化失败。本骨架不用 IMU，但整机初始化仍受这个影响。
- 下载模式：Type-C 已经接上电脑时，按住 Reset/Boot 大约 2 秒，直到内部绿色 LED 快闪，再松开。
- 键盘接到侧面 Ext.Port1（2.54-10P），用两侧卡扣扣住。**关机后再插，不要热插这排针。** I2C：SDA = G0，SCL = G1，INT = G50，默认地址 0x6D。
- 屏幕驱动可能是 ST7123、ST7121，早期批次是 ILI9881C + GT911。库版本下限是 M5Unified >= 0.2.23、M5GFX >= 0.2.30。

官方文档：

- 产品：<https://docs.m5stack.com/zh_CN/core/Tab5>
- Arduino 烧录：<https://docs.m5stack.com/zh_CN/arduino/m5tab5/program>
- Wi-Fi 示例：<https://docs.m5stack.com/en/arduino/m5tab5/wifi>
- 键盘产品：<https://docs.m5stack.com/zh_CN/tab5/Tab5_Keyboard>
- 键盘 Arduino 示例：<https://docs.m5stack.com/zh_CN/arduino/projects/tab5/tab5_keyboard>

## 在 Cursor 里打开并烧录

1. 安装 Cursor 扩展 **PlatformIO IDE**（扩展市场里搜索这个名字，安装后按提示重载窗口）。
2. 用 Cursor 打开本文件夹。左下或左侧会出现 PlatformIO 的图标，环境选 `esp32p4_pioarduino`。
3. 第一次编译会从 GitHub 拉取平台 `pioarduino/platform-espressif32` 的标签 `54.03.21`，以及 M5Unified、M5GFX、M5Unit-KEYBOARD、ArduinoJson。需要网络，工具链也比较大。
4. 用数据线把 Tab5 的 **Type-C** 接到 Mac。单击电源开机。若平时上传失败，再进下载模式（按住 Reset/Boot 约 2 秒，绿灯快闪后松开）。
5. 选串口。这台 Mac 上出现过 `/dev/cu.usbmodem101`，重新插拔后编号可能变成别的 `usbmodem`。用 PlatformIO 的设备列表，或在终端执行 `pio device list`。端口没有写死在工程里；需要固定时，去掉 `platformio.ini` 里 `upload_port` / `monitor_port` 两行前面的分号再改。
6. 点 PlatformIO 的 Build，通过后再 Upload。串口监视波特率是 115200。官方片段打开了 `CORE_DEBUG_LEVEL=5`，日志会很多。
7. Tab5 的 Flash 是 16MB。`platformio.ini` 里加了 `board_build.partitions = default_16MB.csv`，这是 `esp32-p4-evboard` 板定义里写的分区表。标签 `54.03.21` 不会自动套用它；不写的话 app 槽只有 1.25MB，中文字库链接不过。

命令行等价于：

```bash
pio run -e esp32p4_pioarduino
pio run -e esp32p4_pioarduino -t upload --upload-port /dev/cu.usbmodem101
pio device monitor --port /dev/cu.usbmodem101 --baud 115200
```

`/dev/cu.usbmodem101` 只是示例。

用 pip 安装的 PlatformIO Core 如果带着 click 8.2 或更新版本，平台自带的 esptool 5.0.0-dev1 可能在生成 bootloader 时报 `get_metavar() missing ctx`。把 click 固定到 8.1.x 即可。Cursor 里的 PlatformIO IDE 扩展通常自带 Python 环境，不一定会碰到。

编写时在 `54.03.21` 上完整编过一次：M5Unified 0.3.0、M5GFX 0.3.0（高于屏幕驱动要求的下限）、M5Unit-KEYBOARD 0.1.0、M5UnitUnified 0.6.5。debug 固件大约 1.5MB，落在 `default_16MB.csv` 的 6.25MB app 槽里。这些 git 版本会随默认分支变化。

## 荐书板 · GitHub Pages

每日片单由 Actions 生成，托管在 `docs/`（GitHub Pages）：

- 预览：`https://<user>.github.io/tab5-grok-board/`
- 设置 / 手动重生：`…/settings.html`
- 设备拉取：`…/weread_day.json`

仓库 Secrets（Settings → Secrets and variables → Actions）：

- `WEREAD_API_KEY`
- `DEEPSEEK_API_KEY`
- `DEEPSEEK_BASE_URL`
- `DEEPSEEK_MODEL`（可选）

Pages：Settings → Pages → Deploy from branch → `main` / `/docs`。  
固件 `WEREAD_DAY_URL` 指向上述 HTTPS 地址（支持 https）。本机仍可用 `python3 tools/weread_board.py --once` 或 `--serve`。

## 本地配置

```bash
cp include/config.example.h include/config.h
```

在 `include/config.h` 里填写：

- `WIFI_SSID`、`WIFI_PASSWORD`：留空表示不启动 Wi-Fi。
- `STATUS_URL`：例如 `http://192.168.1.20:8787/status.json`。示例文件里的 `192.168.x.x` 是占位符，固件会把它当成还没配置。
- `WEREAD_DAY_URL`：GitHub Pages 的 `weread_day.json`，或局域网旁路地址。

`include/config.h` 已在 `.gitignore` 里。不要把密码提交上去。没有这个文件时，编译使用 `config.example.h`，屏幕仍会起来。

在电脑上旁路观察并提供 JSON（只使用 Python 标准库）：

```bash
python3 tools/observe_status.py
```

它读取 `examples/bot_signals/<bot>.json`，按白名单写出 `examples/status.json`，并挂在 `0.0.0.0:8787`。**不要让 Grok Bot 专门为看板汇报**；信号文件由本机钩子从已有产物推断后写入。若只要静态托管已有文件，可用 `python3 tools/serve_status.py`。

Tab5 要填这台电脑的局域网 IP，不能填 `127.0.0.1`。macOS 若询问防火墙，允许传入连接。

第一版只监测：蜡笔、零工、票匣、旅伴、膳枢、主脑、书枢、卷枢、机枢、剧枢。  
`status`：`hold` / `run` / `done` / `error`。屏幕画房间小人，只显示短 `task` 气泡；约每 **5 秒** 拉一次，Enter 立刻再拉。

约定的 JSON 长这样（应用层约定，不是某家 bot 的官方接口）：

```json
{
  "updated_at": "2026-11-23T17:49:00+08:00",
  "items": [
    {
      "bot": "主脑",
      "status": "hold",
      "task": "等音线开闸",
      "note": "板上第一版不显示 note"
    }
  ]
}
```

## 键盘

使用官方 `UnitTab5Keyboard` 的 **Character（字符）模式**：`loop` 里调用 `M5.update()` 和 `Units.update()`，再用 `empty()` / `oldest()` / `discard()` 读队列。

- 可打印字符显示在屏幕底部的回显行。
- 退格（`\b` 或 DEL）删除最后一个字符。
- Enter（`\r` 或 `\n`）刷新 JSON。官方示例把这两个字符当成换行，这里改成刷新。
- 方向键导航还没做。官方说明字符模式不提供行列坐标，也没有完整的导航键编辑语义。

没检测到键盘时，屏幕写“未检测到键盘”，主界面继续跑。

## 程序怎么分

| 文件 | 作用 |
| --- | --- |
| `src/main.cpp` | `M5.config()` / `M5.begin()`，串起键盘、Wi-Fi 和刷新 |
| `src/ui.cpp` | 1280×720 房间分区 + 几何小人 + 短 task 气泡 |
| `src/keyboard_input.cpp` | Ext.Port1，Character 模式；失败则降级 |
| `src/wifi_link.cpp` | 按官方示例做 STA。核心没打开 ESP-Hosted 时只显示不可用 |
| `src/status_client.cpp` | `HTTPClient` 取 JSON，只保留白名单 bot |
| `include/bots_roster.h` | 监测白名单与房间落位 |
| `include/config.example.h` | SSID、密码、URL 占位 |
| `examples/status.json` | 观察服务写出的看板文档 |
| `examples/bot_signals/` | 各 bot 旁路信号（可选） |
| `tools/observe_status.py` | 旁路观察并挂局域网 8787 |
| `tools/serve_status.py` | 仅静态托管已有 status.json |

Wi-Fi 成功之后，`status_fetch()` 才会发请求。无线电初始化和 HTTP 是分开的。

## 尚未在这块板上跑通的部分

这个骨架是按官方文档和库头文件写的，编写时没有把固件烧进一台 Tab5，也碰不到你 Mac 上的 `/dev/cu.usbmodem101`。

- **Wi-Fi 能编过，还没在实机上连通过。** 官方 PlatformIO 片段使用板型 `esp32-p4-evboard`。这个板型头文件里的 `BOARD_SDIO_ESP_HOSTED_*` 是乐鑫 EV 板引脚（CLK 18、CMD 19、D0–D3 为 14–17、RST 54），不是 Tab5。Tab5 的 SDIO2 在产品管脚图和官方 Wi-Fi 示例里是 CLK = G12、CMD = G13、D0 = G11、D1 = G10、D2 = G9、D3 = G8、RST = G15。`src/wifi_link.cpp` 按该示例调用 `WiFi.setPins(...)`，然后 `WiFi.mode(WIFI_STA)` 和 `WiFi.begin()`。`54.03.21` 自带的 ESP32-P4 预编译库里 `CONFIG_ESP_WIFI_REMOTE_ENABLED` 为 1，所以这条路径会参与编译，不是空桩。屏幕上的“核心未启用 ESP-Hosted”只在该开关被关掉时出现。关联是否成功要上板才知道。
- **没有调用 WLAN 供电脚，也没有切换天线。** 官方 STA 示例不操作 PI4IO 的 `WLAN_PWR_EN`，默认内置天线。外置天线的官方写法是 `M5.getIOExpander(0).digitalWrite(0, true)`（低电平为内置）。骨架没加这一步，避免在显示初始化之外再碰 IO 扩展。
- **HTTP。** 用的是 Arduino 核心里的 `HTTPClient`，不是 Tab5 专用 API。官方 Wi-Fi 文档只演示了关联和扫描。
- **键盘字符模式**的 Enter 是否正好上报 `\r` / `\n`，以官方示例的处理为准，没有在实机上再确认。库默认地址 0x6D，程序不改地址。
- 官方 Arduino IDE 板名是 `M5Tab5`（板管理包 >= 3.2.2）。PlatformIO 官方片段没有这个板名，本仓库没有自造板定义。

屏幕若黑，先核对数据线、下载模式，以及 M5Unified / M5GFX 是否不低于上面的版本。屏幕复位时序由这两个库处理，不要自己改 LCD 复位脚。
