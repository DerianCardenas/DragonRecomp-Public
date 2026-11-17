#pragma once

/*
 * Cross-platform dynamic library loading shim.
 * Uses LoadLibraryA/GetProcAddress on Windows, dlopen/dlsym on POSIX.
 */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define RTLD_NOW 0
#define RTLD_LOCAL 0

inline void* dlopen(const char* path, int) {
    return static_cast<void*>(LoadLibraryA(path));
}

inline void* dlsym(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

inline int dlclose(void* handle) {
    return FreeLibrary(static_cast<HMODULE>(handle)) ? 0 : -1;
}

inline const char* dlerror() {
    return "Windows dynamic library error";
}
#else
#include <dlfcn.h>
#endif
