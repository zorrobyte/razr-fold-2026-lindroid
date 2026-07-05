#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ABinderProcess_startThreadPool(void);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
#ifdef __cplusplus
}
#endif
