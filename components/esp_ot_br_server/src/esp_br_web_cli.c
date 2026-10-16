/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_br_web_cli.h"
#include "esp_br_web_cli_ring.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_stdio.h"
#include "esp_vfs.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define WEB_CLI_POST_CONTENT_TYPE_SIZE 48
#define WEB_CLI_POST_BODY_SIZE 1024

#define RECORD_SIZE CLI_RING_TEXT_SIZE
#define READ_COUNT 16
static uint8_t s_storage[CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE];
static cli_ring_t s_ring = CLI_RING_INITIALIZER(s_storage);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static uint32_t s_session;

// Batch small writes as raw bytes. No formatting or character interpretation.
static char s_cli_pending[RECORD_SIZE];
static size_t s_cli_pending_length;

static void flush_cli_pending(void)
{
    if (s_cli_pending_length) {
        cli_ring_append(&s_ring, 'C', s_cli_pending, s_cli_pending_length);
        s_cli_pending_length = 0;
    }
}

static void append_console_bytes(const char *data, size_t length)
{
    while (length) {
        portENTER_CRITICAL(&s_mux);
        size_t count = sizeof(s_cli_pending) - 1 - s_cli_pending_length;
        if (count > length) count = length;
        memcpy(s_cli_pending + s_cli_pending_length, data, count);
        s_cli_pending_length += count;
        if (s_cli_pending_length == sizeof(s_cli_pending) - 1) flush_cli_pending();
        portEXIT_CRITICAL(&s_mux);
        data += count;
        length -= count;
    }
}

#define CONSOLE_TEE_PATH "/dev/brcli"
static int s_console_fd = -1;
static FILE *s_output[2];
static __thread bool s_forwarding;

static int tee_open(const char *path, int flags, int mode)
{
    if ((flags & O_ACCMODE) != O_WRONLY) {
        errno = EACCES;
        return -1;
    }
    return strcmp(path, "/stderr") == 0 ? 1 : 0;
}

static int tee_close(int fd)
{
    return 0; // Firmware-lifetime shared backend, not owned by an individual FILE.
}

static ssize_t tee_write(int fd, const void *data, size_t length)
{
    // Fixed 80 columns for both inputs. Fail BEFORE sending the SDK's query,
    // so getColumns() falls back without reading stdin or moving the cursor.
    static const char query[] = "\x1b[6n";
    if (length == sizeof(query) && memcmp(data, query, sizeof(query)) == 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (s_forwarding) return length; // Do not recurse on driver diagnostics.
    FILE *stream = s_output[fd];
    // Reuse libc's recursive FILE lock; an extra output mutex would invert locks.
    flockfile(stream);
    s_forwarding = true;
    ssize_t written = write(s_console_fd, data, length);
    int saved_errno = errno;
    if (written > 0) append_console_bytes(data, (size_t)written);
    s_forwarding = false;
    funlockfile(stream);
    errno = saved_errno;
    return written;
}

static int tee_fstat(int fd, struct stat *info)
{
    return fstat(s_console_fd, info);
}

static int tee_fcntl(int fd, int command, int argument)
{
    return fcntl(s_console_fd, command, argument);
}

static int tee_fsync(int fd)
{
    return fsync(s_console_fd);
}

static void tee_reopen(FILE *stream, const char *path)
{
    flockfile(stream);
    if (!freopen(path, "w", stream) || setvbuf(stream, NULL, _IONBF, 0) != 0) abort();
    funlockfile(stream);
}

static esp_err_t console_tee_init(void)
{
    s_console_fd = open(ESP_VFS_DEV_CONSOLE, O_WRONLY);
    if (s_console_fd < 0) return ESP_FAIL;
    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = tee_open,
        .close = tee_close,
        .write = tee_write,
        .fstat = tee_fstat,
        .fcntl = tee_fcntl,
        .fsync = tee_fsync,
    };
    esp_err_t result = esp_vfs_register(CONSOLE_TEE_PATH, &vfs, NULL);
    if (result != ESP_OK) {
        close(s_console_fd);
        s_console_fd = -1;
        return result;
    }
    s_output[0] = stdout;
    s_output[1] = stderr;
    // Reopen IN PLACE: Picolibc globals and existing tasks' TLS aliases share FILE.
    // After destructive freopen failure the original stream cannot be restored.
    tee_reopen(stdout, CONSOLE_TEE_PATH "/stdout");
    if (stderr != stdout) tee_reopen(stderr, CONSOLE_TEE_PATH "/stderr");
    return ESP_OK;
}

