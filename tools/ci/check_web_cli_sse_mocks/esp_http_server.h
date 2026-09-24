/* Host-only mocks for check_web_cli_sse.c. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_408_REQ_TIMEOUT 408
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN -1
typedef struct { size_t content_len; const char *header; const char *query; } httpd_req_t;
int httpd_req_to_sockfd(httpd_req_t *req);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
size_t httpd_req_get_url_query_len(httpd_req_t *);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
esp_err_t httpd_query_key_value(const char *, const char *, char *, size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *, httpd_req_t **);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
