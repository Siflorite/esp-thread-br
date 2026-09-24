/* Actual SSE handler/worker host tests. Run tools/ci/check_web_cli_sse.sh.
 * Mock scheduling is deterministic; this does not model concurrent tasks/TCP. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/esp_ot_br_server/src/esp_br_web_cli.c"

static char output[131072], status_code[64];
static size_t output_len;
static int fail_begin, fail_task, fail_complete, fail_socket, fail_chunk;
static unsigned begins, completes, shutdowns, deletes, delays, stop_after;
static TickType_t ticks;
static void (*pending[2])(void *);
static void *pending_arg[2];
static unsigned pending_count;
static int http_error;
static bool change_ready;
static bool peer_closed, drain_on_delay;
static unsigned last_complete_shutdowns;
ssize_t mock_recv(int fd, void *buf, size_t len, int flags)
{ (void)buf; assert(fd == 123 && len == 1 && flags == (MSG_PEEK | MSG_DONTWAIT)); errno = EAGAIN; return peer_closed ? 0 : -1; }

int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return 123; }
int mock_setsockopt(int fd, int level, int opt, const void *p, socklen_t len)
{ (void)fd; (void)level; (void)p; (void)len; return opt == SO_SNDTIMEO && fail_socket ? -1 : 0; }
int mock_shutdown(int fd, int how)
{ assert(fd == 123 && how == SHUT_RDWR); shutdowns++; return 0; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *s)
{ (void)r; (void)s; http_error = code; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v)
{ (void)r; (void)k; (void)v; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s)
{ (void)r; snprintf(status_code, sizeof(status_code), "%s", s); return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *s, ssize_t len)
{
    (void)r;
    if (fail_chunk) return ESP_FAIL;
    if (len == HTTPD_RESP_USE_STRLEN) len = strlen(s);
    assert(len > 0 && output_len + (size_t)len < sizeof(output));
    memcpy(output + output_len, s, len); output_len += len; output[output_len] = 0;
    return ESP_OK;
}
static esp_err_t copy_value(const char *s, char *out, size_t n)
{ if (!s || strlen(s) >= n) return ESP_FAIL; strcpy(out, s); return ESP_OK; }
size_t httpd_req_get_url_query_len(httpd_req_t *r) { return r->query ? strlen(r->query) : 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *s, size_t n) { return copy_value(r->query, s, n); }
esp_err_t httpd_query_key_value(const char *q, const char *key, char *out, size_t n)
{
    if (strcmp(key, "after") || strncmp(q, "after=", 6)) return ESP_FAIL;
    const char *end = strchr(q + 6, '&'); size_t len = end ? (size_t)(end - q - 6) : strlen(q + 6);
    if (len >= n) return ESP_FAIL;
    memcpy(out, q + 6, len); out[len] = 0; return ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *k)
{ assert(!strcmp(k, "Last-Event-ID")); return r->header ? strlen(r->header) : 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *k, char *out, size_t n)
{ (void)k; return copy_value(r->header, out, n); }
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out)
{ begins++; if (fail_begin) return ESP_FAIL; *out = malloc(sizeof(**out)); assert(*out); **out = *r; return ESP_OK; }
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r)
{ assert(shutdowns > last_complete_shutdowns); last_complete_shutdowns = shutdowns; completes++; free(r); return fail_complete ? ESP_FAIL : ESP_OK; }
int httpd_req_recv(httpd_req_t *r, char *b, size_t n) { (void)r; (void)b; (void)n; return -1; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    if (fail_task) return 0;
    assert(pending_count < 2); pending[pending_count] = fn; pending_arg[pending_count++] = arg; return pdPASS;
}
void vTaskDelay(TickType_t time)
{
    ticks += time; delays++; if (change_ready) s_ready = true;
    if (delays >= stop_after) s_stream_stopping = true;
    if (drain_on_delay) {
        drain_on_delay = false;
        for (unsigned i = 0; i < pending_count; i++) pending[i](pending_arg[i]);
    }
}
void vTaskDelete(void *p) { assert(!p); deletes++; }
TickType_t xTaskGetTickCount(void) { return ticks; }
uint32_t esp_random(void) { return 42; }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn) { (void)fn; return vprintf; }

static void reset(void)
{
    assert(s_stream_count == 0);
    s_ring = (cli_ring_t)CLI_RING_INITIALIZER(s_storage); s_session = 42; s_ready = false;
    s_stream_stopping = false; output_len = 0; output[0] = status_code[0] = 0;
    fail_begin = fail_task = fail_complete = fail_socket = fail_chunk = 0;
    begins = completes = shutdowns = deletes = delays = ticks = pending_count = 0;
    stop_after = 1; http_error = 0; change_ready = false; peer_closed = drain_on_delay = false;
    last_complete_shutdowns = 0;
    for (unsigned i = 0; i < SSE_CLIENT_LIMIT; i++) assert(!s_streams[i]);
    esp_br_web_cli_stream_start();
}
static void run_workers(void)
{
    for (unsigned i = 0; i < pending_count; i++) pending[i](pending_arg[i]);
    assert(s_stream_count == 0 && completes == pending_count && shutdowns == completes && deletes == completes);
}
static void fill(unsigned count)
{ for (unsigned i = 0; i < count; i++) append_record(i % 3 == 0 ? 'I' : i % 3 == 1 ? 'L' : 'C', "hello\n"); }
static unsigned events(const char *name)
{
    char needle[64]; snprintf(needle, sizeof(needle), "event: %s\n", name);
    unsigned n = 0; for (const char *p = output; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static unsigned check_batches(uint64_t expected_first, uint64_t expected_last)
{
    unsigned total = 0;
    for (const char *p = output; (p = strstr(p, "event: records\n"));) {
        p += strlen("event: records\n");
        unsigned boot; uint64_t id;
        assert(sscanf(p, "id: %u:%" SCNu64, &boot, &id) == 2 && boot == 42);
        const char *data = strstr(p, "data: "); assert(data);
        cJSON *json = cJSON_Parse(data + 6); assert(json);
        cJSON *records = cJSON_GetObjectItemCaseSensitive(json, "records");
        int n = cJSON_GetArraySize(records); assert(n > 0 && n <= 16);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(records, i);
            cJSON *seq = cJSON_GetObjectItemCaseSensitive(item, "seq"); assert(cJSON_IsString(seq));
            assert(strtoull(seq->valuestring, NULL, 10) == expected_first + total++);
        }
        cJSON *cursor = cJSON_GetObjectItemCaseSensitive(json, "cursor"); assert(cJSON_IsString(cursor));
        assert(strtoull(cursor->valuestring, NULL, 10) == id && id == expected_first + total - 1);
        cJSON_Delete(json);
    }
    assert(total == expected_last - expected_first + 1); return total;
}
static void parsing(void)
{
    uint32_t boot; uint64_t cursor;
    assert(stream_parse_id("4294967295:18446744073709551615", false, &boot, &cursor));
    assert(boot == UINT32_MAX && cursor == UINT64_MAX);
    assert(stream_parse_id("42%3A9007199254740993", true, &boot, &cursor));
    assert(cursor == UINT64_C(9007199254740993));
    assert(stream_parse_id("42%3a1", true, &boot, &cursor));
    const char *bad[] = {"", ":1", "1:", "-1:0", "+1:0", "1: 2", "1:2x", "1:2:3",
        "4294967296:0", "1:18446744073709551616", "1%3", "1%3G0", "1%253A0", "1:0\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) assert(!stream_parse_id(bad[i], true, &boot, &cursor));
    assert(!stream_parse_id("42%3A1", false, &boot, &cursor));
}
static void handlers(void)
{
    reset(); httpd_req_t r = {.header = "42:2", .query = "after=invalid"};
    fill(5); assert(esp_br_web_cli_events_handler(&r) == ESP_OK);
    assert(((cli_stream_t *)pending_arg[0])->cursor == 2); run_workers(); check_batches(3, 5);
    reset(); r = (httpd_req_t){.header = "bad", .query = "after=42%3A2"};
    esp_br_web_cli_events_handler(&r); assert(http_error == 400 && !begins && !s_stream_count);
    reset(); r = (httpd_req_t){.query = "after=42%3A2"}; fill(5);
    esp_br_web_cli_events_handler(&r); run_workers(); check_batches(3, 5);
    reset(); r = (httpd_req_t){.content_len = 1}; esp_br_web_cli_events_handler(&r); assert(http_error == 400 && !begins);
    for (unsigned mode = 0; mode < 3; mode++) {
        reset(); r = (httpd_req_t){0}; fail_begin = mode == 0; fail_socket = mode == 1; fail_task = mode == 2;
        esp_br_web_cli_events_handler(&r);
        assert(http_error == 500 && !s_stream_count && !pending_count);
        assert(completes == (mode == 2) && shutdowns == completes);
    }
    reset(); r = (httpd_req_t){0};
    esp_br_web_cli_events_handler(&r); esp_br_web_cli_events_handler(&r); esp_br_web_cli_events_handler(&r);
    assert(s_stream_count == 2 && begins == 2 && !strncmp(status_code, "503", 3)); run_workers();
    reset(); esp_br_web_cli_stream_stop(); esp_br_web_cli_events_handler(&r);
    assert(!begins && !strncmp(status_code, "503", 3)); esp_br_web_cli_stream_start();
    esp_br_web_cli_events_handler(&r); run_workers();
}
static void workers(void)
{
    httpd_req_t r = {0};
    reset(); fill(35); stop_after = 3; esp_br_web_cli_events_handler(&r); run_workers();
    assert(events("status") == 1 && events("records") == 3 && !events("gap") && !events("reset")); check_batches(1, 35);
    reset(); fill(400); uint64_t first = s_ring.first_seq; assert(first > 1); stop_after = 30;
    esp_br_web_cli_events_handler(&r); run_workers(); assert(!events("gap")); check_batches(first, 400);
    reset(); fill(400); first = s_ring.first_seq; stop_after = 30; r.header = "42:1";
    esp_br_web_cli_events_handler(&r); run_workers(); assert(events("gap") == 1); check_batches(first, 400);
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(); fill(3); r.header = mode ? "42:999" : "41:2";
        esp_br_web_cli_events_handler(&r); run_workers(); assert(events("reset") == 1); check_batches(1, 3);
    }
    reset(); r.header = "42:90"; fill(100); stop_after = 3;
    esp_br_web_cli_events_handler(&r); run_workers(); check_batches(91, 100);
    reset(); r.header = NULL; change_ready = true; stop_after = 2;
    esp_br_web_cli_events_handler(&r); run_workers(); assert(events("status") == 2);
    reset(); fail_complete = 1; esp_br_web_cli_events_handler(&r); run_workers(); assert(completes == 1);
    reset(); fail_chunk = 1; esp_br_web_cli_events_handler(&r); run_workers(); assert(completes == 1 && delays == 0);
    reset(); fail_task = fail_complete = 1; esp_br_web_cli_events_handler(&r);
    assert(completes == 1 && shutdowns == 1 && !s_stream_count);
    reset(); peer_closed = true; esp_br_web_cli_events_handler(&r); run_workers();
    assert(!delays && !events("status"));
    reset(); stop_after = 202; esp_br_web_cli_events_handler(&r); run_workers();
    assert(strstr(output, ": heartbeat\n\n"));
    reset(); esp_br_web_cli_events_handler(&r); esp_br_web_cli_events_handler(&r);
    drain_on_delay = true; fail_complete = 1; esp_br_web_cli_stream_stop();
    assert(completes == 2 && shutdowns == 4 && !s_stream_count);
    assert(!s_streams[0] && !s_streams[1]);
}
int main(void)
{
    parsing(); handlers(); workers();
    puts("Web CLI SSE C checks passed: strict IDs, header precedence, resume/reset/gap, bounded batches, async/task/socket failures, limit, shutdown-before-complete (including failure).");
    return 0;
}
