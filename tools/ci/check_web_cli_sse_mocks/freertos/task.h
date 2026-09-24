#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
TickType_t xTaskGetTickCount(void);
