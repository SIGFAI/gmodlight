#pragma once
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <windows.h>

namespace gml::log {

inline FILE*& File() { static FILE* f = nullptr; return f; }
inline std::mutex& Lock() { static std::mutex m; return m; }

inline void Open(const char* path) {
    File() = _fsopen(path, "w", _SH_DENYNO);  // readable while the game runs
}

inline void Write(const char* level, const char* fmt, ...) {
    std::lock_guard<std::mutex> g(Lock());
    if (!File()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(File(), "[%02d:%02d:%02d.%03d] [%s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level);
    va_list a;
    va_start(a, fmt);
    vfprintf(File(), fmt, a);
    va_end(a);
    fputc('\n', File());
    fflush(File());
}

}  // namespace gml::log

#define LOGI(...) ::gml::log::Write("info", __VA_ARGS__)
#define LOGW(...) ::gml::log::Write("warning", __VA_ARGS__)
#define LOGE(...) ::gml::log::Write("error", __VA_ARGS__)
