<p align="right">
  <strong>简体中文</strong> · <a href="spotify.md">English</a>
</p>

# Spotify 正在播放

[主屏](home-screen.zh_CN.md)的 Spotify 页面显示你的 Spotify 账号正在播放的曲目，并用三个按键控制播放。在主屏按 `UP` 打开该页面。

## 界面与按键

| 区域 | 内容 |
| --- | --- |
| 状态栏 | 左侧 `NOW PLAYING`，右侧电量 |
| 状态行 | 绿色的 `PLAYING • <设备>` 或 `PAUSED • <设备>`；橙色的 `RECONNECTING`、`OFFLINE` 或 `SPOTIFY BUSY` |
| 专辑封面 | 专辑（或播客）封面，最大 80 x 80 像素，位于状态行下方居中 |
| 均衡器 | 封面加载中或没有封面时代替封面显示：播放时跳动的绿色竖条；暂停或空闲时变平并呈灰色 |
| 标题 | 最多两行，超出部分以 `...` 截断 |
| 艺人 | 最多三位艺人（播客则显示节目名），一行 |
| 进度 | 进度条加已播放时间和总时长（`1:23` / `3:45`），两次轮询之间在本地推进 |
| 底部行 | `Hold OK for home`；命令发送中显示 `Sending...`；命令出错时显示 3 秒错误，如 `No active device` |
| 按键提示 | `UP` Prev、`OK` Play 或 Pause、`DOWN` Next |

| 按键 | 动作 |
| --- | --- |
| 短按 `UP` | 上一首（`POST /v1/me/player/previous`） |
| 短按 `DOWN` | 下一首（`POST /v1/me/player/next`） |
| 短按 `OK` | 正在播放时暂停，否则继续播放（`PUT /v1/me/player/pause` 或 `/play`） |
| 长按 `OK` | 返回主屏 |

与主屏其他页面一样，快速双击按一次处理。30 秒无按键后屏幕仍会休眠；唤醒屏幕的那次按键不触发其他动作。

没有曲目可显示时，页面改为显示提示：`Link Spotify`（尚未关联账号）、`Link expired`、`No Wi-Fi`、`Offline`、`Loading`、`Nothing playing`、`Access denied`、`Premium needed`、`Spotify is busy` 或 `Can't reach Spotify`。

## 前提条件

