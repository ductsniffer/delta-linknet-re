#include "Ports.h"

#include <cstdio>

namespace linknet {

void ILog::logf(const char *fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(buf);
}

}  // namespace linknet
