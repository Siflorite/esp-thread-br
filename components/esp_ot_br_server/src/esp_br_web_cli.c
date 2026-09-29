/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_br_web_cli.h"
#include "esp_br_web_cli_ring.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if CONFIG_OPENTHREAD_CLI
#include "esp_console.h"
#endif

/* Output capture callbacks never allocate, log, or perform network I/O. */
#define RECORD_SIZE CLI_RING_TEXT_SIZE
#define COMMAND_SIZE 256
#define READ_COUNT 16
static uint8_t s_storage[CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE];
static cli_ring_t s_ring = CLI_RING_INITIALIZER(s_storage);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_output;
static bool s_ready;
static uint32_t s_session;
#if CONFIG_OPENTHREAD_CLI
static SemaphoreHandle_t s_console_mutex;
static bool s_command_active;
/* Log text is captured by log_output(), not again by the console FILE tee. */
static __thread bool s_forwarding_log;
#endif

/* OpenThread prints hex output one byte (two characters) at a time.
 * Accumulate console fragments until newline/full/source change or the next read.
 * Protected by s_mux; producers still never allocate or wait for network I/O. */
static char s_cli_pending[RECORD_SIZE];
static size_t s_cli_pending_length;

static void flush_cli_pending(void)
{
    if (s_cli_pending_length) {
        cli_ring_append(&s_ring, 'C', s_cli_pending, s_cli_pending_length);
        s_cli_pending_length = 0;
    }
}

static void append_record(char source, const char *text)
{
    size_t length = strlen(text);
    portENTER_CRITICAL(&s_mux);
    if (source == 'C') {
        /* Keep each formatted callback intact at a capacity boundary so a
         * UTF-8 character cannot be split between JSON strings. */
        if (length > sizeof(s_cli_pending) - 1 - s_cli_pending_length) {
            flush_cli_pending();
        }
        for (size_t i = 0; i < length; i++) {
            s_cli_pending[s_cli_pending_length++] = text[i];
            if (text[i] == '\n' || s_cli_pending_length == sizeof(s_cli_pending) - 1) {
                flush_cli_pending();
            }
        }
    } else {
        flush_cli_pending(); /* Preserve order when logs or commands interleave. */
        cli_ring_append(&s_ring, source, text, length);
    }
    portEXIT_CRITICAL(&s_mux);
}

static void capture(char source, const char *format, va_list args)
{
    char text[RECORD_SIZE];
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(text, sizeof(text), format, copy);
    va_end(copy);
    if (length > 0) {
        if ((size_t)length >= sizeof(text)) {
            static const char truncated[] = " [truncated]\r\n";
            memcpy(text + sizeof(text) - sizeof(truncated), truncated, sizeof(truncated));
        }
        append_record(source, text);
    }
}

static int log_output(const char *format, va_list args)
{
    capture('L', format, args);
#if CONFIG_OPENTHREAD_CLI
    bool forwarding = s_forwarding_log;
    s_forwarding_log = true;
#endif
    int result = s_log_output ? s_log_output(format, args) : vprintf(format, args);
#if CONFIG_OPENTHREAD_CLI
    s_forwarding_log = forwarding;
#endif
    return result;
}

void esp_br_web_cli_init(void)
{
    /* Called once during application startup, before the serial console starts. */
#if CONFIG_OPENTHREAD_CLI
    if (!s_console_mutex) s_console_mutex = xSemaphoreCreateMutex();
    s_ready = s_console_mutex != NULL;
#endif
    if (!s_log_output) {
        s_session = esp_random();
        s_log_output = esp_log_set_vprintf(log_output);
    }
}

#if CONFIG_OPENTHREAD_CLI
/* esp_console_run uses shared parsing/argtable storage. Route both the serial
 * REPL and the Web worker through this wrapper and reject concurrent commands.
 * The registered OT handler retains IDF's normal queue and completion wait. */
esp_err_t __real_esp_console_run(const char *line, int *command_result);

static int console_write(void *context, const char *text, int length)
{
    if (!s_forwarding_log) {
        for (int offset = 0; offset < length;) {
            char part[RECORD_SIZE];
            size_t count = (size_t)(length - offset);
            if (count >= sizeof(part)) {
                count = sizeof(part) - 1;
                /* Do not split a UTF-8 character between JSON strings. */
                while (count && ((unsigned char)text[offset + count] & 0xc0) == 0x80) count--;
                if (!count) count = sizeof(part) - 1;
            }
            memcpy(part, text + offset, count);
            part[count] = 0;
            append_record('C', part);
            offset += count;
        }
    }
    /* Forward captured text to the original serial stream. */
    return (int)fwrite(text, 1, length, (FILE *)context);
}

