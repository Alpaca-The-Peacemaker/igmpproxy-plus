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
*   igmpproxy.h - Header file for common includes.
*/

#include "config.h"
#include "os.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * Limit on length of route data
 */
#define MAX_IP_PACKET_LEN	576
#define MIN_IP_HEADER_LEN	20
#define MAX_IP_HEADER_LEN	60
#define IP_HEADER_RAOPT_LEN	24

#define MAX_UPS_VIFS    8

// Useful macros..
#define VCMC( Vc )  (sizeof( Vc ) / sizeof( (Vc)[ 0 ] ))
#define VCEP( Vc )  (&(Vc)[ VCMC( Vc ) ])

// Bit manipulation macros...
#define BIT_ZERO(X)      ((X) = 0)
#define BIT_SET(X,n)     ((X) |= 1 << (n))
#define BIT_CLR(X,n)     ((X) &= ~(1 << (n)))
#define BIT_TST(X,n)     ((X) & 1 << (n))


//#################################################################################
//  Globals
//#################################################################################

/*
 * External declarations for global variables and functions.
 */
#define RECV_BUF_SIZE 8192
extern char     *recv_buf;
extern char     *send_buf;

extern char     s1[];
extern char     s2[];
extern char		s3[];
extern char		s4[];



//#################################################################################
//  Lib function prototypes.
//#################################################################################

/* syslog.c
 */
extern bool Log2Stderr;           // Log to stderr instead of to syslog
extern int  LogLevel;             // Log threshold, LOG_WARNING .... LOG_DEBUG

void my_log( int Serverity, int Errno, const char *FmtSt, ... );

/* ifvc.c
 */
#define MAX_IF         40     // max. number of interfaces recognized

// Interface states
#define IF_STATE_DISABLED      0   // Interface should be ignored.
#define IF_STATE_UPSTREAM      1   // Interface is the upstream interface
#define IF_STATE_DOWNSTREAM    2   // Interface is a downstream interface
#define IF_STATE_LOST          3   // aimwang: Temp from downstream to hidden
#define IF_STATE_HIDDEN        4   // aimwang: Interface is hidden

// Multicast default values...
#define DEFAULT_ROBUSTNESS     2
#define DEFAULT_THRESHOLD      1
#define DEFAULT_RATELIMIT      0

// Define timer constants. igmpproxy-plus keeps all timers in milliseconds.
#define INTERVAL_QUERY              125000  // RFC 2236 Query Interval
#define INTERVAL_QUERY_RESPONSE      10000  // RFC 2236 Query Response Interval
#define INTERVAL_LAST_MEMBER_QUERY    1000  // RFC 2236 Last Member Query Interval
#define DEFAULT_HOST_LIMIT             256  // tracked hosts per group per interface

// Fast leave modes
#define FASTLEAVE_INHERIT   -1  // per-interface: use global setting
#define FASTLEAVE_OFF        0  // RFC behaviour: keep forwarding until last member queries time out
#define FASTLEAVE_ON         1  // stop forwarding on that interface as soon as any leave arrives
#define FASTLEAVE_AUTO       2  // stop forwarding immediately when the last tracked host leaves

typedef uint64_t msec_t;

#define ROUTESTATE_NOTJOINED            0   // The group corresponding to route is not joined
#define ROUTESTATE_JOINED               1   // The group corresponding to route is joined
#define ROUTESTATE_CHECK_LAST_MEMBER    2   // The router is checking for hosts



// Linked list of networks...
struct SubnetList {
    uint32_t            subnet_addr;
    uint32_t            subnet_mask;
    struct SubnetList   *next;
    bool                allow;
};

struct IfDesc {
    char                Name[IF_NAMESIZE];
    struct in_addr      InAdr;          /* == 0 for non IP interfaces */
    int                 ifIndex;
    short               Flags;
    short               state;
    struct SubnetList*  allowednets;
    struct SubnetList*  allowedgroups;
    unsigned int        robustness;  /* 0 = use global value */
    int                 fastLeave;   /* FASTLEAVE_* */
    unsigned int        lastMemberQueryInterval; /* ms, 0 = use global */
    unsigned int        lastMemberQueryCount;    /* 0 = use global */
    unsigned int        groupTimeout;            /* ms, 0 = computed GMI */
    unsigned int        queryVersion;            /* 2 or 3, 0 = use global */
    unsigned char       threshold;   /* ttl limit */
    unsigned int        ratelimit;
    unsigned int        index;
};

