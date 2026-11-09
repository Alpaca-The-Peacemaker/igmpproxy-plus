/*
**  igmpproxy - IGMP proxy based multicast router
**  Copyright (C) 2005 Johnny Egeland <johnny@rlo.org>
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**
**----------------------------------------------------------------------------
**
**  This software is derived work from the following software. The original
**  source code has been modified from it's original state by the author
**  of igmpproxy.
**
**  smcroute 0.92 - Copyright (C) 2001 Carsten Schill <carsten@cschill.de>
**  - Licensed under the GNU General Public License, either version 2 or
**    any later version.
**
**  mrouted 3.9-beta3 - Copyright (C) 2002 by The Board of Trustees of
**  Leland Stanford Junior University.
**  - Licensed under the 3-clause BSD license, see Stanford.txt file.
**
*/
/**
*   igmpproxy.c - The main file for the IGMP proxy application.
*
*   February 2005 - Johnny Egeland
*/

/* getopt() and clock_getime() */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "igmpproxy.h"

static const char Usage[] =
"Usage: igmpproxy [-h] [-n] [-d] [-v [-v]] <configfile>\n"
"       igmpproxy -t <configfile>\n"
"       igmpproxy -S|-E|-Q [statussocket]\n"
"\n"
"   -h   Display this help screen\n"
"   -n   Do not run as a daemon\n"
"   -d   Run in debug mode. Output all messages on stderr. Implies -n.\n"
"   -v   Be verbose. Give twice to see even debug messages.\n"
"   -t   Check the configuration file and exit.\n"
"   -S   Print the live group table as JSON (default socket /var/run/igmpproxy.sock).\n"
"   -E   Print the event log: joins, leaves, timeouts, queries (JSON Lines).\n"
"   -Q   Send a general query now, so every member reports again.\n"
"\n"
PACKAGE_STRING "\n"
;

// Local function Prototypes
static void signalHandler(int);
int     igmpProxyInit(void);
void    igmpProxyCleanUp(void);
void    igmpProxyRun(void);

// Global vars...
static int sighandled = 0;
#define GOT_SIGINT  0x01
#define GOT_SIGHUP  0x02
#define GOT_SIGUSR1 0x04
#define GOT_SIGUSR2 0x08

// Holds the indeces of the upstream IF...
int     upStreamIfIdx[MAX_UPS_VIFS];

// Daemon start time (monotonic ms)
msec_t  startTime;