void esp_br_web_cli_init(void)
{
    static bool initialized;
    if (initialized) return;
    initialized = true;
    s_session = esp_random();
    s_ready = console_tee_init() == ESP_OK;
    if (!s_ready) ESP_LOGE("br_web_cli", "Cannot capture console output");
}

bool esp_br_web_cli_capture_ready(void)
{
    return s_ready;
}

/* HTTPD sends headers and chunk framing separately. Disable Nagle for these
 * latency-sensitive responses so a short write does not wait for an ACK. */
static void cli_no_delay(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }
}

/* One debug page owns the sender task. Keep network I/O off the HTTP server
 * task so a slow stream cannot block POST or other Web UI requests. */
#define SSE_STACK_SIZE 4096
#define SSE_TICK_MS 50
#define SSE_HEARTBEAT_MS 10000
static bool s_stream_active;
static bool s_stream_stopping;

typedef struct {
    httpd_req_t *req;
    uint64_t cursor;
    bool reset;
} cli_stream_t;

/* Serialize socket shutdown with async completion, never with the producer's
 * critical section. This prevents fd reuse while server stop cancels sends. */
static SemaphoreHandle_t s_stream_mutex;
static cli_stream_t s_stream;

void esp_br_web_cli_stream_start(void)
{
    if (!s_stream_mutex) s_stream_mutex = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_mux);
    s_stream_stopping = false;
    portEXIT_CRITICAL(&s_mux);
}

void esp_br_web_cli_stream_stop(void)
{
    /* Must finish all async requests BEFORE httpd_stop frees their sessions. */
    portENTER_CRITICAL(&s_mux);
    s_stream_stopping = true;
    portEXIT_CRITICAL(&s_mux);
    if (s_stream_mutex) {
        xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
        if (s_stream.req) shutdown(httpd_req_to_sockfd(s_stream.req), SHUT_RDWR);
        xSemaphoreGive(s_stream_mutex);
    }
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        bool active = s_stream_active;
        portEXIT_CRITICAL(&s_mux);
        if (!active) return;
        vTaskDelay(pdMS_TO_TICKS(SSE_TICK_MS));
    }
}

static void stream_release(void)
{
    portENTER_CRITICAL(&s_mux);
    s_stream_active = false;
    portEXIT_CRITICAL(&s_mux);
}

/* Strict decimal parsing preserves all 64 sequence bits (unlike JS numbers).
 * httpd_query_key_value does not URL-decode, so accept the browser's %3A too. */
static bool stream_parse_id(const char *text, bool query, uint32_t *session, uint64_t *cursor)
{
    uint64_t boot = 0, seq = 0;
    const char *p = text;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        if (boot > (UINT32_MAX - (unsigned)(*p - '0')) / 10) return false;
        boot = boot * 10 + (*p++ - '0');
    }
    if (*p == ':') p++;
    else if (query && !strncmp(p, "%3", 2) && (p[2] == 'A' || p[2] == 'a')) p += 3;
    else return false;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        if (seq > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return false;
        seq = seq * 10 + (*p++ - '0');
    }
    if (*p) return false;
    *session = (uint32_t)boot;
    *cursor = seq;
    return true;
}

static cJSON *stream_payload(uint64_t cursor, bool ready)
{
    char seq[24], boot[16];
    snprintf(seq, sizeof(seq), "%" PRIu64, cursor);
    snprintf(boot, sizeof(boot), "%" PRIu32, s_session);
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "session", boot) ||
        !cJSON_AddStringToObject(json, "cursor", seq) || !cJSON_AddBoolToObject(json, "ready", ready)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/* Takes ownership of json. A blank line commits an event and its resume ID. */
