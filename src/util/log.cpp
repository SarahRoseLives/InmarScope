#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr long kMaxLogBytes = 1 * 1024 * 1024; // 1 MB

std::mutex  g_logMtx;
std::FILE*  g_logFile = nullptr;

void ensureOpen()
{
    if (g_logFile)
        return;
    g_logFile = std::fopen("log.txt", "ab");
}

void rotateLog()
{
    if (!g_logFile) return;
    long sz = std::ftell(g_logFile);
    if (sz < kMaxLogBytes) return;
    std::fclose(g_logFile);
    g_logFile = nullptr;
#if defined(_WIN32)
    DeleteFileA("log.txt");
#else
    std::remove("log.txt");
#endif
    g_logFile = std::fopen("log.txt", "ab");
}

} // namespace

void logWrite(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    ensureOpen();
    if (!g_logFile)
        return;

    // Timestamp prefix: [HH:MM:SS]
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::fprintf(g_logFile, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(g_logFile, fmt, args);
    va_end(args);

    std::fputc('\n', g_logFile);
    std::fflush(g_logFile);

    rotateLog();
}
