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
*   request.c
*
*   Functions for recieveing and processing IGMP requests.
*
*/

#include "igmpproxy.h"

/**
*   224.0.0.0/24 is link-local (RFC 5771): never routed, so memberships
*   there (mDNS, LLMNR, ...) must not be proxied upstream.
*/
static bool linkLocalGroup(uint32_t group) {
    return (ntohl(group) & 0xffffff00) == 0xe0000000;
}

/**
*   Checks the downstream black/whitelist for a group on an interface.
*/
static bool groupAllowedOn(struct IfDesc *Dp, uint32_t group) {
    bool                 allow_list = false;
    struct SubnetList   *match = NULL;
    struct SubnetList   *sn;

    if (Dp->allowedgroups == NULL)
        return true;

    for(sn = Dp->allowedgroups; sn != NULL; sn = sn->next) {
        if (sn->allow)
            allow_list = true;
        if((group & sn->subnet_mask) == sn->subnet_addr)
            match = sn;
    }
    return (!allow_list && match == NULL) ||
           (allow_list && match != NULL && match->allow);
}

static void sendQuery(struct IfDesc *Dp, uint32_t dst, uint32_t group, unsigned int maxRespMs, int forceV2);
static void querierLeave(struct IfDesc *Dp, uint32_t group);
static void querierReport(struct IfDesc *Dp, uint32_t group, uint32_t src);
static msec_t querierGroupsTick(msec_t now);

/**
*   Handles incoming membership reports, and
*   appends them to the routing table.
*/
void acceptGroupReport(uint32_t src, uint32_t group, int version) {
    struct IfDesc  *sourceVif;

    if(!IN_MULTICAST( ntohl(group) )) {
        my_log(LOG_WARNING, 0, "The group address %s is not a valid Multicast group.",
            inetFmt(group, s1));
        return;
    }

    if (linkLocalGroup(group)) {
        my_log(LOG_DEBUG, 0, "Ignoring link-local group %s", inetFmt(group, s1));
        return;
    }

    sourceVif = getIfByAddress( src );
    if(sourceVif == NULL) {
        my_log(LINKLOCAL_SRC(src) ? LOG_INFO : LOG_WARNING, 0, "No interfaces found for source %s",
            inetFmt(src,s1));
        return;
    }

    if(sourceVif->InAdr.s_addr == src) {
        my_log(LOG_DEBUG, 0, "The IGMP message was from myself. Ignoring.");
        return;
    }

    if (sourceVif->state == IF_STATE_QUERIER) {
        // Querier only: nothing is forwarded here, the report only shows
        // that the snooping switches will see members answer.
        if (eventLogReports())
            eventLog("report", group, sourceVif, src, "\"igmp\":%d,\"role\":\"querier\"", version);
        querierReport(sourceVif, group, src);
        return;
    }

    if(sourceVif->state != IF_STATE_DOWNSTREAM) {
        my_log(LOG_INFO, 0, "Membership report was received on %s. Ignoring.",
            sourceVif->state==IF_STATE_UPSTREAM?"the upstream interface":"a disabled interface");
        return;
    }

    if (!groupAllowedOn(sourceVif, group)) {
        my_log(LOG_INFO, 0, "The group address %s may not be requested from this interface. Ignoring.",
               inetFmt(group, s1));
        return;
    }

    my_log(LOG_DEBUG, 0, "Report for %s from %s on %s (VIF %d)",
        inetFmt(group,s1), inetFmt(src,s2), sourceVif->Name, sourceVif->index);
    insertRoute(group, sourceVif->index, src, version);
}

