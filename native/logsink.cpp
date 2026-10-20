#include "logsink.h"

#include <cstdarg>
#include <cstdio>

namespace logsink {

namespace {
Fn g_fn = nullptr;
}

void Set(Fn fn) { g_fn = fn; }

bool ready() { return g_fn != nullptr; }

void Write(const char* fmt, ...) {
    if (!g_fn || !fmt) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // 再过一层：调用方给的 fmt 里可能带 %s，拼好再交给写文件的那个
    g_fn("%s", buf);
}

}  // namespace logsink
