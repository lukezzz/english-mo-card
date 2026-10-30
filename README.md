# 墨卡 / English Flash Cards

面向 4.2 英寸三色电子墨水屏的英语单词闪卡服务。FastAPI 同时提供管理网页和 JSON API；SQLite 保存词卡资料，PNG 文件保存在本地。

## 启动

```bash
cp .env.example .env
# 在 .env 填入 OPENAI_API_KEY
uv sync
uv run --env-file .env uvicorn main:app --reload
```

访问 <http://127.0.0.1:8000>。首次运行会创建 `data/flashcards.db`；已有数据库会自动迁移新增的单词本和图片状态字段。

## 工作方式

1. 新建单词本，并将一个单词加入任意多个单词本；也可在词卡上直接调整归属。
2. 输入单词后点击“自动补全”：服务从 Dictionary API 获取 IPA 与简明释义，并用本地规则估算音节。批量导入会在后台自动补全新卡片，且不会覆盖已有手工内容。超过 52 个字符的释义不会显示在闪卡上，也不会传给图片生成模型。
3. 单张“强制重新生成”必定调用 OpenAI；“后台批量生成”会跳过已存在的 `data/cards/<word>.png`，并在网页显示进度与失败状态。
4. 在 `.env` 设置 `EPD_UPLOAD_URL=http://<ESP32 地址>` 后，点击“发送至 EPD”：服务会将 400×300 的闪卡量化为白、红、黑三色，先调用 ESP32 的 `/api/panel` 选择 `gdey042z98` / `tricolor`，再向 `/api/frame/raw` 上传 30,000 字节帧数据以刷新屏幕。若网关需要认证，填入 `EPD_API_TOKEN`，会作为 Bearer token 发送。
5. “EPD 自动刷新”可选择全部单词或一个单词本，并设置 1–1440 分钟的发送间隔（默认 10 分钟）。服务会从范围内已生成的闪卡随机抽取一张发送；配置保存在数据库，关闭管理页面后仍会继续，点击“停止”即可暂停。

## API

| 方法 | 地址 | 作用 |
| --- | --- | --- |
| GET /api/cards | 分页列出单词卡；支持 `page`、`page_size`、`q`、`sort` 和 `book_id` |
| POST /api/cards | 新建单词卡 |
| POST /api/cards/bulk | 从逐行文本批量新建单词卡 |
| PUT /api/cards/{id} | 修改词卡文字资料 |
| PUT /api/cards/{id}/books | 替换该单词所属的单词本 |
| DELETE /api/cards/{id} | 删除词卡与本地图片 |
| GET/POST/DELETE /api/books | 管理单词本 |
| POST /api/cards/enrich | 自动建议 IPA、音节、记忆提示 |
| POST /api/cards/{id}/generate | 用 OpenAI 生成插图并合成闪卡 |
| POST /api/images/generate-batch | 后台生成当前范围的缺失图片 |
| GET /api/images/progress | 查询批量生成进度 |
| GET /api/cards/{id}/image | 获取 400×300 PNG |
| POST /api/cards/{id}/review | 记录一次复习 |
| POST /api/cards/{id}/epd | 转换为 4.2 英寸三色帧并刷新 ESP32 EPD |
| GET/PUT /api/epd/auto-refresh | 查询或设置 EPD 自动刷新 |

`POST /api/cards` 的请求体示例：

```json
{"word":"apple","ipa":"/ˈæp.əl/","syllables":"ap-ple","hint":"a red apple"}
```

## XIAO ESP32-C3 墨水屏接收端

`esp/` 是 PlatformIO Arduino 固件。它连接 Wi-Fi，并在 80 端口接收本项目服务器推送的图片；设备与服务器需要能通过网络互相访问。

1. 将 `esp/src/config.example.h` 复制为 `esp/src/config.h`，填写 `WIFI_SSID`、`WIFI_PASSWORD`。如需认证，设置 `EPD_API_TOKEN` 并与服务端 `.env` 中的同名值保持一致；留空则不验证。配置文件不会提交到 Git。
2. 按下表连接屏幕驱动板；如接线不同，修改 `config.h` 中的 GPIO 编号。供电和电平请以所用屏幕驱动板规格为准。

   | EPD 信号 | XIAO 引脚 | GPIO |
   | --- | --- | --- |
   | BUSY | D2 | 4 |
   | CS | D1 | 3 |
   | DC | D3 | 5 |
   | RST | D0 | 2 |
   | SCK / CLK | D8 | 8 |
   | MOSI / DIN | D10 | 10 |

   MISO 使用 D9 / GPIO9 初始化，但屏幕只接收数据，该引脚无需接线。固件会在 Wi-Fi 取得 IP 后启动 HTTP 服务，断线后暂停服务，并在重连后重新启动。