/**
*   Receives and handles a group leave message (IGMPv2 leave or
*   IGMPv3 TO_IN{} / IS_IN{} with no sources).
*/
void acceptLeaveMessage(uint32_t src, uint32_t group) {
    struct IfDesc   *sourceVif;

    if(!IN_MULTICAST( ntohl(group) )) {
        my_log(LOG_WARNING, 0, "The group address %s is not a valid Multicast group.",
            inetFmt(group, s1));
        return;
    }

    if (linkLocalGroup(group)) {
        my_log(LOG_DEBUG, 0, "Ignoring link-local group %s", inetFmt(group, s1));
        return;
    }

    sourceVif = getIfByAddress( src );
    if(sourceVif == NULL) {
        my_log(LINKLOCAL_SRC(src) ? LOG_INFO : LOG_WARNING, 0, "No interfaces found for source %s",
            inetFmt(src,s1));
        return;
    }

    if (sourceVif->state == IF_STATE_QUERIER) {
        eventLog("leave", group, sourceVif, src, "\"role\":\"querier\"");
        querierLeave(sourceVif, group);
        return;
    }

    if(sourceVif->state != IF_STATE_DOWNSTREAM) {
        my_log(LOG_DEBUG, 0, "The found if for %s was not downstream. Ignoring leave request.",
               inetFmt(src, s1));
        return;
    }

    my_log(LOG_DEBUG, 0, "Leave for %s from %s on %s",
        inetFmt(group, s1), inetFmt(src, s2), sourceVif->Name);
    routeLeave(group, sourceVif->index, src);
}

/**
*   Converts milliseconds into an IGMPv2 max response code (1/10 s).
*/
static int maxRespCode(unsigned int ms) {
    unsigned int code = (ms + 99) / 100;
    if (code < 1)
        code = 1;
    if (code > 255)
        code = 255;
    return (int)code;
}

/**
*   RFC 3376 4.1.1 / 4.1.7: values from 128 on use an 8 bit floating point
*   format, value = (mant | 0x10) << (exp + 3). Rounds down.
*/
static int igmpv3Code(unsigned int v) {
    unsigned int exp = 0;

    if (v < 128)
        return (int)v;
    while (exp < 7 && (v >> (exp + 3)) > 0x1f)
        exp++;
    if ((v >> (exp + 3)) > 0x1f)
        return 0xff;
    return (int)(0x80 | (exp << 4) | ((v >> (exp + 3)) & 0x0f));
}

/**
*   Sends a membership query on one interface: general (group 0) or group
*   specific. IGMPv3 queries (RFC 3376 4.1) add S/QRV, QQIC and a zero
*   source count after the IGMPv2 header; IGMPv2 hosts accept them as v2
*   queries.
*/
static void sendQuery(struct IfDesc *Dp, uint32_t dst, uint32_t group, unsigned int maxRespMs, int forceV2) {
    struct Config *conf = getCommonConfig();
    unsigned int tenths = (maxRespMs + 99) / 100;

    if (tenths < 1)
        tenths = 1;

    if (ifQueryVersion(Dp) == 3 && !forceV2) {
        unsigned char *data = (unsigned char *)send_buf + IP_HEADER_RAOPT_LEN + IGMP_MINLEN;
        unsigned int qrv = Dp->robustness ? Dp->robustness : conf->robustnessValue;

        data[0] = (unsigned char)(qrv > 7 ? 0 : qrv);          /* Resv | S=0 | QRV */
        data[1] = (unsigned char)igmpv3Code(conf->queryInterval / 1000);  /* QQIC */
        data[2] = 0;                                            /* number of sources */
        data[3] = 0;
        sendIgmp(Dp->InAdr.s_addr, dst, IGMP_MEMBERSHIP_QUERY,
                 igmpv3Code(tenths), group, 4, Dp->ifIndex);
    } else {
        sendIgmp(Dp->InAdr.s_addr, dst, IGMP_MEMBERSHIP_QUERY,
                 maxRespCode(maxRespMs), group, 0, Dp->ifIndex);
    }
}

