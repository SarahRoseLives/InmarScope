// Bounded printf-style append into a fixed char buffer.
// The usual `pos += snprintf(buf + pos, sizeof(buf) - pos, ...)` idiom breaks
// once the output is truncated: snprintf returns the length it *would* have
// written, pos runs past the end, and `sizeof(buf) - pos` wraps to a huge
// size_t. With _FORTIFY_SOURCE (default on Ubuntu) glibc then aborts with
// "buffer overflow detected". appendf keeps pos within [0, cap - 1] and always
// leaves the buffer NUL-terminated, silently truncating instead.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
inline void appendf(char* buf, size_t cap, int& pos, const char* fmt, ...)
{
    if (cap == 0)
        return;
    if (pos < 0)
        pos = 0;
    if ((size_t)pos >= cap - 1)
    {
        pos = (int)(cap - 1);
        buf[pos] = 0;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf + pos, cap - (size_t)pos, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        buf[pos] = 0;
        return;
    }
    size_t end = (size_t)pos + (size_t)n;
    pos = (int)(end < cap - 1 ? end : cap - 1);
}
