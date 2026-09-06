/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_br_web_cli.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_OPENTHREAD_CLI
#include "esp_openthread_cli.h"
#include "openthread/cli.h"
#endif

/* Fixed RAM usage; callbacks never allocate, log, or perform network I/O. */
#define RECORD_COUNT CONFIG_ESP_BR_WEB_CLI_RECORD_COUNT
#define RECORD_SIZE 256
#define COMMAND_SIZE 256
#define READ_COUNT 16
typedef struct {
    uint64_t seq;
    char source;
    char text[RECORD_SIZE];
} cli_record_t;
static cli_record_t s_records[RECORD_COUNT];
static uint64_t s_next = 1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_output;
static bool s_ready;
static uint32_t s_session;

static void append_record(char source, const char *text)
{
    portENTER_CRITICAL(&s_mux);
    cli_record_t *record = &s_records[(s_next - 1) % RECORD_COUNT];
    record->seq = s_next++;
    record->source = source;
    strlcpy(record->text, text, sizeof(record->text));
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
    uint64_t end = s_next;
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_mux);
    uint64_t first = end > RECORD_COUNT ? end - RECORD_COUNT : 1;
    bool reset = after >= end;
    bool dropped = !reset && after != 0 && after < first - 1;
    uint64_t cursor = (reset || after < first - 1) ? first - 1 : after;
    for (int i = 0; i < READ_COUNT && cursor + 1 < end; i++) {
        cli_record_t record;
        portENTER_CRITICAL(&s_mux);
        record = s_records[cursor % RECORD_COUNT];
        portEXIT_CRITICAL(&s_mux);
        if (record.seq != cursor + 1) {
            break; /* Producer overtook us; next poll reports the gap. */
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
    }
    cJSON_AddNumberToObject(root, "cursor", (double)cursor);
    cJSON_AddNumberToObject(root, "session", s_session);
    cJSON_AddBoolToObject(root, "reset", reset);
    cJSON_AddBoolToObject(root, "dropped", dropped);
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddBoolToObject(root, "more", cursor + 1 < end);
    return send_json(req, root);
}

esp_err_t esp_br_web_cli_post_handler(httpd_req_t *req)
{
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
