#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define tskIDLE_PRIORITY 0
