#ifndef ERROR_H
#define ERROR_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum loglevel_t {
	LOG_LEVEL_DEBUG = 0,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_FATAL
};

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

typedef enum {
    LOG_TO_CONSOLE,
    LOG_TO_FILE
} log_mode_t;

void set_log_mode(log_mode_t mode);
void logging(enum loglevel_t loglevel, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
