// SPDX-License-Identifier: CC0-1.0
// Windows build: an ANativeWindow is the game window's HWND (EGLNativeWindowType for ANGLE).
#pragma once
#include <stdint.h>
typedef struct HWND__ ANativeWindow;
#ifdef __cplusplus
extern "C" {
#endif
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
#ifdef __cplusplus
}
#endif
