#ifndef PG3D_LOG_H
#define PG3D_LOG_H

#include <stdarg.h>

void log_init(const char *path);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
void log_flush(void);
__attribute__((noreturn)) void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(...) log_printf(__VA_ARGS__)
#ifdef PG3D_DEBUG
#define DLOG(...) log_printf(__VA_ARGS__)
#else
#define DLOG(...) do { } while (0)
#endif

#endif