3. 使用 PlatformIO 编译、烧录，并打开 115200 波特率串口监视器：`pio run -d esp -t upload`，然后 `pio device monitor -d esp -b 115200`。串口会显示连接状态、IP、每个接收数据块的大小、帧校验及刷新结果。
4. 将服务端 `.env` 的 `EPD_UPLOAD_URL` 设置为串口打印的 `http://<设备 IP>`，重启服务，再从网页发送闪卡。设备断线后会自动重连；路由器中为设备保留 IP 可避免地址变化。

服务端先通过 `POST /api/panel` 选择 `gdey042z98` 和 `tricolor`，再通过 `POST /api/frame/raw` 以 `application/octet-stream` 发送 30,000 字节（HTTP Content-Length 必须为 30000）。新后端与新固件需要一起更新；旧的 multipart `/api/frame` 仅保留兼容用途。每字节包含四个像素，从高位到低位分别为白色 `00`、红色 `10`、黑色 `11`。固件只接受完整帧，刷新成功后才返回成功；屏幕 BUSY 超时会返回错误。`GET /api/health` 可用于检查设备是否在线。

### ESP32 离线诊断与恢复

固件启用 Wi-Fi 自动重连，并每 30 秒补充尝试一次；连续失败时，每第 4 次尝试关闭再重新初始化 Wi-Fi。关闭 Wi-Fi 省电以减少排查变量（会增加功耗）。收到断线、丢失 IP 或重新取得 IP 的事件后，会重新绑定 HTTP 服务；即使事件发生在屏幕刷新期间，也会在刷新结束后处理。网络事件从回调转交主循环处理，不在回调中操作 Flash 或 HTTP。

- `GET http://<设备 IP>/api/status`：当前 IP、RSSI、运行时间、启动次数、重启原因、剩余内存、历史最低剩余内存和最大可分配内存块。
- `GET http://<设备 IP>/api/debug`：上述状态加最近 24 条诊断记录。若配置了 `EPD_API_TOKEN`，这两个接口都需要 `Authorization: Bearer <token>`。
- 串口监视器输入 `status` 或 `logs` 并发送换行（或发送后等待 250ms）：设备没有网络时也可读取相同信息；屏幕刷新期间命令会在刷新结束后处理。

诊断记录包括启动原因、Wi-Fi 断开原因码、丢失/取得 IP、重连尝试、刷新开始/结束/失败，以及每 5 分钟的内存和 Wi-Fi 心跳。每条记录带启动编号和本次启动后的秒数，不含 Wi-Fi 密码和 API token。逐个上传数据块仍只输出到串口。

默认通过 Preferences/NVS 将这 24 条记录循环保存到 Flash，重启后可查看。启动时保存一次，运行期间有新记录时最多每 60 秒保存一次；刷新阻塞主循环时，保存会延后。因此突然断电或崩溃可能丢失最近约一分钟或一次刷新期间的记录，也不会保存完整崩溃堆栈。排查结束可在本地 `config.h` 添加 `#define EPD_PERSIST_DEBUG 0` 并重新烧录，停止后续 Flash 日志读写。

离线时先检查路由器中设备是否换了 IP，并建议为其配置 DHCP 地址保留。若诊断显示 `brownout`，检查供电与线缆；`panic` 或 `watchdog` 需要结合串口崩溃输出；启动编号不变而反复出现断线原因码时，优先检查 Wi-Fi 信号和接入点。只有 HTTP 暂时不可访问、随后刷新完成恢复的情况，可能只是当前同步刷新占用主循环。断线恢复仍需在实机上通过临时关闭/开启接入点、连续刷新等方式验证。


### HTTP 接收卡住的恢复

Arduino 2.x WebServer 的 multipart 接收可能一直等待未关闭的 TCP 连接。后端现改用 `/api/frame/raw`，空闲读超时为 2 秒，接收过程中检查 15 秒总时限，异常中断不会刷新不完整帧。该接口仍使用固定的 30,000 字节缓冲区。最后一块不足库内读取块大小时，可能额外等待一次读超时。

固件监视主循环进展；若主循环持续 180 秒不能返回，将触发任务看门狗并重启。此时 Wi-Fi 和串口命令都可能暂时无法响应，恢复后再读取 `logs`。180 秒阈值留出了屏幕驱动各阶段 BUSY 等待的总时间，不会因正常约 20 秒的刷新触发。此措施用于恢复并定位故障，不代表已确认所有卡死原因。

固件通过 RTC 内存保留最近的执行位置（不增加 Flash 写入），在保留 RTC 内容的异常重启后打印并写入 Flash：`PREVIOUS stage=... up=... progress=...`。`raw_upload` 的 progress 是已接收字节数；`http_reply` 表示正在发送响应；`http_handle_client` 表示 HTTP 解析/处理尚未返回。掉电后这条 RTC 信息不保证保留。`/api/panel` 增加了发送响应前后的日志，用来区分响应发送阻塞与下一次上传阻塞。
