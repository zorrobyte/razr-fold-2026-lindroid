#pragma once
#include <android/binder_ibinder.h>
#ifdef __cplusplus
extern "C" {
#endif
AIBinder* AServiceManager_getService(const char* instance) __INTRODUCED_IN(29);
AIBinder* AServiceManager_waitForService(const char* instance) __INTRODUCED_IN(31);
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance) __INTRODUCED_IN(29);
#ifdef __cplusplus
}
#endif
