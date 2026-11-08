<p align="right">
  <strong>简体中文</strong> · <a href="home-screen.md">English</a>
</p>

# 主屏应用

主屏应用把 AI Passport 变成一个桌面时钟：在一块竖屏上显示联网校时的时钟、本地天气、电量和 Wi-Fi 状态。开机后直接进入主屏，取代基线的硬件测试菜单。

## 界面与按键

| 区域 | 内容 |
| --- | --- |
| 状态栏 | 左侧 Wi-Fi 图标（离线时变暗）；右侧为 CW2017 电量计读取的电量百分比和电池图标 |
| 日期 | 强调色的一行，如 `SUN • 8 NOV` |
| 时钟 | 大号 `HH:MM` 加小号秒数；默认 24 小时制，可选带 AM/PM 的 12 小时制 |
| 天气卡片 | 图标（晴、少云、多云、雾、雨、雪、雷暴；夜间显示月亮），摄氏温度，天气描述，以及 `City • H xx° L xx°` |
| 同步行 | `Time synced today • HH:MM`、`Syncing time...`，或橙色的 `Sync failed • retrying in 1 min` |
| 按键提示 | `UP` Apps、`OK` Setup、`DOWN` Sync now |

按 `UP` 打开 **Apps** 应用列表。用 `UP` 和 `DOWN` 选择应用（选中项停在第一项和最后一项，不会循环），按 `OK` 打开。长按 `OK` 总是返回上一级：从应用回到列表，从列表回到主屏。列表只显示已安装的应用，目前只有 [Spotify 正在播放](spotify.zh_CN.md)。按 `OK` 打开 [Setup](#用手机设置)，用手机设置 Wi-Fi 和 Spotify。

| 位置 | `UP` | `DOWN` | 短按 `OK` | 长按 `OK` |
| --- | --- | --- | --- | --- |
| 主屏 | 应用列表 | 立即同步 | Setup | - |
| 应用列表 | 上一项 | 下一项 | 打开应用 | 主屏 |
| Spotify | 上一首 | 下一首 | 播放 / 暂停 | 应用列表 |
| Setup | 上一张二维码卡片 | 下一张二维码卡片 | 主屏 | 主屏 |

应用的界面只在应用打开期间存在。打开时创建界面；离开时先停止应用的服务（Spotify 停止轮询），再删除界面，因此同一时间只有一个应用占用 LVGL 内存。添加应用只需在 `main/home_apps.c` 的 `HOME_APPS` 中加一项：名称、图标、副标题，以及创建、刷新、销毁、启停和按键回调。列表最多容纳三个应用（`HOME_APPS_MAX`）。

30 秒内没有按键时屏幕进入休眠（关闭背光）。按任意键唤醒；这次按键只负责唤醒屏幕，不会打开页面或开始同步。屏幕休眠期间，时钟、Wi-Fi 和天气仍在后台运行。

10 分钟内没有按键时，设备进一步进入深度睡眠：关闭 Wi-Fi，耗电大大降低。单击 `UP`、`OK` 或 `DOWN` 即可唤醒。随后应用重新启动：约 2 到 3 秒后屏幕恢复，再过几秒 Wi-Fi、天气和 Spotify 重新连接。这次按键不触发其他动作。深度睡眠期间时钟继续计时。Setup 打开时不会进入深度睡眠；通过 USB 连接电脑时也不会（以便串口控制台继续可用）；只接普通 USB 充电器时仍会进入深度睡眠。

独立的电源键接在电源电路上，而不是芯片上，固件无法改变它的行为。按住约 0.5 秒开机，约 2 秒关机。

内置 Montserrat 字体没有间隔号（U+00B7）和省略号（U+2026），因此界面改用项目符号（U+2022）和三个句点。城市名以 ASCII 显示：带重音的拉丁字母去掉重音，其他文字显示为 `?`。

## 联网行为

- **时间**：每次开机连上 Wi-Fi 后通过 SNTP 校时，之后每天本地时间 03:00 校时。失败后 1 分钟重试，之后每次失败间隔加倍，最长每小时一次。按 `DOWN` 立即校时。
- **位置**：开机、按 `DOWN` 以及每天通过 IP 定位（默认 `https://ipwho.is/`）获取城市。定位服务会看到设备的公网 IP 地址。
- **天气**：使用 [Open-Meteo](https://open-meteo.com/)（无需 API key），每 30 分钟刷新，失败后 10 分钟重试。离线或过旧的读数显示为灰色。
- **缓存**：最近一次天气和位置保存在 NVS 中，断电重启后先以灰色显示，直到获得新读数。时钟本身没有备用电池，断电后在首次校时前显示 `--:--`。

两个请求都通过 HTTPS 并使用 ESP-IDF 证书包校验，响应读入同一个 1.5 KB 缓冲区。固件在每次请求前后记录空闲堆和最大空闲块（日志标签 `home_net`），以便在设备上确认 TLS 内存预算。

## 用手机设置

在主屏按 `OK` 打开 **Setup**。如果没有保存 Wi-Fi，开机时也会自动打开。Setup 打开期间：

- 设备开启名为 `AI-Passport-XXXX` 的设置热点（XXXX 为 MAC 的最后四位十六进制数），每次打开都使用新的随机密码；
- 在热点上的 `http://192.168.4.1/`，以及设备联网后在家庭网络中的设备地址上，提供 Wi-Fi 和 Spotify 设置网页；
- 屏幕保持常亮并显示二维码。`UP` 和 `DOWN` 切换卡片，`OK` 关闭 Setup。

| 卡片 | 二维码 | 用途 |
| --- | --- | --- |
| 1 Join the setup Wi-Fi | 加入热点（含名称和密码的 Wi-Fi 二维码） | 首次设置，或设备离线时 |
| 2 Open the setup page | `http://192.168.4.1/` | 通过热点打开网页 |
| Setup page on home Wi-Fi | `http://<设备 IP>/` | 设备联网后优先显示；Spotify 登录需要用它 |

在网页上点 **Scan for networks**，选择 2.4 GHz 网络，输入密码并点 **Connect**。设备保存后开始连接，屏幕随后切换到家庭 Wi-Fi 卡片。连接路由器可能使热点改到路由器所在信道，手机可能短暂断开热点，必要时重新加入即可。关联 Spotify 的步骤见 [Spotify 正在播放](spotify.zh_CN.md#关联账号)。

按 `OK`，或 15 分钟内没有按键和网页操作时，Setup 关闭，同时停止热点和网页。Setup 关闭期间，任何人都无法通过网络修改设备设置。每次加载网页都会获得一个其他网站读不到的会话令牌，因此在 Setup 打开期间，其他网站也无法向设备提交请求。

## 通过串口控制台设置

串口控制台是手机设置页面之外的另一种方式。Wi-Fi 凭据不会编译进固件。通过 USB 连接，打开串口监视器（例如 `idf.py -p <PORT> monitor`），然后输入：

```text
wifi "My Network" "my password"
status
```

| 命令 | 作用 |
| --- | --- |
| `wifi "<ssid>" "<password>"` | 保存 2.4 GHz 网络并连接（含空格时必须加引号） |
| `wifi "<ssid>"` | 保存开放网络 |
| `wifi clear` | 清除网络设置 |
| `tz <POSIX TZ>` | 设置时区，例如 UTC+8 为 `tz <+08>-8`，或 `tz CET-1CEST,M3.5.0,M10.5.0/3` |
| `tz default` | 恢复编译默认值（`CONFIG_HOME_TZ_DEFAULT`，UTC+8） |
| `clock 12` / `clock 24` | 时钟格式 |
| `sync` | 与按 `DOWN` 相同 |
| `status` | 显示设置、网络、同步、天气和堆内存；从不打印密码 |
| `spotify ...` | 关联 Spotify 账号，参见 [Spotify 正在播放](spotify.zh_CN.md#关联账号) |

设置保存在 NVS 命名空间 `home` 中。密码未经 NVS 加密存储，持有设备和 USB 线的人可以读出 Flash。烧录合并镜像可能重置 NVS 并清除这些设置，参见[烧录与已存数据](development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

## 编译期选项

`idf.py menuconfig` → **AI Passport Home Screen** 可设置默认时区、默认 12 小时制、校时钟点、SNTP 服务器、定位 URL、Open-Meteo 基础 URL、屏幕休眠时间（`CONFIG_HOME_SCREEN_SLEEP_S`，0 表示从不休眠）、深度睡眠时间（`CONFIG_HOME_DEEP_SLEEP_MIN`，0 表示从不）、连接电脑时是否保持唤醒（`CONFIG_HOME_DEEP_SLEEP_STAY_ON_USB`）和天气刷新间隔。

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `main/main.c` | 启动、按键队列、电量轮询 |
| `main/home_ui.c` | 主屏、应用列表、Spotify 和 Setup 页面；250 ms 刷新定时器 |
| `main/home_apps.c`、`main/home_apps.h` | 应用列表的注册表和应用生命周期回调 |
| `main/home_net.c` | Wi-Fi STA、SNTP、定位和天气工作任务 |
| `main/home_store.c` | NVS 设置与缓存 |
| `main/home_console.c` | USB 串口命令控制台 |
| `main/home_logic.c`、`main/home_json.c` | 纯逻辑，由 `tests/test_home_logic.c` 和 `tests/test_home_json.c` 覆盖 |
| `main/home_layout.h` | 屏幕几何参数及编译期重叠检查 |
| `main/spot_*.c` | Spotify 页面，参见 [Spotify 正在播放](spotify.zh_CN.md#代码结构) |
| `main/setup_portal.c`、`main/setup_page.html` | 设置热点和网页 |
| `main/setup_logic.c` | 设置相关纯逻辑（PKCE、URL/表单解析、二维码文本），由 `tests/test_setup_logic.c` 覆盖 |