esp_err_t __wrap_esp_console_run(const char *line, int *command_result)
{
    if (!s_console_mutex || xSemaphoreTake(s_console_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *original_stdout = stdout, *original_stderr = stderr;
    FILE *captured_stdout = funopen(original_stdout, NULL, console_write, NULL, NULL);
    FILE *captured_stderr = funopen(original_stderr, NULL, console_write, NULL, NULL);
    if (!captured_stdout || !captured_stderr) {
        if (captured_stdout) fclose(captured_stdout);
        if (captured_stderr) fclose(captured_stderr);
        xSemaphoreGive(s_console_mutex);
        return ESP_ERR_NO_MEM;
    }
    setvbuf(captured_stdout, NULL, _IONBF, 0);
    setvbuf(captured_stderr, NULL, _IONBF, 0);
    /* The current SDK uses shared stdout/stderr, so OT task output also passes through this tee. */
    stdout = captured_stdout;
    stderr = captured_stderr;
    append_record('I', line);
    esp_err_t result = __real_esp_console_run(line, command_result);
    stdout = original_stdout;
    stderr = original_stderr;
    fclose(captured_stdout);
    fclose(captured_stderr);
    xSemaphoreGive(s_console_mutex);
    return result;
}

static void console_worker(void *arg)
{
    char *line = arg;
    int command_result = 0;
    esp_err_t result = __wrap_esp_console_run(line, &command_result);
    char message[128];
    if (result == ESP_ERR_NOT_FOUND) {
        snprintf(message, sizeof(message), "\r\n[Console: unrecognized command]\r\n");
    } else if (result == ESP_ERR_INVALID_STATE) {
        snprintf(message, sizeof(message), "\r\n[Console: busy or unavailable; command was not executed]\r\n");
    } else if (result != ESP_OK) {
        snprintf(message, sizeof(message), "\r\n[Console: %s]\r\n", esp_err_to_name(result));
    } else if (command_result != 0) {
        snprintf(message, sizeof(message), "\r\n[Console: command returned %d]\r\n", command_result);
    } else {
        message[0] = 0;
    }
    if (message[0]) append_record('C', message);
    free(line);
    portENTER_CRITICAL(&s_mux);
    s_command_active = false;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

#endif

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

static void stream_worker(void *arg)
{
    cli_stream_t *stream = arg;
    httpd_req_t *req = stream->req;
    uint64_t cursor = stream->cursor;
    bool initial = true, last_ready = false;
    TickType_t heartbeat = xTaskGetTickCount();
    cli_ring_reader_t reader;
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
        bool ready = s_ready;
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
        if (event || initial || reader.seq < first) reader = begin;
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
                bool found = cli_ring_read(&s_ring, &reader, skip ? NULL : &record);
                portEXIT_CRITICAL(&s_mux);
                if (!found) break; /* Next iteration sends gap and repositions. */
                if (skip) continue;
                char seq[24];
                snprintf(seq, sizeof(seq), "%" PRIu64, record.seq);
                cJSON *item = cJSON_CreateObject();
                if (!item || !cJSON_AddStringToObject(item, "seq", seq) ||
                    !cJSON_AddStringToObject(item, "source", record.source == 'L' ? "log" :
                                            record.source == 'I' ? "input" : "cli") ||
                    !cJSON_AddStringToObject(item, "text", record.text) || !cJSON_AddItemToArray(records, item)) {
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
    cli_no_delay(req);
    /* JSON-only same-origin fetch: reject cross-site browser form submissions. */
    char type[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8"))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected application/json");
    }
    if (req->content_len == 0 || req->content_len >= 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command length");
    }
    char body[1024];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Incomplete request");
        }
        received += n;
    }
    body[received] = 0;
    /* cJSON strings cannot represent an embedded NUL without truncation. */
    if (memchr(body, 0, received) || strstr(body, "\\u0000")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
    }
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    cJSON *command = cJSON_GetObjectItemCaseSensitive(json, "command");
    char line[COMMAND_SIZE];
    bool valid = cJSON_IsString(command) && command->valuestring && strlen(command->valuestring) < sizeof(line);
    if (valid) {
        strcpy(line, command->valuestring);
    }
    cJSON_Delete(json);
    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected a command of 1-255 bytes");
    }
    valid = false;
    for (const char *p = line; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Control characters are not allowed");
        }
        if (*p != ' ') valid = true;
    }
    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter one non-empty console command");
    }
#if CONFIG_OPENTHREAD_CLI
    portENTER_CRITICAL(&s_mux);
    bool available = s_ready && !s_command_active;
    if (available) s_command_active = true;
    portEXIT_CRITICAL(&s_mux);
    if (available) {
        char *copy = strdup(line);
        if (copy && xTaskCreate(console_worker, "br_web_console", 8192, copy,
                                tskIDLE_PRIORITY + 1, NULL) == pdPASS) {
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(req, "{\"accepted\":true}");
        }
        free(copy);
        portENTER_CRITICAL(&s_mux);
        s_command_active = false;
        portEXIT_CRITICAL(&s_mux);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start console command");
    }
#endif
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Console unavailable or a Web command is still running");
}
