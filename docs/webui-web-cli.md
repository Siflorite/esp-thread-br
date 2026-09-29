# WebUI 设计与 Web CLI

项目的 WebUI 是 ESP HTTP Server + SPIFFS 静态页面 + 原生 JavaScript。此次在 **Management → Tools** 增加 Web CLI，可读取 BR 日志并执行与串口相同的控制台命令。

## 使用

启用 `CONFIG_OPENTHREAD_BR_START_WEB=y` 后，在 `idf.py menuconfig` → `ESP Thread Border Router Example` 中开启 `Enable Web CLI and BR log capture`。执行命令还需要 `CONFIG_OPENTHREAD_CLI=y`。重新编译并同时刷入应用和 `web_storage` 分区，浏览器刷新 `/tools.html` 后使用。仅更新应用的 OTA 不会更新 SPIFFS 页面。

Web CLI 默认关闭，可在项目的 `sdkconfig.defaults` 中配置：

```ini
CONFIG_OPENTHREAD_BR_START_WEB=y
CONFIG_OPENTHREAD_CLI=y
CONFIG_ESP_BR_WEB_CLI=y
CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE=16384
```

已有 `sdkconfig` 的项目请用 menuconfig 调整实际配置；defaults 不会覆盖已有值。

缓存改为按字节配置，范围 1–64 KiB，默认 16 KiB。`CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE` 就是预分配字节数组的大小；此外还有少量环形缓冲管理状态。关闭 `CONFIG_ESP_BR_WEB_CLI` 时，缓存、缓存实现、日志 hook 和 CLI 链接包装均不编译／启用。

旧配置 `CONFIG_ESP_BR_WEB_CLI_RECORD_COUNT` 不再使用，重新配置时采用新的默认容量；如有自定义需求，请在 menuconfig 或项目配置中显式设置字节数。例如原先 64 条固定记录占约 17 KiB，可改为 `CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE=16384`。条数不能直接当成字节数迁移。

每条记录保存 12 字节记录头（序号、文本长度、来源、保留字节），紧跟实际文本，不存结构体对齐填充或字符串结束符。例如 2 字节换行占 14 字节，255 字节片段占 267 字节。默认 16 KiB 在仅包含这两种片段时分别可容纳 1170 条或 61 条；实际容量随文本长度变化。

网页通过 SSE 接收输出，每批最多 16 个片段，原始文本最多约 4 KiB。缓存仍为一个共享字节数组，不为客户端复制。只允许一个 SSE 订阅，使用一个固定连接上下文和一个 4096 字节栈的发送任务，另有任务控制块、HTTP 异步请求副本、TCP 缓冲及 JSON／事件帧临时分配，实际内存峰值需上板测量。客户端断开／发送失败后释放请求和任务。无客户端时没有 SSE 任务。网页不轮询输出。

关闭后 `/cli` 和 `/cli/events` 路由不注册；页面从小型 `/web/features` 接口读取编译功能，隐藏 CLI 且不建立 SSE 连接。共享 SPIFFS 内仍保留静态 HTML／JS，这占用 Flash，不会保留设备端的日志环形缓冲。

输入 `ot state`、`ot wifi ...` 或 `help`，按 Enter 发送，与当前 BR 串口控制台使用同一套命令注册表。网页和 HTTP 后端均不添加或删除 `ot` 前缀，不裁剪首尾空格；不认识的命令按控制台规则报错。当前工程的 `wifi` 是 OT 扩展子命令，需使用 `ot wifi ...`。OT 命令仍按原样输出 `Done`／`Error`；成功的控制台返回值 0 不再添加额外文本。方向键浏览最近 50 条命令，BR LOG 开关隐藏／显示日志，Pause 暂停读取，Clear 只清除当前页面显示。

Thread 为 disabled / detached 时仍可操作 CLI；Ping 单独根据连接状态禁用。HTTP 管理网络必须可达；改变 Wi-Fi、重启或恢复出厂配置可能使页面断线。

现有 HTTP 服务没有认证或 TLS，新增接口沿用这一访问边界。可访问管理地址的客户端能够查看日志并执行具有设备权限的 OT 命令，因此该服务应仅在受信任的管理网络使用。JSON 内容类型限制不能替代认证。

## 原有设计

| 层 | 文件 | 职责 |
|---|---|---|
| 页面 | `components/esp_ot_br_server/frontend/*.html` | Dashboard、Network、Commissioner、Addresses、Tools、Topology、About 等独立页面 |
| 公共前端 | `frontend/static/api.js`、`style.css` | Fetch 封装、导航、缓存、状态提示及卡片布局；不依赖 SPA 框架 |
| HTTP 入口 | `src/esp_br_web.c` | 收到 STA／ETH IP 事件后启动服务，注册 REST 与 GUI 接口，最后注册静态资源通配路由 |
| 业务处理 | `src/esp_br_web_api.c`、`esp_br_web_base.c` | OpenThread 查询和操作、数据转换与 JSON 封装 |
| 打包 | `components/esp_ot_br_server/CMakeLists.txt` | 将 frontend 打包进 `web_storage` SPIFFS 分区，favicon 嵌入固件 |