static esp_err_t stream_event(httpd_req_t *req, const char *event, uint64_t cursor, cJSON *json)
{
    char header[96];
    int n = snprintf(header, sizeof(header), "event: %s\nid: %" PRIu32 ":%" PRIu64 "\ndata: ",
                     event, s_session, cursor);
    char *body = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;
    size_t body_length = strlen(body);
    size_t length = (size_t)n + body_length + 2;
    char *frame = malloc(length);
    if (!frame) {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    memcpy(frame, header, n);
    memcpy(frame + n, body, body_length);
    memcpy(frame + n + body_length, "\n\n", 2);
    cJSON_free(body);
    /* Submit the terminating blank line with the payload, not in a later chunk.
     * TCP may still split it; EventSource dispatches only complete events. */
    esp_err_t err = httpd_resp_send_chunk(req, frame, length);
    free(frame);
    return err;
}

static void stream_complete(cli_stream_t *stream)
{
    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    /* Shut down while this request still owns its session; HTTPD sees EOF
     * after complete and performs the final close. No queued close/fd race. */
    shutdown(httpd_req_to_sockfd(stream->req), SHUT_RDWR);
    esp_err_t err = httpd_req_async_handler_complete(stream->req);
    stream->req = NULL;
    xSemaphoreGive(s_stream_mutex);
    /* Even ESP_FAIL from complete means req was freed. Never retry it. */
    if (err != ESP_OK) ESP_LOGW("br_cli_sse", "Async completion wakeup failed: %d", err);
    stream_release();
}

// Prepare a JSON string at the sender, leaving the raw ring untouched.
// NUL is not log text; retain at most three unfinished UTF-8 bytes for next time.
static void stream_text(const cli_ring_record_t *record, char *text, char *tail, size_t *tail_length)
{
    size_t length = *tail_length;
    memcpy(text, tail, length);
    for (size_t i = 0; i < record->length; i++) {
        if (record->text[i]) text[length++] = record->text[i];
    }
    size_t complete = length;
    if (length) {
        size_t start = length - 1;
        while (start && ((unsigned char)text[start] & 0xc0) == 0x80) start--;
        unsigned char lead = (unsigned char)text[start];
        size_t expected = lead >= 0xc2 && lead <= 0xdf ? 2 :
                          lead >= 0xe0 && lead <= 0xef ? 3 :
                          lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
        if (length - start < expected) complete = start;
    }
    *tail_length = length - complete;
    memcpy(tail, text + complete, *tail_length);
    text[complete] = '\0';
}

static void stream_worker(void *arg)
{
    cli_stream_t *stream = arg;
    httpd_req_t *req = stream->req;
    uint64_t cursor = stream->cursor;
    bool initial = true, last_ready = false;
    TickType_t heartbeat = xTaskGetTickCount();
    cli_ring_reader_t reader;
    char tail[4];
    size_t tail_length = 0;
    portENTER_CRITICAL(&s_mux);
    reader = cli_ring_begin(&s_ring);
    portEXIT_CRITICAL(&s_mux);

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = httpd_resp_send_chunk(req, "retry: 3000\n\n", HTTPD_RESP_USE_STRLEN);
    while (err == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        flush_cli_pending();
        bool stopping = s_stream_stopping;
        uint64_t first = s_ring.first_seq, end = s_ring.next_seq;
        cli_ring_reader_t begin = cli_ring_begin(&s_ring);
        bool ready = s_ready && esp_br_web_cli_input_ready();
        portEXIT_CRITICAL(&s_mux);
        if (stopping) break;
        /* HTTPD excludes async sockets from its read loop. Detect FIN here so
         * pause/resume releases scarce slots without waiting for a heartbeat.
         * This endpoint is plain HTTP, with no request body or pipelining. */
        char peek;
        int pending = recv(httpd_req_to_sockfd(req), &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pending >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;

        const char *event = NULL;
        if ((initial && stream->reset) || cursor >= end) {
            cursor = first - 1;
            event = "reset";
        } else if (cursor < first - 1) {
            if (!initial || cursor != 0) event = "gap";
            cursor = first - 1;
        }
        if (event || initial || reader.seq < first) {
            reader = begin;
            tail_length = 0;
        }
        if (event) {
            err = stream_event(req, event, cursor, stream_payload(cursor, ready));
            if (err != ESP_OK) break;
        }
        if (initial || ready != last_ready) {
            err = stream_event(req, "status", cursor, stream_payload(cursor, ready));
            if (err != ESP_OK) break;
            last_ready = ready;
        }
        initial = false;

        if (cursor + 1 < end) {
            cJSON *json = stream_payload(cursor, ready);
            cJSON *records = json ? cJSON_AddArrayToObject(json, "records") : NULL;
            if (!records) { cJSON_Delete(json); break; }
            uint64_t sent_cursor = cursor;
            unsigned count = 0;
            /* Bound work even when seeking through a large retained history.
             * Each read/skip holds the producer lock for at most one fragment. */
            for (unsigned scanned = 0; scanned < 64 && count < READ_COUNT && reader.seq < end; scanned++) {
                cli_ring_record_t record;
                bool skip = reader.seq <= cursor;
                portENTER_CRITICAL(&s_mux);
                bool found = cli_ring_read(&s_ring, &reader, &record);
                portEXIT_CRITICAL(&s_mux);
                if (!found) break; /* Next iteration sends gap and repositions. */
                char text[CLI_RING_TEXT_SIZE + 4];
                // Rebuild text state while seeking, without replaying old text.
                stream_text(&record, text, tail, &tail_length);
                if (skip) continue;
                char seq[24];
                snprintf(seq, sizeof(seq), "%" PRIu64, record.seq);
                cJSON *item = cJSON_CreateObject();
                if (!item || !cJSON_AddStringToObject(item, "seq", seq) ||
                    !cJSON_AddStringToObject(item, "source", "cli") ||
                    !cJSON_AddStringToObject(item, "text", text) || !cJSON_AddItemToArray(records, item)) {
                    cJSON_Delete(item);
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                sent_cursor = record.seq;
                count++;
            }
            if (err == ESP_OK && count) {
                char seq[24];
                snprintf(seq, sizeof(seq), "%" PRIu64, sent_cursor);
                if (!cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(json, "cursor"), seq)) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    err = stream_event(req, "records", sent_cursor, json);
                    json = NULL;
                    if (err == ESP_OK) cursor = sent_cursor;
                }
            }
            cJSON_Delete(json);
        }
        TickType_t now = xTaskGetTickCount();
        if (err == ESP_OK && now - heartbeat >= pdMS_TO_TICKS(SSE_HEARTBEAT_MS)) {
            err = httpd_resp_send_chunk(req, ": heartbeat\n\n", HTTPD_RESP_USE_STRLEN);
            heartbeat = now;
        }
        /* Device-local checking only: there is no browser polling. Yield even
         * under sustained output, with at most 16 records per 50 ms batch. */
        if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(SSE_TICK_MS));
    }
    stream_complete(stream);
    vTaskDelete(NULL);
}

