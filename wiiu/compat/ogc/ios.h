/* Wii U stand-in for libogc's IOS calls: there is no IOS to query or reload from a Wii U app. */
#pragma once
#include <stdint.h>
static inline int32_t IOS_GetVersion(void) { return -1; }
static inline int32_t IOS_ReloadIOS(int32_t version) { return -1; }