原生 REST 接口如 `/node/state` 与面向页面的 `/ping` 等接口共存，返回结构并非完全相同。现有 Tools 只有 Ping，且原先调用 `disableManagementPage()` 整页禁用，不适合断网诊断。

## 新增数据流

```mermaid
flowchart LR
    UI[Tools Web CLI] -->|POST /cli| HTTP[ESP HTTP Server]
    HTTP -->|复制原始命令| Worker[独立控制台命令任务]
    Worker --> Run[esp_console_run 与串口共用命令注册表]
    Run -->|ot 命令的原注册回调| Queue[OpenThread 任务队列]
    Run -->|其他命令| Handler[控制台处理函数]
    Handler -->|当前任务 stdout 与 stderr 副本| Ring
    Run -->|完整原始输入| Ring
    Queue --> Interpreter[现有 OT CLI 解释器]
    Interpreter --> Tee[CLI 回调复制]
    Tee --> Serial[原串口回调及完成通知]
    Tee --> Ring[固定大小日志环形缓冲区]
    Log[ESP-IDF 日志回调] --> Ring
    Log --> Console[原日志输出]
    UI -->|GET /cli/events 订阅| SSE[单个 SSE 发送任务]
    Ring -->|按续传游标复制| SSE
    SSE -->|records/status/gap/reset| UI
```

`esp_br_web_cli.c` 用 `esp_log_set_vprintf()` 复制启动 Web 功能之后的常规 ESP-IDF 日志，继续调用原日志输出函数。日志回调可被多任务调用，缓冲区写入用短临界区保护，格式化、JSON 分配和 HTTP 发送都在临界区之外。

`esp_br_web_cli_ring.c` 实现无动态分配的变长记录字节环形缓冲，由调用者加锁。记录头和文本均可跨缓冲区末尾，按两段复制；空间不足时逐条覆盖最旧的完整记录，不等待客户端。读取不删除记录，各请求以各自的序号续读。读取时每次只在临界区内跳过或复制一条记录，锁外序列化和发送；若读取期间目标记录被覆盖，停止本批读取，发送任务下一轮重新定位并发送 `gap`。

OT 命令的原控制台处理函数会将命令投递到 OpenThread 任务，输出仍由 SDK 原回调写入控制台标准流并通知串口 REPL 完成。Web CLI 只在 `esp_console_run` 期间采集控制台输出，不再另行复制 OT 输出回调，避免同一段文本出现两遍。输入由 `--wrap=esp_console_run` 记录，原命令文本不加或删前缀。

HTTP POST 保留命令原文，复制到一个独立的 8192 字节栈命令任务；该任务调用 `esp_console_run()`，与串口共用注册表及参数解析。`ot` 的原注册处理函数继续使用 `esp_openthread_cli_input()` 投递到 OT 任务并等待原有完成通知；其他命令直接运行自己的处理函数。202 仅表示已接受异步执行，不代表命令成功。控制台返回非零值或调用失败时通过 SSE 输出错误提示；成功返回 0 不另加提示。

`esp_console_run` 使用共享解析缓冲和命令参数结构，所以网页与串口的调用均经过同一个互斥包装；命令执行期间其他控制台调用立即返回 busy，不排队、不自动重放。最多一个网页命令任务，结束后释放；断开 SSE 不会取消已接受的命令。

控制台包装临时将 `stdout`／`stderr` 接到 FILE tee，复制普通打印内容到 ring，同时保留原串口输出，返回后恢复。当前 SDK 的标准流是全局的，因此 tee 生效期间，OT 任务原回调的 `vprintf()` 和其他任务的普通输出也会被采集。ESP 日志已经由日志 hook 采集，tee 不重复采集同一份日志。独立于控制台命令的 ESP 日志由日志 hook 持续采集。

## 接口及资源边界

完整结构见 `components/esp_ot_br_server/src/openapi.yaml` 的 `/cli` 和 `/cli/events`。

| 接口 | 行为 |
|---|---|
| `GET /cli/events` | SSE 订阅，先状态后输出；无游标时从当前最旧记录开始 |
| `GET /cli/events?after=SESSION:SEQ` | 从上次已接收事件续读；原生重连的 `Last-Event-ID` 优先于 URL 参数 |
| `POST /cli`，JSON `{"command":"ot state"}` | 接受单条 1–255 字节命令；拒绝控制字符、无效 JSON、过大请求；不可用时返回 503 |

