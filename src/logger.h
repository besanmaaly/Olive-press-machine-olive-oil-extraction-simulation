#ifndef OLIVE_LOGGER_H
#define OLIVE_LOGGER_H

int logger_open(const char *path);
void logger_log(const char *level, const char *fmt, ...);
void logger_close(void);

#endif