/**
*   Program main method. Is invoked when the program is started
*   on commandline. The number of commandline arguments, and a
*   pointer to the arguments are received on the line...
*/
int main( int ArgCn, char *ArgVc[] ) {

    int c, devnull = -1;
    bool NotAsDaemon = false;
    bool TestConfig = false;
    const char *ClientCmd = NULL;

    struct Config *config = NULL;
    struct passwd *pw = NULL;

    srand(time(NULL) * getpid());

    // Parse the commandline options and setup basic settings..
    while ((c = getopt(ArgCn, ArgVc, "vdnhtSEQ")) != -1) {
        switch (c) {
        case 't':
            TestConfig = true;
            Log2Stderr = true;
            break;
        case 'S':
            ClientCmd = "status";
            break;
        case 'E':
            ClientCmd = "events";
            break;
        case 'Q':
            ClientCmd = "query";
            break;
        case 'n':
            NotAsDaemon = true;
            break;
        case 'd':
            Log2Stderr = true;
            NotAsDaemon = true;
            break;
        case 'v':
            if (LogLevel == LOG_INFO)
                LogLevel = LOG_DEBUG;
            else
                LogLevel = LOG_INFO;
            break;
        case 'h':
            fputs(Usage, stderr);
            exit(0);
            break;
        default:
            exit(1);
            break;
        }
    }

    if (ClientCmd) {
        exit(controlClient(optind < ArgCn ? ArgVc[optind] : "/var/run/igmpproxy.sock", ClientCmd));
    }

    if (optind != ArgCn - 1) {
        fputs("You must specify the configuration file.\n", stderr);
        exit(1);
    }
    char *configFilePath = ArgVc[optind];

    if (TestConfig) {
        // loadConfig() exits non-zero on hard errors
        if (!loadConfig(configFilePath)) {
            fputs("igmpproxy: configuration is invalid\n", stderr);
            exit(1);
        }
        printf("igmpproxy: configuration OK\n");
        exit(0);
    }

    // Chech that we are root
    if (geteuid() != 0) {
       fprintf(stderr, "igmpproxy: must be root\n");
       exit(1);
    }

    openlog("igmpproxy", LOG_PID, LOG_USER);

    // Write debug notice with file path...
    my_log(LOG_DEBUG, 0, "Searching for config file at '%s'" , configFilePath);

    do {

        // Loads the config file...
        if( ! loadConfig( configFilePath ) ) {
            my_log(LOG_ERR, 0, "Unable to load config file...");
            break;
        }

        // Initializes the deamon.
        if ( !igmpProxyInit() ) {
            my_log(LOG_ERR, 0, "Unable to initialize IGMPproxy.");
            break;
        }

        // Open /dev/null before chrooting.
        if (!NotAsDaemon) {
            devnull = open("/dev/null", O_RDWR);
            if (devnull == -1)
                my_log(LOG_ERR, 0, "unable to open /dev/null");
        }

        config = getCommonConfig();

        if (config->user[0]) {
            pw = getpwnam(config->user);
            if (pw == NULL)
                my_log(LOG_ERR, 0, "unknown user %s", config->user);
        }

        if (config->chroot[0])
            if (chroot(config->chroot) != 0 || chdir("/") != 0)
                my_log(LOG_ERR, 0, "unable to chroot to %s",
                  config->chroot);

        if (pw != NULL)
            if (setgroups(1, &pw->pw_gid) != 0 ||
              setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) != 0 ||
              setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) != 0)
                my_log(LOG_ERR, 0, "unable to drop privileges to %s",
                  config->user);

        if ( !NotAsDaemon ) {

            // Only daemon goes past this line...
            if (fork()) exit(0);

            // Detach daemon from terminal
            if ( close( 0 ) < 0 || close( 1 ) < 0 || close( 2 ) < 0 ||
              dup2( devnull, 0 ) < 0 || dup2( devnull, 1 ) < 0 ||
              dup2( devnull, 2 ) < 0 || setpgid( 0, 0 ) < 0 ) {
                my_log( LOG_ERR, errno, "failed to detach daemon" );
            }
            if (devnull > 2)
                close(devnull);
        }

        // Go to the main loop.
        igmpProxyRun();

        // Clean up
        igmpProxyCleanUp();

    } while ( false );

    // Inform that we are exiting.
    my_log(LOG_INFO, 0, "Shutdown complete....");

    exit(0);
}

/**
*   Handles the initial startup of the daemon.
*/
int igmpProxyInit(void) {
    struct sigaction sa;
    int Err;

    // A status client that disconnects mid-reply must not kill the proxy.
    signal(SIGPIPE, SIG_IGN);

    sa.sa_handler = signalHandler;
    sa.sa_flags = 0;    /* Interrupt system calls */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Loads configuration for Physical interfaces...
    buildIfVc();

    // Configures IF states and settings
    configureVifs();

    switch ( Err = enableMRouter() ) {
    case 0: break;
    case EADDRINUSE: my_log( LOG_ERR, EADDRINUSE, "MC-Router API already in use" ); break;
    default: my_log( LOG_ERR, Err, "MRT_INIT failed" );
    }

    /* create VIFs for all IP, non-loop interfaces
     */
    {
        unsigned Ix;
        struct IfDesc *Dp;
        int     vifcount = 0, upsvifcount = 0;

        // init array to "not set"
        for ( Ix = 0; Ix < MAX_UPS_VIFS; Ix++)
        {
            upStreamIfIdx[Ix] = -1;
        }

        for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {

            if ( Dp->InAdr.s_addr && ! (Dp->Flags & IFF_LOOPBACK) ) {
                if(Dp->state == IF_STATE_UPSTREAM) {
                    if (upsvifcount < MAX_UPS_VIFS -1)
                    {
                        my_log(LOG_DEBUG, 0, "Found upstrem IF #%d, will assing as upstream Vif %d",
                            upsvifcount, Ix);
                        upStreamIfIdx[upsvifcount++] = Ix;
                    } else {
                        my_log(LOG_ERR, 0, "Cannot set VIF #%d as upstream as well. Mac upstream Vif count is %d",
                            Ix, MAX_UPS_VIFS);
                    }
                }

                if (Dp->state != IF_STATE_DISABLED) {
                    addVIF( Dp );
                    vifcount++;
                }
            }
        }

        if(0 == upsvifcount) {
            my_log(LOG_ERR, 0, "There must be at least 1 Vif as upstream.");
        }
    }

    // Event log first, so the startup queries are in it too
    eventLogInit();
    {
        struct Config *c = getCommonConfig();
        eventLog("start", 0, NULL, 0,
                 "\"version\":\"%s\",\"fastleave\":\"%s\",\"query_version\":%u,"
                 "\"robustness\":%u,\"query_interval_ms\":%u,\"query_response_interval_ms\":%u,"
                 "\"last_member_query_interval_ms\":%u,\"last_member_query_count\":%u,"
                 "\"upstream_leave_delay_ms\":%u",
                 PACKAGE_VERSION, fastLeaveName(c->fastLeave), c->queryVersion,
                 c->robustnessValue, c->queryInterval, c->queryResponseInterval,
                 c->lastMemberQueryInterval, c->lastMemberQueryCount, c->upstreamLeaveDelay);
    }
    // Initialize IGMP
    initIgmp();
    // Initialize Routing table
    initRouteTable();
    // Initialize timer
    callout_init();
    // Status socket (created before chroot / privilege drop)
    controlInit();
    startTime = now_ms();

    return 1;
}