/**
*   Sends one group specific query on one interface.
*/
/**
*   Group specific queries go out in IGMPv2 format when an IGMPv1/v2 host
*   reported the group on this interface (forceV2): some IPTV set-top boxes
*   do not answer IGMPv3 group specific queries.
*/
void sendGroupSpecificQuery(struct IfDesc *Dp, uint32_t group, int forceV2) {
    unsigned int lmqi = ifLastMemberQueryInterval(Dp);

    if (!Dp->InAdr.s_addr || (Dp->state != IF_STATE_DOWNSTREAM && Dp->state != IF_STATE_QUERIER))
        return;

    sendQuery(Dp, group, group, lmqi, forceV2);

    my_log(LOG_DEBUG, 0, "Sent group specific query for %s on %s. Max resp: %ums",
           inetFmt(group, s1), Dp->Name, lmqi);
    eventLog("query", group, Dp, 0, "\"kind\":\"group\",\"igmp\":%d,\"max_resp_ms\":%u",
             (ifQueryVersion(Dp) == 3 && !forceV2) ? 3 : 2, lmqi);
}

/**
*   Sends a general query on every downstream VIF, without touching the
*   periodic query timer. 'why' ends up in the event log
*   ("periodic", "startup", "requested").
*/
static void sendGeneralQueryOn(struct IfDesc *Dp, const char *why) {
    struct  Config  *conf = getCommonConfig();

    sendQuery(Dp, allhosts_group, 0, conf->queryResponseInterval, 0);

    my_log(LOG_DEBUG, 0, "Sent membership query from %s ifIndex %d to %s. Max resp: %ums",
        inetFmt(Dp->InAdr.s_addr,s1), Dp->ifIndex,
        inetFmt(allhosts_group,s2), conf->queryResponseInterval);
    if (eventLogReports() || strcmp(why, "periodic"))
        eventLog("query", 0, Dp, 0, "\"kind\":\"general\",\"igmp\":%u,\"max_resp_ms\":%u,\"why\":\"%s\"",
                 ifQueryVersion(Dp), conf->queryResponseInterval, why);
}

/*
 * General query on every downstream and querier interface. force: also
 * where another querier is active ("igmpproxy -Q" / refresh button).
 */
static void sendGeneralQueries(const char *why, int force) {
    struct  IfDesc  *Dp;
    int             Ix;

    for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {
        if ( !Dp->InAdr.s_addr || (Dp->Flags & IFF_LOOPBACK) ||
             (Dp->state != IF_STATE_DOWNSTREAM && Dp->state != IF_STATE_QUERIER))
            continue;
        if (!force && Dp->otherQuerierUntil) {
            my_log(LOG_DEBUG, 0, "Not the querier on %s (%s is): no general query",
                   Dp->Name, inetFmt(Dp->otherQuerier, s1));
            continue;
        }
        sendGeneralQueryOn(Dp, why);
    }
}

/**
*   "igmpproxy -Q" / refresh button: ask every member to report now. The
*   periodic schedule is left as it is.
*/
void sendGeneralQueriesNow(const char *why) {
    my_log(LOG_INFO, 0, "General query on all downstream interfaces (%s)", why);
    sendGeneralQueries(why, 1);
}

/**
*   Sends a general membership query on downstream VIFs and schedules the
*   next one.
*/
void sendGeneralMembershipQuery(void) {
    struct  Config  *conf = getCommonConfig();

    sendGeneralQueries(conf->startupQueryCount > 0 ? "startup" : "periodic", 0);

    // Install timer for next general query...
    if(conf->startupQueryCount > 1) {
        conf->startupQueryCount--;
        timer_setTimer(conf->startupQueryInterval, (timer_f)sendGeneralMembershipQuery, NULL);
    } else {
        conf->startupQueryCount = 0;
        timer_setTimer(conf->queryInterval, (timer_f)sendGeneralMembershipQuery, NULL);
    }
}

/**
*   RFC 3376 4.1.7: max resp code and QQIC from 128 on are floating point.
*/
static unsigned int igmpv3Decode(unsigned char c) {
    if (c < 128)
        return c;
    return ((c & 0x0f) | 0x10) << (((c >> 4) & 0x07) + 3);
}

