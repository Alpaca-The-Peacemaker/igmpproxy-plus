/*
**  igmpproxy-plus - status socket
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
*/
/**
*   control.c - UNIX stream socket that returns the live group table as
*   JSON. The daemon writes one JSON document per connection and closes
*   it. "igmpproxy -S [path]" is the matching client, used by the
*   OPNsense and LuCI status pages.
*/

#include "igmpproxy.h"

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

static void writeAll(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
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
    char *buf = NULL;
    size_t len = 0;
    FILE *f;

    fd = accept(ctlFd, NULL, NULL);
    if (fd < 0)
        return;

    // Blocking writes with a short timeout so a stuck client cannot stall
    // the proxy for long.
    {
        struct timeval tv = { 1, 0 };
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0)
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    f = open_memstream(&buf, &len);
    if (f) {
        routeTableDumpJson(f, now_ms());
        fclose(f);
        writeAll(fd, buf, len);
        free(buf);
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
*   Client mode: print the status JSON to stdout. Returns exit code.
*/
int controlClient(const char *path) {
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
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}
