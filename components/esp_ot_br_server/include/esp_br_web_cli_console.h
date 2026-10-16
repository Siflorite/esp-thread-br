/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Reserve one eventfd in the application's shared eventfd budget.
// Call once after esp_br_web_start(). Creates the normal SDK REPL task, with
// serial/Web input and fixed 80 columns. SDK full/basic editing is retained.
// Firmware-lifetime console: runtime stop/delete is not supported.
void esp_br_web_cli_console_start(void);

#ifdef __cplusplus
}
#endif
