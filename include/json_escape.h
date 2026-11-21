#pragma once
// ── Shared bounded JSON string escaper ────────────────────────────────────
// Backslash-prefixes `"` and `\\` (the only two characters JSON requires) and
// copies everything else verbatim. Output is always NUL-terminated; truncates
// silently if `cap` runs out, so the caller must size `dst` for at most
// 2× source + 1 NUL.
//
// Used by /api/status, /api/fsd-mock/* and the diagnostic upload payload so
// every firmware-generated JSON string escapes uniformly.

#include <cstddef>

static inline void jsonEscapeInto(const char* src, char* dst, size_t cap) {
    if (!dst || cap == 0) return;
    char* out = dst;
    char* end = dst + cap - 3;  // room for one final escape byte + NUL
    while (src && *src && out < end) {
        if (*src == '"' || *src == '\\') *out++ = '\\';
        *out++ = *src++;
    }
    *out = 0;
}