esp_err_t esp_br_web_cli_events_handler(httpd_req_t *req)
{
    if (req->content_len) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected body");
    uint64_t cursor = 0;
    uint32_t session = s_session;
    char id[40], query[64];
    bool has_id = false;
    size_t header_len = httpd_req_get_hdr_value_len(req, "Last-Event-ID");
    if (header_len) {
        /* Native reconnect ID takes precedence over the original query URL. */
        if (header_len >= sizeof(id) ||
            httpd_req_get_hdr_value_str(req, "Last-Event-ID", id, sizeof(id)) != ESP_OK ||
            !stream_parse_id(id, false, &session, &cursor)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid event ID");
        }
        has_id = true;
    } else if (httpd_req_get_url_query_len(req)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "after", id, sizeof(id)) != ESP_OK ||
            !stream_parse_id(id, true, &session, &cursor)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid event cursor");
        }
        has_id = true;
    }
    portENTER_CRITICAL(&s_mux);
    bool available = s_stream_mutex && !s_stream_stopping && !s_stream_active;
    if (available) s_stream_active = true;
    portEXIT_CRITICAL(&s_mux);
    if (!available) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "3");
        return httpd_resp_sendstr(req, "Web CLI stream limit reached or server stopping");
    }
    cli_stream_t *stream = &s_stream;
    stream->cursor = cursor;
    stream->reset = has_id && session != s_session;
    cli_no_delay(req);
    httpd_req_t *async_req = NULL;
    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        /* Do not leave a send timeout on a socket that stays keep-alive for
         * other endpoints when this request never became a stream. */
        struct timeval none = {0};
        setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
        stream_release();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start stream");
    }
    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    stream->req = async_req;
    portENTER_CRITICAL(&s_mux);
    bool stopping = s_stream_stopping;
    portEXIT_CRITICAL(&s_mux);
    /* Stop may have scanned the registry while this handler was in begin(). */
    if (stopping) shutdown(httpd_req_to_sockfd(stream->req), SHUT_RDWR);
    xSemaphoreGive(s_stream_mutex);
    if (stopping) {
        stream_complete(stream);
    } else if (xTaskCreate(stream_worker, "br_cli_sse", SSE_STACK_SIZE, stream, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        httpd_resp_send_err(stream->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create stream task");
        stream_complete(stream);
    }
    return ESP_OK;
}

esp_err_t esp_br_web_cli_post_handler(httpd_req_t *req)
{
    if (!s_ready || !esp_br_web_cli_input_ready()) {
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "Console unavailable");
    }

    cJSON *json = NULL;
    httpd_err_code_t error_code = HTTPD_400_BAD_REQUEST;
    const char *error_message = NULL;

    // Require JSON to reject cross-site browser form submissions.
    char type[WEB_CLI_POST_CONTENT_TYPE_SIZE];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8"))) {
        error_message = "Expected application/json";
        goto exit;
    }

    char body[WEB_CLI_POST_BODY_SIZE];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        error_message = "Invalid request body length";
        goto exit;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            error_code = HTTPD_408_REQ_TIMEOUT;
            error_message = "Incomplete request";
            goto exit;
        }
        received += n;
    }
    body[received] = '\0';

    // cJSON strings cannot represent an embedded NUL without truncation.
    if (memchr(body, '\0', received) || strstr(body, "\\u0000")) {
        error_message = "Invalid command";
        goto exit;
    }

    json = cJSON_ParseWithOpts(body, NULL, true);
    if (!json) {
        error_message = "Invalid JSON";
        goto exit;
    }
    cJSON *command = cJSON_GetObjectItemCaseSensitive(json, "command");
    if (!cJSON_IsString(command) || !command->valuestring) {
        error_message = "Expected a command string";
        goto exit;
    }
    const char *line = command->valuestring;
    size_t command_length = strlen(line);
    if (command_length >= WEB_CLI_COMMAND_SIZE) {
        error_message = "Expected a command of 1-255 bytes";
        goto exit;
    }

    bool has_non_space = false;
    for (const char *p = line; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
            error_message = "Control characters are not allowed";
            goto exit;
        }
        if (*p != ' ') {
            has_non_space = true;
        }
    }
    if (!has_non_space) {
        error_message = "Expected non-empty console command";
        goto exit;
    }

    // Copy into the fixed input slot before freeing the request's JSON.
    esp_err_t submitted = esp_br_web_cli_submit(line, command_length);
    cJSON_Delete(json);
    json = NULL;
    if (submitted != ESP_OK) {
        return httpd_resp_send_custom_err(req, "503 Service Unavailable",
                                         submitted == ESP_ERR_TIMEOUT ? "Console input buffer is full" : "Console unavailable");
    }

    // 202 means accepted, not executed. Response failure does not retract input.
    ESP_RETURN_ON_ERROR(httpd_resp_set_status(req, "202 Accepted"), "br_web_cli", "Failed to set response status");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Cache-Control", "no-store"), "br_web_cli",
                        "Failed to set cache control");
    return httpd_resp_send(req, NULL, 0);

exit:
    cJSON_Delete(json);
    return httpd_resp_send_err(req, error_code, error_message);
}
