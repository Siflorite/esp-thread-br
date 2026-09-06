/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <ctype.h>
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
#include "esp_openthread_cli.h"
#include "openthread/cli.h"
#endif

/* Fixed RAM usage; callbacks never allocate, log, or perform network I/O. */
#define RECORD_SIZE CLI_RING_TEXT_SIZE
#define COMMAND_SIZE 256
#define READ_COUNT 16
static uint8_t s_storage[CONFIG_ESP_BR_WEB_CLI_BUFFER_SIZE];
static cli_ring_t s_ring = CLI_RING_INITIALIZER(s_storage);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_output;
static bool s_ready;
static uint32_t s_session;

static void append_record(char source, const char *text)
{
    size_t length = strlen(text);
    portENTER_CRITICAL(&s_mux);
    cli_ring_append(&s_ring, source, text, length);
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
    return s_log_output ? s_log_output(format, args) : vprintf(format, args);
}

void esp_br_web_cli_init(void)
{
    /* Called once during application startup, before OpenThread starts. */
    if (!s_log_output) {
        s_session = esp_random();
        s_log_output = esp_log_set_vprintf(log_output);
    }
}

#if CONFIG_OPENTHREAD_CLI
static otCliOutputCallback s_cli_output;
static void *s_cli_context;
/* CLI extensions may also emit output from their own tasks. */
static bool s_busy;
void __real_otCliInit(otInstance *instance, otCliOutputCallback callback, void *context);
void __real_otCliInputLine(char *line);

static int cli_output(void *context, const char *format, va_list args)
{
    char marker[16];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(marker, sizeof(marker), format, copy);
    va_end(copy);
    if (!strcmp(marker, "> ") || !strcmp(marker, "Done") || !strcmp(marker, "Done\r\n") ||
        !strncmp(marker, "Error ", 6)) {
        portENTER_CRITICAL(&s_mux);
        s_busy = false;
        portEXIT_CRITICAL(&s_mux);
    }
    capture('C', format, args);
    /* Preserve IDF's callback: it also wakes the serial REPL on completion. */
    return s_cli_output(s_cli_context, format, args);
}

void __wrap_otCliInit(otInstance *instance, otCliOutputCallback callback, void *context)
{
    s_cli_output = callback;
    s_cli_context = context;
    __real_otCliInit(instance, cli_output, NULL);
    portENTER_CRITICAL(&s_mux);
    s_ready = true;
    portEXIT_CRITICAL(&s_mux);
}

void __wrap_otCliInputLine(char *line)
{
    /* Preserve OpenThread's emergency factoryreset while an async command runs. */
    bool factory_reset = !strncmp(line, "factoryreset", 12) && (line[12] == 0 || line[12] == ' ');
    portENTER_CRITICAL(&s_mux);
    bool rejected = s_busy && !factory_reset;
    if (!rejected) {
        s_busy = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (rejected) {
        append_record('C', "\r\n[Web CLI: busy; command was not executed. Retry after completion.]\r\n");
    } else {
        append_record('I', line);
        __real_otCliInputLine(line);
    }
}
#endif

/* ESP-IDF's HTTP server writes the status line, every extra header and the body
 * with separate send() calls and leaves Nagle enabled for normal responses (the
 * TCP_NODELAY path in httpd_txrx.c only covers error responses). Nagle then
 * withholds the final segment until the previous one is ACKed. That is free on
 * a wired link, but this server is reachable over Wi-Fi and the BR runs with
 * modem sleep, so the ACK of the first segment can be buffered by the AP for
 * hundreds of milliseconds or lost outright -- TCP then answers with a
 * multi-second retransmission and the browser sees the body only after the
 * stall. These responses are small and never worth coalescing, so send them
 * immediately. The option sticks to the socket and therefore also covers every
 * later request on the same keep-alive connection. */
static void cli_no_delay(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

esp_err_t esp_br_web_cli_get_handler(httpd_req_t *req)
{
    cli_no_delay(req);
    char query[64], value[24];
    uint64_t after = 0;
    if (httpd_req_get_url_query_len(req)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "after", value, sizeof(value)) != ESP_OK || !value[0]) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cursor");
        }
        for (char *p = value; *p; p++) {
            if (!isdigit((unsigned char)*p) || after > (UINT64_MAX - (*p - '0')) / 10) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cursor");
            }
            after = after * 10 + (*p - '0');
        }
    }
    /* Copy each record under a short critical section; serialize outside it. */
    cJSON *root = cJSON_CreateObject();
    cJSON *records = cJSON_AddArrayToObject(root, "records");
    if (!root || !records) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    portENTER_CRITICAL(&s_mux);
    uint64_t end = s_ring.next_seq;
    uint64_t first = s_ring.first_seq;
    cli_ring_reader_t reader = cli_ring_begin(&s_ring);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_mux);
    bool reset = after >= end;
    bool dropped = !reset && after != 0 && after < first - 1;
    uint64_t cursor = (reset || after < first - 1) ? first - 1 : after;
    for (int i = 0; i < READ_COUNT && cursor + 1 < end && reader.seq < end;) {
        cli_ring_record_t record;
        bool skip = reader.seq <= cursor;
        portENTER_CRITICAL(&s_mux);
        bool found = cli_ring_read(&s_ring, &reader, skip ? NULL : &record);
        portEXIT_CRITICAL(&s_mux);
        if (!found) {
            break; /* Producer overtook us; next poll reports the gap. */
        }
        if (skip) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddNumberToObject(item, "seq", (double)record.seq) ||
            !cJSON_AddStringToObject(item, "source",
                                     record.source == 'L'       ? "log"
                                         : record.source == 'I' ? "input"
                                                                : "cli") ||
            !cJSON_AddStringToObject(item, "text", record.text) || !cJSON_AddItemToArray(records, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        cursor = record.seq;
        i++;
    }
    cJSON_AddNumberToObject(root, "cursor", (double)cursor);
    cJSON_AddNumberToObject(root, "session", s_session);
    cJSON_AddBoolToObject(root, "reset", reset);
    cJSON_AddBoolToObject(root, "dropped", dropped);
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddBoolToObject(root, "more", cursor + 1 < end);
    return send_json(req, root);
}

