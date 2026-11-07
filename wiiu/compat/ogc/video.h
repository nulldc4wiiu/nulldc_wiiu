/* Wii U stand-in for libogc's <ogc/video.h>. No video output yet (renderer is a later phase). */
#pragma once
#include <stdint.h>
typedef void (*VIRetraceCallback)(uint32_t retraceCnt);
#ifdef __cplusplus
extern "C" {
#endif
void VIDEO_WaitVSync(void);
VIRetraceCallback VIDEO_SetPostRetraceCallback(VIRetraceCallback callback);
#ifdef __cplusplus
}
#endif
