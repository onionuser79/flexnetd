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

int   g_log_level  = LOG_LEVEL_INFO;
int   g_use_syslog = 0;
FILE *g_log_file   = NULL;
int   g_port_idx   = -1;   /* M6.6: set by parent before CE fork */

/*
 * write_timestamped — write a timestamped log line to a FILE stream.
 * Used for both stderr and the optional log file.
 */
static void write_timestamped(FILE *fp, const char *tag,
                               const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000L,
            tag);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);
}

void flexnetd_log(int level, const char *tag, const char *fmt, ...)
{
    if (level > g_log_level) return;

    if (g_use_syslog) {
        va_list ap;
        va_start(ap, fmt);
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
        va_end(ap);
    } else {
        /* print to stderr with ISO timestamp and level tag */
        va_list ap;
        va_start(ap, fmt);
        write_timestamped(stderr, tag, fmt, ap);
        va_end(ap);
    }

    /* If a log file is open, ALWAYS write there too (console + file) */
    if (g_log_file) {
        va_list ap2;
        va_start(ap2, fmt);
        write_timestamped(g_log_file, tag, fmt, ap2);
        va_end(ap2);
    }
}