/**
*   A query from another router. With querier election on, the router
*   with the lowest address is the querier (RFC 2236 3, RFC 3376 6.6.2):
*   while one with a lower address than ours is active, we send no general
*   queries on that interface. We keep listening to the reports that answer
*   it, so forwarding works as before. Group specific queries after a leave
*   on a downstream interface are still ours: how fast a stream stops must
*   not depend on another router (documented deviation from the RFC).
*/
void acceptQuery(uint32_t src, uint32_t group, const unsigned char *igmp, int len) {
    struct IfDesc *Dp;
    msec_t now = now_ms();
    unsigned int maxResp, qi = 0, qrv = 0;

    if (src == 0)
        return;
    Dp = getIfByAddress(src);
    if (Dp == NULL || Dp->InAdr.s_addr == src)
        return;
    if (Dp->state != IF_STATE_DOWNSTREAM && Dp->state != IF_STATE_QUERIER)
        return;
    if (!ifQuerierElection(Dp))
        return;
    if (ntohl(src) >= ntohl(Dp->InAdr.s_addr)) {
        my_log(LOG_DEBUG, 0, "Query from %s on %s: higher address, we stay the querier",
               inetFmt(src, s1), Dp->Name);
        return;
    }

    if (len >= 12) {                       /* IGMPv3 query */
        maxResp = igmpv3Decode(igmp[1]) * 100;
        qrv = igmp[8] & 0x07;
        qi = igmpv3Decode(igmp[9]) * 1000;
    } else {
        maxResp = igmp[1] * 100;           /* IGMPv2 (1/10 s); 0 = IGMPv1 */
    }
    if (maxResp == 0)
        maxResp = 10000;

    if (Dp->otherQuerier != src) {
        my_log(LOG_NOTICE, 0, "%s: %s is the querier (lower address than ours), no general queries from us",
               Dp->Name, inetFmt(src, s1));
        eventLog("querier_lost", 0, Dp, src, NULL);
        Dp->otherQuerier = src;
        Dp->otherLastGeneral = 0;
        Dp->otherQueryInterval = 0;
        Dp->otherMaxResp = 0;
        Dp->otherRobustness = 0;
    }
    if (group == 0) {
        // Its query interval: learned from the gap between its general
        // queries (only grows, so a refresh query does not shorten it)
        if (Dp->otherLastGeneral) {
            msec_t gap = now - Dp->otherLastGeneral;
            if (gap >= 1000 && gap <= 3600000 && gap > Dp->otherQueryInterval)
                Dp->otherQueryInterval = (unsigned int)gap;
        }
        Dp->otherLastGeneral = now;
    }
    if (qi > Dp->otherQueryInterval)
        Dp->otherQueryInterval = qi;
    if (maxResp > Dp->otherMaxResp)
        Dp->otherMaxResp = maxResp;
    if (qrv > Dp->otherRobustness)
        Dp->otherRobustness = qrv;
    Dp->otherQuerierUntil = now + ifOtherQuerierPresentInterval(Dp);
}

/**
*   The other querier went quiet for the Other Querier Present Interval:
*   we are the querier again and ask at once. Returns the next deadline.
*/
msec_t querierTick(msec_t now) {
    struct IfDesc *Dp;
    msec_t next = querierGroupsTick(now);
    int Ix;

    for (Ix = 0; (Dp = getIfByIx(Ix)); Ix++) {
        if (!Dp->otherQuerierUntil)
            continue;
        if (Dp->otherQuerierUntil > now) {
            if (!next || Dp->otherQuerierUntil < next)
                next = Dp->otherQuerierUntil;
            continue;
        }
        my_log(LOG_NOTICE, 0, "%s: no query from %s for %us, we are the querier again",
               Dp->Name, inetFmt(Dp->otherQuerier, s1), ifOtherQuerierPresentInterval(Dp) / 1000);
        eventLog("querier_resumed", 0, Dp, Dp->otherQuerier, NULL);
        Dp->otherQuerier = 0;
        Dp->otherQuerierUntil = 0;
        Dp->otherLastGeneral = 0;
        Dp->otherQueryInterval = 0;
        Dp->otherMaxResp = 0;
        Dp->otherRobustness = 0;
        if (Dp->InAdr.s_addr && (Dp->state == IF_STATE_DOWNSTREAM || Dp->state == IF_STATE_QUERIER))
            sendGeneralQueryOn(Dp, "querier");
    }
    return next;
}