/* Each subscriber owns one bounded worker, not the HTTP server task. A slow
 * socket cannot block POST or the other subscriber. No task is created by a
 * producer callback, and no producer waits for a subscriber. */
#define SSE_CLIENT_LIMIT 2
#define SSE_STACK_SIZE 4096
#define SSE_TICK_MS 50
#define SSE_HEARTBEAT_MS 10000
static unsigned s_stream_count;
static bool s_stream_stopping;

typedef struct {
    httpd_req_t *req;
    uint64_t cursor;
    bool reset;
} cli_stream_t;

/* Serialize socket shutdown with async completion, never with the producer's
 * critical section. This prevents fd reuse while server stop cancels sends. */
static SemaphoreHandle_t s_stream_mutex;
static cli_stream_t *s_streams[SSE_CLIENT_LIMIT];

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
        for (unsigned i = 0; i < SSE_CLIENT_LIMIT; i++) {
            if (s_streams[i]) shutdown(httpd_req_to_sockfd(s_streams[i]->req), SHUT_RDWR);
        }
        xSemaphoreGive(s_stream_mutex);
    }
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        unsigned active = s_stream_count;
        portEXIT_CRITICAL(&s_mux);
        if (!active) return;
        vTaskDelay(pdMS_TO_TICKS(SSE_TICK_MS));
    }
}

static void stream_release(void)
{
    portENTER_CRITICAL(&s_mux);
    s_stream_count--;
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
    esp_err_t err = httpd_resp_send_chunk(req, header, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, body, strlen(body));
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\n\n", 2);
    cJSON_free(body);
    return err;
}

static void stream_complete(cli_stream_t *stream)
{
    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    /* Shut down while this request still owns its session; HTTPD sees EOF
     * after complete and performs the final close. No queued close/fd race. */
    shutdown(httpd_req_to_sockfd(stream->req), SHUT_RDWR);
    esp_err_t err = httpd_req_async_handler_complete(stream->req);
    for (unsigned i = 0; i < SSE_CLIENT_LIMIT; i++) {
        if (s_streams[i] == stream) s_streams[i] = NULL;
    }
    xSemaphoreGive(s_stream_mutex);
    /* Even ESP_FAIL from complete means req was freed. Never retry it. */
    if (err != ESP_OK) ESP_LOGW("br_cli_sse", "Async completion wakeup failed: %d", err);
    free(stream);
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
    bool available = s_stream_mutex && !s_stream_stopping && s_stream_count < SSE_CLIENT_LIMIT;
    if (available) s_stream_count++;
    portEXIT_CRITICAL(&s_mux);
    if (!available) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "3");
        return httpd_resp_sendstr(req, "Web CLI stream limit reached or server stopping");
    }
    cli_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        stream_release();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    stream->cursor = cursor;
    stream->reset = has_id && session != s_session;
    cli_no_delay(req);
    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        httpd_req_async_handler_begin(req, &stream->req) != ESP_OK) {
        /* Do not leave a send timeout on a socket that stays keep-alive for
         * other endpoints when this request never became a stream. */
        struct timeval none = {0};
        setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
        free(stream);
        stream_release();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start stream");
    }
    xSemaphoreTake(s_stream_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < SSE_CLIENT_LIMIT; i++) {
        if (!s_streams[i]) { s_streams[i] = stream; break; }
    }
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
    char *start = line;
    while (*start == ' ') start++;
    if (!strncmp(start, "ot ", 3))
        start += 3;
    while (*start == ' ') start++;
    valid = *start != 0;
    for (char *p = start; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127)
            valid = false;
    }
    if (!valid) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter one non-empty OT command");
    }
#if CONFIG_OPENTHREAD_CLI
    portENTER_CRITICAL(&s_mux);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_mux);
    if (ready) {
        esp_err_t err = esp_openthread_cli_input(start);
        if (err == ESP_OK) {
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(req, "{\"accepted\":true}");
        }
    }
#endif
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "OpenThread CLI unavailable or task queue full");
}
