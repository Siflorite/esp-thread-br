/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_br_web_cli.h"
#include "esp_br_web_cli_console.h"
#include "esp_console.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"

static char s_input[WEB_CLI_COMMAND_SIZE];
static size_t s_length, s_position;
static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_input_fd = -1;
static atomic_bool s_ready = ATOMIC_VAR_INIT(false);

bool esp_br_web_cli_input_ready(void)
{
    return atomic_load(&s_ready);
}

esp_err_t esp_br_web_cli_submit(const char *line, size_t length)
{
    if (!line || !length || length >= sizeof(s_input)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_br_web_cli_input_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_input_mux);
    if (s_length) {
        portEXIT_CRITICAL(&s_input_mux);
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_input, line, length);
    s_input[length] = '\n';
    s_length = length + 1;
    s_position = 0;
    portEXIT_CRITICAL(&s_input_mux);
    // This firmware-lifetime fd has one reader. Publication precedes wakeup.
    uint64_t signal = 1;
    ESP_ERROR_CHECK(write(s_input_fd, &signal, sizeof(signal)) == sizeof(signal) ? ESP_OK : ESP_FAIL);
    return ESP_OK;
}

static ssize_t console_read(int fd, void *buffer, size_t size)
{
    if (!size) {
        return 0;
    }
    for (;;) {
        portENTER_CRITICAL(&s_input_mux);
        size_t count = s_length - s_position;
        if (count > size) {
            count = size;
        }
        memcpy(buffer, s_input + s_position, count);
        s_position += count;
        if (s_position == s_length) {
            s_length = s_position = 0;
        }
        portEXIT_CRITICAL(&s_input_mux);
        if (count) {
            return count;
        }

        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        FD_SET(s_input_fd, &reads);
        int max_fd = fd > s_input_fd ? fd : s_input_fd;
        int result = select(max_fd + 1, &reads, NULL, NULL, NULL);
        if (result > 0 && FD_ISSET(s_input_fd, &reads)) {
            uint64_t signal;
            if (read(s_input_fd, &signal, sizeof(signal)) < 0) {
                vTaskDelay(1);
            }
            continue;
        }
        if (result > 0 && FD_ISSET(fd, &reads)) {
            ssize_t received = read(fd, buffer, size);
            if (received > 0) {
                return received;
            }
        }
        // Idle/errors must not return 0/-1: linenoise may submit a partial line.
        vTaskDelay(1);
    }
}

void esp_br_web_cli_console_start(void)
{
    static esp_console_repl_t *repl;
    ESP_ERROR_CHECK(repl ? ESP_ERR_INVALID_STATE : ESP_OK);
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = CONFIG_IDF_TARGET ">";
    config.max_cmdline_length = WEB_CLI_COMMAND_SIZE;
    config.max_history_len = 10;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t device = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&device, &config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t device = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&device, &config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device, &config, &repl));
#else
#error Unsupported console type
#endif
    bool capture_ready = esp_br_web_cli_capture_ready();
    if (capture_ready) {
        // app_main reserves this eventfd before registering the shared VFS.
        s_input_fd = eventfd(0, 0);
        ESP_ERROR_CHECK(s_input_fd < 0 ? ESP_ERR_NO_MEM : ESP_OK);
        int fd = fileno(stdin);
        int flags = fcntl(fd, F_GETFL);
        ESP_ERROR_CHECK(flags < 0 ? ESP_FAIL : ESP_OK);
        ESP_ERROR_CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? ESP_FAIL : ESP_OK);
        // The constructor's terminal probe resets this hook; install it AFTER.
        linenoiseSetReadFunction(console_read);
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    atomic_store(&s_ready, capture_ready);
}
