/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

#define WEB_CLI_COMMAND_SIZE 256

bool esp_br_web_cli_capture_ready(void);
bool esp_br_web_cli_input_ready(void);
// Caller validates a nonempty, control-free single line. Copies bytes plus LF;
// no pointer is retained. ESP_ERR_TIMEOUT means unread input still occupies it.
esp_err_t esp_br_web_cli_submit(const char *line, size_t length);

void esp_br_web_cli_init(void);
esp_err_t esp_br_web_cli_events_handler(httpd_req_t *req);

void esp_br_web_cli_stream_start(void);
void esp_br_web_cli_stream_stop(void);
esp_err_t esp_br_web_cli_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
