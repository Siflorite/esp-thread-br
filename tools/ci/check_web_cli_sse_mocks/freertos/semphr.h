#pragma once
#include <assert.h>
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) { (void)timeout; assert(mutex); return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex); return 1; }
