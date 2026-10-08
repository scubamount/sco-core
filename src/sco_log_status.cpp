#include "sco/log.h"
#include "sco/status.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sco {

static std::atomic<LogSink> g_sink{ nullptr };

void SetLogSink(LogSink sink) { g_sink.store(sink); }

static void Emit(const char* line) {
    if (LogSink sink = g_sink.load()) sink(line);
}

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Emit(buf);
}

static std::mutex g_statusLock;
static char       g_status[256] = "";
static bool       g_statusSet = false;

void Status(const char* fmt, ...) {
    char buf[sizeof(g_status)];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> hold(g_statusLock);
        snprintf(g_status, sizeof(g_status), "%s", buf);
        g_statusSet = true;
    }
    Log("[status] %s", buf);
}

bool GetStatus(char* out, size_t n) {
    if (!out || n == 0) return false;
    std::lock_guard<std::mutex> hold(g_statusLock);
    snprintf(out, n, "%s", g_status);
    return g_statusSet;
}

}  // namespace sco
