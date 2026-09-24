#pragma once
#define ESP_LOGW(...) ((void)0)
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t);
