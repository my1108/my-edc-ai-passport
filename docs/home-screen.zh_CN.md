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
| 同步行 | `Time synced today • HH:MM`、`Syncing time...`，或橙色的 `Sync failed • retrying in 1 h` |
| 按键提示 | `UP` Apps、`OK` Focus、`DOWN` Sync now |

在 Apps 和 Focus 占位页面上，按 `OK` 返回主屏。

30 秒内没有按键时屏幕进入休眠（关闭背光）。按任意键唤醒；这次按键只负责唤醒屏幕，不会打开页面或开始同步。屏幕休眠期间，时钟、Wi-Fi 和天气仍在后台运行。

内置 Montserrat 字体没有间隔号（U+00B7）和省略号（U+2026），因此界面改用项目符号（U+2022）和三个句点。城市名以 ASCII 显示：带重音的拉丁字母去掉重音，其他文字显示为 `?`。

## 联网行为

- **时间**：每次开机连上 Wi-Fi 后通过 SNTP 校时，之后每天本地时间 03:00 校时；失败后每小时重试。按 `DOWN` 立即校时。
- **位置**：开机、按 `DOWN` 以及每天通过 IP 定位（默认 `https://ipwho.is/`）获取城市。定位服务会看到设备的公网 IP 地址。
- **天气**：使用 [Open-Meteo](https://open-meteo.com/)（无需 API key），每 30 分钟刷新，失败后 10 分钟重试。离线或过旧的读数显示为灰色。
- **缓存**：最近一次天气和位置保存在 NVS 中，断电重启后先以灰色显示，直到获得新读数。时钟本身没有备用电池，断电后在首次校时前显示 `--:--`。

两个请求都通过 HTTPS 并使用 ESP-IDF 证书包校验，响应读入同一个 1.5 KB 缓冲区。固件在每次请求前后记录空闲堆和最大空闲块（日志标签 `home_net`），以便在设备上确认 TLS 内存预算。

## 通过串口控制台设置

Wi-Fi 凭据不会编译进固件。通过 USB 连接，打开串口监视器（例如 `idf.py -p <PORT> monitor`），然后输入：

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

设置保存在 NVS 命名空间 `home` 中。密码未经 NVS 加密存储，持有设备和 USB 线的人可以读出 Flash。烧录合并镜像可能重置 NVS 并清除这些设置，参见[烧录与已存数据](development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

## 编译期选项

`idf.py menuconfig` → **AI Passport Home Screen** 可设置默认时区、默认 12 小时制、校时钟点、SNTP 服务器、定位 URL、Open-Meteo 基础 URL、屏幕休眠时间（`CONFIG_HOME_SCREEN_SLEEP_S`，0 表示从不休眠）和天气刷新间隔。

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `main/main.c` | 启动、按键队列、电量轮询 |
| `main/home_ui.c` | 主屏、Apps 和 Focus 页面；250 ms 刷新定时器 |
| `main/home_net.c` | Wi-Fi STA、SNTP、定位和天气工作任务 |
| `main/home_store.c` | NVS 设置与缓存 |
| `main/home_console.c` | USB 串口命令控制台 |
| `main/home_logic.c`、`main/home_json.c` | 纯逻辑，由 `tests/test_home_logic.c` 和 `tests/test_home_json.c` 覆盖 |
| `main/home_layout.h` | 屏幕几何参数及编译期重叠检查 |