// Keeps common configuration settings (all times in milliseconds)
struct Config {
    unsigned int        robustnessValue;
    unsigned int        queryInterval;
    unsigned int        queryResponseInterval;
    // Used on startup..
    unsigned int        startupQueryInterval;
    unsigned int        startupQueryCount;
    // Last member probe...
    unsigned int        lastMemberQueryInterval;
    unsigned int        lastMemberQueryCount;
    // Fast leave mode for downstream interfaces (FASTLEAVE_*)
    int                 fastLeave;
    // Delay before the upstream leave is sent once no downstream members remain
    unsigned int        upstreamLeaveDelay;
    // Max tracked hosts per group per interface
    unsigned int        hostLimit;
    // IGMP version of the queries sent on downstream interfaces (2 or 3)
    unsigned int        queryVersion;
    // Path of the status socket, empty = disabled
    char                controlSocket[108];
    //~ aimwang added
    // Set if nneed to detect new interface.
    unsigned short	rescanVif;
    // Set if not detect new interface for down stream.
    unsigned short	defaultInterfaceState;	// 0: disable, 2: downstream
    //~ aimwang added done
    char                chroot[PATH_MAX];
    char                user[LOGIN_NAME_MAX];
};

// Holds the indeces of the upstream IF...
extern int upStreamIfIdx[MAX_UPS_VIFS];

/* ifvc.c
 */
void rebuildIfVc( void );
void buildIfVc( void );
struct IfDesc *getIfByName( const char *IfName );
struct IfDesc *getIfByIx( unsigned Ix );
struct IfDesc *getIfByAddress( uint32_t Ix );
struct IfDesc *getIfByVifIndex( unsigned vifindex );
int isAdressValidForIf(struct IfDesc* intrface, uint32_t ipaddr);

/* mroute-api.c
 */
struct MRouteDesc {
    struct in_addr  OriginAdr, McAdr;
    short           InVif;
    uint8_t         TtlVc[MAXVIFS];
};

// IGMP socket as interface for the mrouted API
// - receives the IGMP messages
extern int MRouterFD;

int enableMRouter( void );
void disableMRouter( void );
void addVIF( struct IfDesc *Dp );
void delVIF( struct IfDesc *Dp );
int addMRoute( struct MRouteDesc * Dp );
int delMRoute( struct MRouteDesc * Dp );
int getVifIx( struct IfDesc *IfDp );

/* config.c
 */
int loadConfig(char *configFile);
void configureVifs(void);
struct Config *getCommonConfig(void);
void ifTuningDefaults(struct IfDesc *Dp);
int ifFastLeave(const struct IfDesc *Dp);
unsigned int ifLastMemberQueryInterval(const struct IfDesc *Dp);
unsigned int ifLastMemberQueryCount(const struct IfDesc *Dp);
unsigned int ifGroupMembershipInterval(const struct IfDesc *Dp);
unsigned int ifQueryVersion(const struct IfDesc *Dp);
const char *fastLeaveName(int mode);

/* igmp.c
*/
extern uint32_t allhosts_group;
extern uint32_t allrouters_group;
extern uint32_t alligmp3_group;
void initIgmp(void);
void acceptIgmp(int);
void sendIgmp (uint32_t, uint32_t, int, int, uint32_t, int, int);

/* lib.c
 */
char   *fmtInAdr( char *St, struct in_addr InAdr );
char   *inetFmt(uint32_t addr, char *s);
char   *inetFmts(uint32_t addr, uint32_t mask, char *s);
msec_t  now_ms(void);
uint16_t inetChksum(uint16_t *addr, int len);

/* kern.c
 */
void k_set_rcvbuf(int bufsize, int minsize);
void k_hdr_include(int hdrincl);
void k_set_ttl(int t);
void k_set_loop(int l);
void k_set_if(uint32_t ifa, int ifidx);
void k_join(struct IfDesc *ifd, uint32_t grp);
void k_leave(struct IfDesc *ifd, uint32_t grp);

/* rttable.c
 */
void initRouteTable(void);
void clearAllRoutes(void);
int insertRoute(uint32_t group, int ifx, uint32_t src, int version);
int activateRoute(uint32_t group, uint32_t originAddr, int upstrVif);
void routeLeave(uint32_t group, int ifx, uint32_t src);
msec_t routeTableTick(msec_t now);
void routeTableDumpJson(FILE *f, msec_t now);

/* request.c
 */
void acceptGroupReport(uint32_t src, uint32_t group, int version);
void acceptLeaveMessage(uint32_t src, uint32_t group);
void sendGeneralMembershipQuery(void);
void sendGroupSpecificQuery(struct IfDesc *Dp, uint32_t group);

/* control.c
 */
void controlInit(void);
int  controlFd(void);
void controlAccept(void);
void controlCleanUp(void);
int  controlClient(const char *path);
extern msec_t startTime;

/* callout.c  (all delays in milliseconds)
*/
typedef void (*timer_f)(void *);

void callout_init(void);
void free_all_callouts(void);
void callout_run(msec_t now);
int timer_nextTimer(msec_t now);
int timer_setTimer(int, timer_f, void *);

/* confread.c
 */
#define MAX_TOKEN_LENGTH    255

int openConfigFile(char *filename);
void closeConfigFile(void);
char* nextConfigToken(void);
char* getCurrentConfigToken(void);
