#define _POSIX_C_SOURCE 200809L
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *log_fp = NULL;
static int log_is_stdout = 0;

int logger_open(const char *path)
{
    if (!path || !*path)
        return -1;
    log_is_stdout = strcmp(path, "-") == 0;
    log_fp = log_is_stdout ? stdout : fopen(path, "a");
    if (!log_fp)
        return -1;
    setvbuf(log_fp, NULL, _IOLBF, 0); /* Flush each line. */
    return 0;
}

void logger_log(const char *level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm local;
    char date[32], message[512], line[640];
    va_list args;

    if (!log_fp)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &local);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &local);

    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    snprintf(line, sizeof line, "[%s.%03ld] [%s] %s\n",
             date, ts.tv_nsec / 1000000, level, message);
    fputs(line, log_fp);
    if (!log_is_stdout)
        fputs(line, stdout);
}

void logger_close(void)
{
    if (log_fp && !log_is_stdout)
        fclose(log_fp);
    log_fp = NULL;
    log_is_stdout = 0;
}
