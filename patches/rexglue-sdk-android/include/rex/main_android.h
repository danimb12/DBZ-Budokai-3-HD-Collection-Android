#pragma once
/**
 * @file        rex/main_android.h
 * @brief       Android process-level init for the Xenia-derived POSIX layers.
 *
 * Must run once before any guest memory or thread is created: it resolves
 * ASharedMemory_create (libandroid) and pthread_getname_np (libc) at runtime.
 */
#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
namespace rex::memory {
void AndroidInitialize();
void AndroidShutdown();
}  // namespace rex::memory
namespace rex::thread {
void AndroidInitialize();
void AndroidShutdown();
}  // namespace rex::thread
#endif