/*
 * Groups on querier interfaces. Nothing is forwarded there; the table only
 * lets us behave like a querier for the snooping switches: after a leave
 * for a group that has members we ask (last member query count x interval)
 * so the switches can prune the port at once, and a repeated leave for a
 * group that is already being checked or gone is ignored.
 */
struct QuerierGroup {
    char         ifName[IF_NAMESIZE];
    uint32_t     group;
    uint32_t     lastReporter;
    msec_t       expires;        // membership timer
    msec_t       nextQuery;      // next group specific query while checking
    unsigned int left;           // group specific queries still to send
    int          checking;       // a leave is being checked (until a report or expiry)
    struct QuerierGroup *next;
};

static struct QuerierGroup *querierGroups;

static struct QuerierGroup *findQuerierGroup(const struct IfDesc *Dp, uint32_t group) {
    struct QuerierGroup *q;
    for (q = querierGroups; q; q = q->next)
        if (q->group == group && !strcmp(q->ifName, Dp->Name))
            return q;
    return NULL;
}

static void querierReport(struct IfDesc *Dp, uint32_t group, uint32_t src) {
    struct QuerierGroup *q = findQuerierGroup(Dp, group);

    if (q == NULL) {
        q = (struct QuerierGroup *)calloc(1, sizeof(*q));
        if (q == NULL)
            return;
        snprintf(q->ifName, sizeof(q->ifName), "%s", Dp->Name);
        q->group = group;
        q->next = querierGroups;
        querierGroups = q;
        eventLog("join", group, Dp, src, "\"role\":\"querier\"");
    }
    q->lastReporter = src;
    q->expires = now_ms() + ifGroupMembershipInterval(Dp);
    q->left = 0;                 // a member answered: the check is over
    q->checking = 0;
}

static void querierLeave(struct IfDesc *Dp, uint32_t group) {
    struct QuerierGroup *q = findQuerierGroup(Dp, group);
    msec_t now = now_ms();

    if (q == NULL || q->checking)
        return;                  // gone already, or being checked
    if (Dp->otherQuerierUntil)
        return;                  // the active querier asks, not us
    q->checking = 1;
    q->left = ifLastMemberQueryCount(Dp);
    q->nextQuery = now;
    q->expires = now + (msec_t)q->left * ifLastMemberQueryInterval(Dp);
}

/*
 * Group specific queries and membership timers of querier interfaces.
 * Returns the next deadline (0 = none).
 */
static msec_t querierGroupsTick(msec_t now) {
    struct QuerierGroup **pp, *q;
    msec_t next = 0;

    for (pp = &querierGroups; (q = *pp); ) {
        struct IfDesc *Dp = getIfByName(q->ifName);

        if (Dp == NULL || Dp->state != IF_STATE_QUERIER || q->expires <= now) {
            *pp = q->next;
            free(q);
            continue;
        }
        if (q->left && q->nextQuery <= now) {
            if (!Dp->otherQuerierUntil)
                sendGroupSpecificQuery(Dp, q->group, 0);
            q->left--;
            q->nextQuery = now + ifLastMemberQueryInterval(Dp);
        }
        if (q->left && (!next || q->nextQuery < next))
            next = q->nextQuery;
        if (!next || q->expires < next)
            next = q->expires;
        pp = &q->next;
    }
    return next;
}

/*
 * Status JSON: the groups seen on a querier interface.
 */
void querierGroupsJson(FILE *f, const struct IfDesc *Dp, msec_t now) {
    struct QuerierGroup *q;
    char a[32], b[32];
    int first = 1;

    fputs(",\"groups\":[", f);
    for (q = querierGroups; q; q = q->next) {
        if (strcmp(q->ifName, Dp->Name))
            continue;
        fprintf(f, "%s{\"group\":\"%s\",\"last_reporter\":\"%s\",\"expires_in_ms\":%lld,\"checking\":%s}",
                first ? "" : ",", inetFmt(q->group, a), inetFmt(q->lastReporter, b),
                q->expires > now ? (long long)(q->expires - now) : 0LL, q->checking ? "true" : "false");
        first = 0;
    }
    fputc(']', f);
}