设备按配置的字节预算保存最近的输出，每片段文本仍至多 255 字节，超长片段标记 truncated；改成字节缓冲不会消除格式化阶段的截断。OpenThread 的十六进制输出会逐字节调用回调；CLI 小片段先在 256 字节暂存区合并，遇换行、达到 255 字节、切换至日志／命令输入或下一次读取时写入 ring。没有换行的提示符也会在读取时刷新，不必等待下一条命令。网页不再周期性发 GET，而使用一个 EventSource。发送任务每轮最多复制 16 个片段，然后让出 CPU 50 ms；空闲时只在 BR 本地检查缓存，每 10 秒发送 SSE 注释心跳。接收/发送均不发生在日志回调中，POST 及其他 Web 请求继续由 HTTP 服务任务处理。每个完整 SSE 事件（包括结束空行）通过一次 HTTP chunk 调用发送；TCP 仍可能分包，浏览器收到完整事件后才处理。单页仍保留最多 800 个片段，多个事件的显示更新合并到浏览器下一帧。暂停、隐藏或离开页面会关闭订阅；恢复时携带最后收到的事件 ID 重连。清屏不重置该 ID，BR 日志也不会被删除。

### SSE 存储、标记与发送

1. 日志／CLI 回调将 `[uint64序号 + uint16长度 + 来源 + 保留字节][实际文本]` 写入共享 byte ring；启动标识 `session` 是独立的全局随机值，不重复存在每条头部。
2. 单个订阅维护续传游标和读取位置；读取时短暂加锁复制一条，锁外 JSON 编码和发送。写入者只会淘汰完整旧记录，不等待客户端。
3. `records` 事件用本批最后一条的序号作为 `cursor`；SSE `id` 是 `session:cursor`。字符串形式的 session/cursor/seq 避免 JavaScript 对 64 位整数的精度损失。事件用空行结束，完整接收后浏览器才提交其 ID。成功写入 TCP 不代表浏览器已收到，断线恢复以浏览器的 ID 为准。

```text
event: records
id: 12345:102
data: {"session":"12345","cursor":"102","ready":true,"records":[{"seq":"102","source":"cli","text":"leader\r\n"}]}

```

4. `status` 在连接建立及 CLI 可用状态变化时发送；`gap` 表示未读数据已经被覆盖，游标定位到当前最旧记录之前；`reset` 表示启动标识变化或游标超出当前序号范围，同样重新定位。三种事件的 JSON 含 session/cursor/ready，不含 records，均携带新的 ID。
5. 网络暂断由 EventSource 携带 `Last-Event-ID` 重连（建议间隔 3 秒）；若因 503 等响应进入 CLOSED，前端等待 3 秒重新创建订阅。手动恢复使用 URL `after` 参数，服务器支持冒号或 `%3A`。历史超过缓存范围只能报告 gap，不能恢复丢失日志，也不自动重发命令。

### 长连接资源与退出

最多一个 SSE 订阅，第二个订阅返回 503；HTTP 服务仍允许 7 个 socket，为命令和普通请求留出空间。SSE 使用独立任务，避免慢发送阻塞 POST。关闭旧页面后，新页面可重试接入。socket 每次发送等待上限为 2 秒（不是整个事件的绝对期限）；发送失败即退出。任务每轮以非阻塞 peek 检查对端关闭，避免闲置连接一直占着名额。该实现面向当前明文 HTTP 服务，若增加 TLS 必须同时调整这种检测方式。

停止服务器时先禁止新订阅，在生命周期互斥锁下 shutdown 所有异步请求仍持有的 socket，以打断慢发送，再等待任务恰好一次 complete 请求，最后停止 HTTP server。shutdown 不直接 close fd；真正关闭交还 HTTP server，避免已释放 fd 被复用的竞态。生命周期互斥锁不用于日志写入，也不在生产者临界区中执行网络操作。此互斥锁首次启动创建并在后续服务重启时复用。

减小缓冲区、日志产生超过发送速度、长时间暂停都可能丢失较早输出。SSE 消除了网页轮询等待，但不消除 Wi-Fi 省电、网络丢包和 TCP 重传延迟。

不保存历史到 Flash，不包含初始化之前的启动日志，也不保证捕获 ROM／early log、panic、Web CLI 无控制台命令运行时其他任务绕过 ESP 日志的普通 `printf()` 输出。日志级别仍受固件原配置控制。显示使用 `textContent`，去除终端控制序列，设备文本不会作为 HTML 执行。

官方参考：[ESP-IDF 日志回调](https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32/api-reference/system/log.html)、[OpenThread CLI API](https://openthread.io/reference/group/api-cli)。实际实现以本机 SDK 源码为准。
