/*
**  igmpproxy-plus - event log
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
*/
/**
*   eventlog.c - machine readable history of what the proxy did: one JSON
*   object per line (JSON Lines) for every join, leave, start/stop of
*   forwarding, timeout, query and upstream membership change. Meant to be
*   read by people, scripts and LLMs ("why did the stream stop at 21:04?").
*
*   The file is rotated when it reaches eventlogsize: the current file
*   becomes <path>.1 (replacing the previous one), so at most twice the
*   size is kept. Default location on OpenWrt is RAM (/tmp).
*
*   Line format, always these keys first, then event specific ones:
*     {"ts":"2026-11-09T21:04:05.123Z","ev":"leave","group":"239.1.1.1",
*      "if":"br-lan","host":"192.168.1.84", ...}
*/

#include "igmpproxy.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <stdarg.h>

static FILE *evFile = NULL;
static long evSize = 0;

static void evOpen(void) {
    struct Config *conf = getCommonConfig();
    struct stat st;

    evFile = fopen(conf->eventLog, "a");
    if (evFile == NULL) {
        my_log(LOG_WARNING, errno, "event log %s", conf->eventLog);
        return;
    }
    fcntl(fileno(evFile), F_SETFD, FD_CLOEXEC);
    evSize = fstat(fileno(evFile), &st) == 0 ? (long)st.st_size : 0;
}

void eventLogInit(void) {
    struct Config *conf = getCommonConfig();

    if (!conf->eventLog[0] || evFile)
        return;
    evOpen();
    if (evFile)
        my_log(LOG_INFO, 0, "Event log: %s (%u KiB, rotated once)",
               conf->eventLog, conf->eventLogSize / 1024);
}

void eventLogClose(void) {
    if (evFile) {
        fclose(evFile);
        evFile = NULL;
    }
}

bool eventLogReports(void) {
    return evFile != NULL && getCommonConfig()->eventLogReports;
}

static void evRotate(void) {
    struct Config *conf = getCommonConfig();
    char old[sizeof(conf->eventLog) + 2];

    snprintf(old, sizeof(old), "%s.1", conf->eventLog);
    fclose(evFile);
    evFile = NULL;
    if (rename(conf->eventLog, old) < 0)
        unlink(conf->eventLog);     // cannot keep the old one: start over
    evOpen();
}

static const char *ip(uint32_t a, char *buf) {
    struct in_addr in;
    in.s_addr = a;
    return inet_ntop(AF_INET, &in, buf, INET_ADDRSTRLEN) ? buf : "?";
}

/**
*   Writes one event. group/host 0 and ifp NULL leave those keys out.
*   'extra' is a printf format for further "key":value pairs (trusted,
*   from this program only), or NULL.
*/
void eventLog(const char *type, uint32_t group, const struct IfDesc *ifp,
              uint32_t host, const char *extra, ...) {
    struct Config *conf = getCommonConfig();
    struct timeval tv;
    struct tm tm;
    char ts[32], b[INET_ADDRSTRLEN];
    int n;

    if (evFile == NULL)
        return;

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);

    n = fprintf(evFile, "{\"ts\":\"%s.%03ldZ\",\"ev\":\"%s\"", ts, (long)(tv.tv_usec / 1000), type);
    if (group)
        n += fprintf(evFile, ",\"group\":\"%s\"", ip(group, b));
    if (ifp)
        n += fprintf(evFile, ",\"if\":\"%s\"", ifp->Name);
    if (host)
        n += fprintf(evFile, ",\"host\":\"%s\"", ip(host, b));
    if (extra) {
        va_list ap;
        n += fprintf(evFile, ",");
        va_start(ap, extra);
        n += vfprintf(evFile, extra, ap);
        va_end(ap);
    }
    n += fprintf(evFile, "}\n");
    fflush(evFile);

    if (n > 0)
        evSize += n;
    if (conf->eventLogSize && evSize >= (long)conf->eventLogSize)
        evRotate();
}

/**
*   Copies the rotated and the current file (oldest first) to fd.
*   Returns false when no event log is configured.
*/
bool eventLogDump(void (*out)(int, const char *, size_t), int fd) {
    struct Config *conf = getCommonConfig();
    char path[sizeof(conf->eventLog) + 2], buf[4096];
    int pass;

    if (!conf->eventLog[0])
        return false;
    if (evFile)
        fflush(evFile);
    for (pass = 0; pass < 2; pass++) {
        FILE *f;
        size_t n;
        snprintf(path, sizeof(path), pass ? "%s" : "%s.1", conf->eventLog);
        if ((f = fopen(path, "r")) == NULL)
            continue;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            out(fd, buf, n);
        fclose(f);
    }
    return true;
}