/**
*   Clean up all on exit...
*/
void igmpProxyCleanUp(void) {
    my_log( LOG_DEBUG, 0, "clean handler called" );

    free_all_callouts();    // No more timeouts.
    controlCleanUp();       // Remove status socket.
    clearAllRoutes();       // Remove all routes.
    disableMRouter();       // Disable the multirout API
    eventLog("exit", 0, NULL, 0, NULL);
    eventLogClose();
}

/**
*   Main daemon loop.
*/
void igmpProxyRun(void) {
    struct Config *config = getCommonConfig();
    int     recvlen;
    int     MaxFD, Rt, ctl;
    long    waitMs;
    fd_set  ReadFDS;
    socklen_t dummy = 0;
    struct  timespec tv;
    msec_t  now, nextRoute = 0;

    // First thing we send a membership query in downstream VIF's...
    sendGeneralMembershipQuery();

    for (;;) {

        if (sighandled & GOT_SIGINT) {
            sighandled &= ~GOT_SIGINT;
            my_log(LOG_NOTICE, 0, "Got a interrupt signal. Exiting.");
            break;
        }

        /* aimwang: call rebuildIfVc */
        if (config->rescanVif)
            rebuildIfVc();

        // Wait until the next callout or group timer, at most 1 s when
        // rescanning interfaces, otherwise 3 s (keeps signal latency low).
        now = now_ms();
        waitMs = timer_nextTimer(now);
        if (nextRoute) {
            long r = nextRoute > now ? (long)(nextRoute - now) : 0;
            if (waitMs < 0 || r < waitMs)
                waitMs = r;
        }
        if (waitMs < 0 || waitMs > (config->rescanVif ? 1000 : 3000))
            waitMs = config->rescanVif ? 1000 : 3000;
        tv.tv_sec = waitMs / 1000;
        tv.tv_nsec = (waitMs % 1000) * 1000000L;

        FD_ZERO( &ReadFDS );
        FD_SET( MRouterFD, &ReadFDS );
        MaxFD = MRouterFD;
        ctl = controlFd();
        if (ctl >= 0) {
            FD_SET(ctl, &ReadFDS);
            if (ctl > MaxFD)
                MaxFD = ctl;
        }

        Rt = pselect( MaxFD +1, &ReadFDS, NULL, NULL, &tv, NULL );

        if( Rt < 0 ) {
            if (errno != EINTR)
                my_log( LOG_WARNING, errno, "select() failure" );
            continue;
        }
        if( Rt > 0 ) {
            if( FD_ISSET( MRouterFD, &ReadFDS ) ) {
                recvlen = recvfrom(MRouterFD, recv_buf, RECV_BUF_SIZE,
                                   0, NULL, &dummy);
                if (recvlen < 0) {
                    if (errno != EINTR) my_log(LOG_ERR, errno, "recvfrom");
                } else {
                    acceptIgmp(recvlen);
                }
            }
            if (ctl >= 0 && FD_ISSET(ctl, &ReadFDS))
                controlAccept();
        }

        // Handle all timers that are due.
        now = now_ms();
        callout_run(now);
        nextRoute = routeTableTick(now);
    }
}

/*
 * Signal handler.  Take note of the fact that the signal arrived
 * so that the main loop can take care of it.
 */
static void signalHandler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        sighandled |= GOT_SIGINT;
        break;
        /* XXX: Not in use.
        case SIGHUP:
            sighandled |= GOT_SIGHUP;
            break;

        case SIGUSR1:
            sighandled |= GOT_SIGUSR1;
            break;

        case SIGUSR2:
            sighandled |= GOT_SIGUSR2;
            break;
        */
    }
}