- **Spotify Premium。** 自 2026 年 2 月起，每个开发模式（Development Mode）应用都要求其所有者拥有 Premium，播放控制接口也需要 Premium。没有 Premium 时页面可能显示 `Premium needed`，按键会提示 `Needs Spotify Premium`。
- **一个活动的 Spotify 设备。** 本板是遥控器，不是播放器。先在手机、电脑或音箱上打开 Spotify；没有活动设备时，页面显示 `Nothing playing`，按键提示 `No active device`。
- **开发模式限制。** 个人应用保持在开发模式：最多 5 个 Spotify 账号，每个账号需在 **User Management** 中手动添加；请求配额由同一开发者账号下的所有开发模式应用共享。
- 已通过 [Setup 页面](home-screen.zh_CN.md#用手机设置)或串口控制台设置 Wi-Fi。

## 创建 Spotify 应用（一次性）

1. 打开 [Spotify 开发者控制台](https://developer.spotify.com/dashboard)，用将作为应用所有者的账号登录（该账号需要 Premium）。
2. **Create app。** 名称和描述随意，例如 `AI Passport`。
3. **Redirect URIs：** 精确添加 `http://127.0.0.1:8888/callback`。Spotify 不接受 `localhost`，请使用回环 IP。保存后刷新页面，确认它没有被改写为 `localhost`。
4. **Which API/SDKs are you planning to use：** 勾选 **Web API**。同意条款并保存。
5. 打开应用的 **Settings**，复制 **Client ID**（32 位十六进制）。不需要 Client secret：设备和配置工具都使用 PKCE。
6. 在 **User Management** 中添加每个将要关联的 Spotify 账号的名字和邮箱；如果你自己的账号不在列表中，也要添加。

## 关联账号

### 用手机（Setup 页面）

1. 确认设备已联网：在主屏按 `OK` 打开 Setup。如果尚未设置 Wi-Fi，先扫描卡片 1 加入设置热点，把设备连到你的 Wi-Fi（参见[用手机设置](home-screen.zh_CN.md#用手机设置)）。
2. 让手机回到家庭 Wi-Fi，扫描 **Setup page on home Wi-Fi** 卡片（登录需要互联网，而设置热点没有互联网）。
3. 输入 **Client ID**，点 **Log in with Spotify**，再点 **Open the Spotify login**。登录页在新标签页中打开，只申请以下权限范围：

| Scope | 用途 |
| --- | --- |
| `user-read-playback-state` | `GET /v1/me/player`：曲目、进度、设备 |
| `user-read-currently-playing` | 同类数据；为兼容性而申请 |
| `user-modify-playback-state` | 上一首、下一首、播放和暂停 |

4. 同意授权。该标签页随后尝试打开 `http://127.0.0.1:8888/callback?...`，显示“无法连接”的页面。这是正常的：Spotify 只允许纯 HTTP 跳转到回环地址，而设备无法拥有公网 HTTPS 地址。
5. 复制该标签页的完整地址，切回设置页面粘贴，然后点 **Finish login**。设备会确认该地址属于它发起的登录，自行用授权码换取 token（PKCE：保密的 verifier 从不离开设备），并保存结果。授权码只能使用一次，几分钟后过期；如果失败，请重新点 **Log in with Spotify**。

同一页面会显示关联状态，并提供 **Forget Spotify** 按钮。

### 用电脑（串口控制台）

在有浏览器的电脑上运行配置工具（Python 3.8 或更高版本，无需额外依赖包）：

```bash
python3 tools/spotify_auth.py --client-id <你的 Client ID>
```

它申请相同的权限范围，然后打印两行命令。把它们粘贴到设备的串口控制台（例如 `idf.py -p COM3 monitor`）：

```text
spotify client <client ID>
spotify token <refresh token>
```

之后在主屏按 `UP`。在控制台输入 `spotify` 可查看关联状态、最近一次轮询结果，以及完整 UTF-8 的当前曲目。

| 命令 | 作用 |
| --- | --- |
| `spotify client <id>` | 保存应用的 Client ID |
| `spotify token <refresh>` | 保存 `spotify_auth.py` 生成的 refresh token |
| `spotify clear` | 清除两者 |
| `spotify` / `spotify status` | 关联状态、最近结果、当前曲目；从不打印 token |

refresh token 相当于你 Spotify 账号的密码。它保存在 NVS 命名空间 `home`（键 `sp_rt`）中，未经 NVS 加密，持有设备和 USB 线的人可以读出。它从不编译进固件，也不会被记录到日志或打印。要撤销授权，请在 [Spotify 账号 → Manage apps](https://www.spotify.com/account/apps/) 中移除该应用，并执行 `spotify clear`。

## 工作原理

### 授权

设备无法显示浏览器登录页，因此带 PKCE 的授权码流程在手机或电脑浏览器中执行一次：通过 Setup 页面（由设备生成 verifier 并自行换取 token），或使用 `tools/spotify_auth.py`。之后设备只保存 Client ID 和 refresh token。设备在 `https://accounts.spotify.com/api/token` 用 refresh token 换取 access token（PKCE 公共客户端：请求体中带 `client_id`，不带 secret）。access token 约一小时有效，只保存在 RAM 中；设备在到期前两分钟或收到 `401` 后立即续期。Spotify 返回新的 refresh token 时，设备先把它写入 NVS，再使用新的 access token。

如果 Spotify 返回 `invalid_grant`、`invalid_client` 或 `unauthorized_client`（token 被撤销、应用被删除，或 refresh token 到期），设备会清除已保存的 refresh token 并显示 `Link expired`。请在 Setup 中重新登录。续期时遇到网络或服务器错误，会在 10 秒后重试，间隔逐次加倍，最长 5 分钟。

### 轮询与限流

只有当 Spotify 页面打开且屏幕亮着时，设备才轮询 `GET /v1/me/player?additional_types=episode`：

| 状态 | 下次轮询 |
| --- | --- |
| 播放中 | 5 秒；若当前曲目更早结束，则在预计结束后 0.7 秒（至少 1 秒） |
| 暂停 | 10 秒 |
| 无播放（`204`）、无活动设备 | 15 秒 |
| 按键命令成功后 | 0.7 秒 |
| 网络或服务器错误 | 5 秒，逐次加倍，最长 60 秒 |
| `429` Too Many Requests | 按 `Retry-After` 头（1 到 600 秒；缺失时 30 秒） |
| 续期后仍 `401`、`403`、其他错误 | 60 秒 |

打开页面时立即轮询。重新打开页面时，超过 30 秒的旧读数不再显示，页面先显示 `Loading`，直到收到新回复。页面隐藏或屏幕休眠时不发出任何 Spotify 请求，并关闭保持的连接。连续播放时大约每分钟 12 个请求。Spotify 没有公布限流的具体数值（它按应用统计 30 秒滑动窗口内的请求数），因此设备每次收到 `429` 都遵守 `Retry-After`。

按键命令进入队列；上一条命令尚未发出时，新按键会替换它。播放/暂停会立即在屏幕上切换状态，由下一次轮询确认。

### 内存

本板没有 PSRAM。`/v1/me/player` 的回复有数 KB（专辑图片、艺人、设备、上下文）。它从不被整体放进内存：HTTP 正文按 512 字节分块送入流式 JSON 扫描器（`main/spot_json.c`），扫描器用 96 字节缓冲区跟踪当前键路径（如 `item.artists.0.name`），只保留页面要绘制的字段。扫描器、解析器和解析结果共占用不到 2 KB 静态 RAM。错误回复和 token 回复很小，使用一个 1.5 KB 缓冲区。Web API 的这个接口不支持 `fields` 过滤，因此用流式解析来限制内存。

所有 HTTPS 流量（包括 Spotify）都在同一个 `home_net` 工作任务中执行。与 `api.spotify.com` 的连接在两次轮询之间保持，避免每隔几秒做一次 TLS 握手；在请求 token 前、任何定位或天气请求前、Wi-Fi 断开时以及页面关闭时都会关闭该连接。因此同一时间最多只有一个 TLS 会话占用内存。天气请求前后（日志标签 `home_net`），以及每次新建 Spotify 连接、解码封面和释放封面之后（日志标签 `spotify`，例如 `heap after cover connect: free=... largest=... min_ever=...`），都会记录空闲堆。`min_ever` 只会下降，因此它第一次下降的那一行就指出了造成最低点的请求。复用保持连接的轮询不记录。

### 专辑封面

Spotify 在 `i.scdn.co` 上以 JPEG 提供每张专辑的封面，尺寸约为 640、300 和 64 像素。300 像素的图片完整解码为 RGB565 需要 180 KB，远超本板的余量。因此 `spot_cover_pick()` 选择解码后尺寸最接近 80 像素的图片；两者相差 8 像素以内时选下载量更小的那张。实际上就是 300 像素的图片，以 1/4 比例解码为 75 x 75。用 640 像素的图片以 1/8 解码可以得到 80 x 80，但下载量要大好几倍。

- **解码器。** JPEG 从 HTTP 正文直接送入 ESP32-C3 ROM 中的 TJpgDec，解码时只需要 3.2 KB 工作区，整个 JPEG 从不完整放进内存。
- **像素缓冲区。** 像素写入一个 12.8 KB 的 RGB565 缓冲区，页面需要封面时分配，页面关闭时释放。其他页面（主屏的天气请求、Setup）因此可以收回这部分内存，代价是每次打开页面都要重新下载一次封面（约 25 KB）。
- **何时下载。** 只在页面打开时、且封面 URL 变化时（换专辑，而不是每首歌或每次轮询）下载，并且先关闭 API 连接，因此同一时间仍只有一个 TLS 会话。
- **切换专辑。** 下一张封面下载期间（网络良好时不到一秒），页面显示均衡器，而不是继续显示属于另一张专辑的旧封面。若要在新封面就绪前保留旧图，需要第二个 12.8 KB 缓冲区，而 HTTPS 期间空闲堆已降到约 60 KB。
- **失败处理。** 下载或解码失败（离线、渐进式 JPEG 或尺寸过大）时保持显示均衡器，并在 2 分钟后重试该 URL。

### 文字与字体

标题和艺人使用 Montserrat（标题 20 px，艺人 14 px），并以 LVGL 内置的思源黑体 SC 16 子集作为后备字体（`CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK`）。该子集约含 1,000 个常用汉字（简体和繁体）、日文假名和 ASCII，不能覆盖全部中文，也不含韩文。

绘制前，`spot_text_for_font()` 会按标签实际使用的字体链逐字检查：

- 字体能绘制的字符保留；
- 带重音的拉丁字母去掉重音（`Beyoncé` → `Beyonce`）；
- 弯引号、破折号、省略号、全角 ASCII 和中日韩标点转为 ASCII；
- 其余字符显示为 `?`。

串口命令 `spotify` 会打印原始 UTF-8，因此屏幕上带 `?` 的标题仍可在串口中读到。要完整显示中文、日文或韩文标题，需要生成更大的字体子集（参见 [LVGL 中文字体](development/engineering/lvgl-chinese-fonts.zh_CN.md)），这需要一个有授权的字体文件和 `lv_font_conv`，目前尚未包含。

## 故障排查

| 现象 | 检查 |
| --- | --- |
| `Link Spotify` | 在 Setup 页面登录，或粘贴 `spotify client` 和 `spotify token` 两行 |
| 设置页面提示登录需要互联网 | 手机连着设置热点；请切回家庭 Wi-Fi，使用家庭 Wi-Fi 卡片 |
| `That address is from an older login` | 使用最近一次点 **Log in with Spotify** 打开的标签页 |
| `Spotify rejected the code` | 授权码已用过或已过期；重新登录并在几分钟内粘贴 |
| `Access denied` | 在 **User Management** 中添加该账号；应用所有者需要 Premium |
| `Premium needed` / `Needs Spotify Premium` | 该账号或应用所有者没有 Premium |
| `Nothing playing` / `No active device` | 先在某个 Spotify 应用中开始播放；本板无法自行启动设备 |
| `Link expired` | refresh token 已被撤销或过期；在 Setup 中重新登录 |
| `Spotify is busy` | 被限流，正在等待 `Retry-After`。关闭共用同一开发者账号配额的其他应用 |
| 浏览器中出现 `INVALID_CLIENT: Invalid redirect URI` | 控制台中的 Redirect URI 必须精确为 `http://127.0.0.1:8888/callback` |
| 标题中出现 `?` | 字符不在内置中日韩子集中；用 `spotify` 命令查看原文 |

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `main/spot_net.c` | token 续期、轮询、播放命令；在 `home_net` 任务中运行 |
| `main/spot_logic.c` | 纯逻辑：播放状态解析、token 回复、HTTP 结果分类、轮询时机、页面提示、文字转换 |
| `main/spot_json.c` | 固定内存的流式 JSON 扫描器 |
| `main/spot_net.c`（`do_cover`） | 下载专辑封面并用 ROM TJpgDec 解码 |
| `main/home_ui.c` | 正在播放页面（`build_spotify`、`refresh_spotify`） |
| `main/home_logic.c` | 按键映射（`home_nav_handle`）和 `spotify` 控制台命令 |
| `main/home_store.c` | NVS 键 `sp_cid` 和 `sp_rt` |
| `main/setup_portal.c`、`main/setup_logic.c` | Setup 页面：发起登录、检查粘贴的地址、请求换取 token |
| `tools/spotify_auth.py` | 另一种方式：在电脑上一次性完成 PKCE 登录 |
| `tests/test_spot_logic.c` | 扫描器和逻辑的主机测试，由 `tools/validate.sh` 运行 |
