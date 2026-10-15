/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_server.h"

void esp_br_web_cli_init(void);
esp_err_t esp_br_web_cli_events_handler(httpd_req_t *req);

void esp_br_web_cli_stream_start(void);
void esp_br_web_cli_stream_stop(void);
esp_err_t esp_br_web_cli_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
