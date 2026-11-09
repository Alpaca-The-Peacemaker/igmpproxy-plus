/*
**  igmpproxy-plus - status socket
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
*/
/**
*   control.c - UNIX stream socket for the status pages and scripts.
*   A client may send one command line, then the daemon answers and
*   closes the connection:
*     "status"  (or nothing)  the live group table as JSON
*     "events"                the event log (JSON Lines), oldest first
*     "query"                 send a general query on every downstream
*                             interface now, so all members report again
*   "igmpproxy -S|-E|-Q [path]" is the matching client.
*/

#include "igmpproxy.h"

#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>

static int ctlFd = -1;
static char ctlPath[sizeof(((struct sockaddr_un *)0)->sun_path)];

void controlInit(void) {
    struct Config *conf = getCommonConfig();
    struct sockaddr_un sa;

    if (!conf->controlSocket[0])
        return;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(conf->controlSocket) >= sizeof(sa.sun_path)) {
        my_log(LOG_WARNING, 0, "controlsocket path too long, status socket disabled");
        return;
    }
    strcpy(sa.sun_path, conf->controlSocket);
    strcpy(ctlPath, conf->controlSocket);

    ctlFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctlFd < 0) {
        my_log(LOG_WARNING, errno, "status socket");
        return;
    }
    unlink(ctlPath);
    if (bind(ctlFd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ctlFd, 4) < 0) {
        my_log(LOG_WARNING, errno, "bind status socket %s", ctlPath);
        close(ctlFd);
        ctlFd = -1;
        return;
    }
    chmod(ctlPath, 0600);
    fcntl(ctlFd, F_SETFL, fcntl(ctlFd, F_GETFL) | O_NONBLOCK);
    fcntl(ctlFd, F_SETFD, FD_CLOEXEC);
    my_log(LOG_INFO, 0, "Status socket listening on %s", ctlPath);
}

int controlFd(void) {
    return ctlFd;
}

static void writeAll(int fd, const char *buf, size_t len);

static void writeStr(int fd, const char *str) {
    writeAll(fd, str, strlen(str));
}

static void writeAll(int fd, const char *buf, size_t len) {
    while (len > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
#else
        ssize_t n = write(fd, buf, len);
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

void controlAccept(void) {
    int fd;
    char *buf = NULL, cmd[32] = "";
    size_t len = 0;
    FILE *f;
    struct pollfd pfd;

    fd = accept(ctlFd, NULL, NULL);
    if (fd < 0)
        return;

    // Optional command line. Clients send it right after connecting; one
    // that sends nothing (older clients) gets the status after 100 ms.
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 100) > 0) {
        ssize_t n = recv(fd, cmd, sizeof(cmd) - 1, 0);
        cmd[n > 0 ? n : 0] = '\0';
        cmd[strcspn(cmd, " \r\n")] = '\0';
    }

    // Blocking writes with a short timeout so a stuck client cannot stall
    // the proxy for long.
    {
        struct timeval tv = { 1, 0 };
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (!strcmp(cmd, "events")) {
        if (!eventLogDump(writeAll, fd))
            writeStr(fd, "{\"error\":\"event log is off (eventlog option)\"}\n");
    } else if (!strcmp(cmd, "query")) {
        sendGeneralQueriesNow("requested");
        writeStr(fd, "{\"ok\":true,\"sent\":\"general query\"}\n");
    } else if (!cmd[0] || !strcmp(cmd, "status")) {
        f = open_memstream(&buf, &len);
        if (f) {
            routeTableDumpJson(f, now_ms());
            fclose(f);
            writeAll(fd, buf, len);
            free(buf);
        }
    } else {
        writeStr(fd, "{\"error\":\"unknown command\"}\n");
    }
    close(fd);
}

void controlCleanUp(void) {
    if (ctlFd >= 0) {
        close(ctlFd);
        unlink(ctlPath);
        ctlFd = -1;
    }
}

/**
*   Client mode: send 'cmd' and print the answer to stdout. Returns exit
*   code (3 when the daemon answered with an error object).
*/
int controlClient(const char *path, const char *cmd) {
    struct sockaddr_un sa;
    char buf[4096];
    ssize_t n;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        return 2;
    }
    strcpy(sa.sun_path, path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "igmpproxy: cannot connect to %s: %s\n", path, strerror(errno));
        return 1;
    }
    writeAll(fd, cmd, strlen(cmd));
    writeAll(fd, "\n", 1);
    shutdown(fd, SHUT_WR);
    {
        bool first = true, err = false;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (first && n > 9 && !strncmp(buf, "{\"error\":", 9))
                err = true;
            first = false;
            fwrite(buf, 1, (size_t)n, stdout);
        }
        close(fd);
        return err ? 3 : 0;
    }
}
