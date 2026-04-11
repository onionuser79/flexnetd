/*
 * log.c — logging subsystem
 *
 * IMPORTANT: syslog.h MUST be included before flexnetd.h here so that
 * syslog's LOG_ERR / LOG_WARNING / LOG_INFO / LOG_DEBUG integer constants
 * are defined before flexnetd.h redefines LOG_ERR as our FLOG_ERR macro.
 * The switch() below uses the syslog integer constants, not our macros.
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <syslog.h>     /* must come BEFORE flexnetd.h */
#include "flexnetd.h"

int g_log_level  = LOG_LEVEL_INFO;
int g_use_syslog = 0;

void flexnetd_log(int level, const char *tag, const char *fmt, ...)
{
    if (level > g_log_level) return;

    va_list ap;
    va_start(ap, fmt);

    if (g_use_syslog) {
        /*
         * Map our levels to syslog priorities.
         * Use the raw integer values to avoid the LOG_ERR macro clash.
         * LOG_ERR=3, LOG_WARNING=4, LOG_INFO=6, LOG_DEBUG=7 (POSIX)
         */
        int priority;
        switch (level) {
            case LOG_LEVEL_ERROR: priority = 3; break;  /* LOG_ERR     */
            case LOG_LEVEL_WARN:  priority = 4; break;  /* LOG_WARNING */
            case LOG_LEVEL_INFO:  priority = 6; break;  /* LOG_INFO    */
            default:              priority = 7; break;  /* LOG_DEBUG   */
        }
        vsyslog(priority, fmt, ap);
    } else {
        /* print to stderr with ISO timestamp and level tag */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        gmtime_r(&ts.tv_sec, &tm);

        fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                ts.tv_nsec / 1000000L,
                tag);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    va_end(ap);
}
