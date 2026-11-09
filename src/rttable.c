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
*   rttable.c
*
*   igmpproxy-plus: group membership state machine.
*
*   Every group keeps, per downstream interface:
*     - a membership timer (absolute ms deadline, 0 = not forwarded there)
*     - the exact list of hosts that reported, each with its own timer
*     - last member query state (queries left, next query time)
*
*   Leaves are handled per interface (the stock proxy queried every
*   interface and aged the whole group). What happens on a leave depends
*   on the interface's fast leave mode:
*     off  - RFC 2236: send last member queries, stop forwarding when
*            lastmembercount * lastmemberinterval passes without a report.
*     on   - stop forwarding on that interface immediately. Queries are
*            still sent so remaining hosts can re-report within ms.
*     auto - stop immediately if the leaving host was the last tracked
*            host on that interface, else behave like "off".
*
*   When no interface forwards a group anymore, the upstream leave is sent
*   after upstreamleavedelay (0 = immediately). A join during that hold
*   time resumes forwarding without touching the upstream membership,
*   which makes channel zapping back and forth instant.
*/

#include "igmpproxy.h"

#define MAX_ORIGINS 4

struct HostEntry {
    struct HostEntry   *next;
    uint32_t            addr;
    msec_t              expires;
    msec_t              heard;      // last report from this host itself
};

struct VifState {
    msec_t              expires;    // membership timer, 0 = not a member
    msec_t              lmqNext;    // next last member query
    unsigned int        lmqLeft;    // last member queries still to send
    unsigned int        nhosts;
    bool                overflow;   // more hosts than hostlimit: untracked
    msec_t              olderHost;  // RFC 3376 7.3.2: v1/v2 host present until
    uint32_t            leftBy;     // host whose leave started the member check
    struct HostEntry   *hosts;
};

struct RouteTable {
    struct RouteTable   *nextroute;
    struct RouteTable   *prevroute;
    uint32_t            group;
    uint32_t            originAddrs[MAX_ORIGINS];
    uint32_t            vifBits;    // VIFs currently installed in the kernel

    short               upstrState;
    uint32_t            upstrJoined;  // bit per upStreamIfIdx[] slot
    int                 upstrVif;

    msec_t              created;
    msec_t              leaveAt;    // pending upstream leave / removal, 0 = none
    msec_t              joinRetryAt; // an upstream join failed: retry then
    unsigned long       reports;
    unsigned long       leaves;

    struct VifState     vif[MAXVIFS];
};

// Keeper for the routing table...
static struct RouteTable   *routing_table;

// Upstream joins refused by the kernel since start (status JSON)
static unsigned long upstreamJoinFailures;
#define JOIN_RETRY_MS 10000

static void logRouteTable(const char *header);
static int internUpdateKernelRoute(struct RouteTable *route, int activate);

/**
*   Initializes the routing table.
*/
void initRouteTable(void) {
    unsigned Ix;
    struct IfDesc *Dp;

    routing_table = NULL;

    // Join the all routers group on downstream vifs...
    for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {
        if( Dp->InAdr.s_addr && ! (Dp->Flags & IFF_LOOPBACK) && Dp->state == IF_STATE_DOWNSTREAM) {
            my_log(LOG_DEBUG, 0, "Joining all-routers group %s on vif %s",
                         inetFmt(allrouters_group,s1),inetFmt(Dp->InAdr.s_addr,s2));
            k_join(Dp, allrouters_group);

            my_log(LOG_DEBUG, 0, "Joining all igmpv3 multicast routers group %s on vif %s",
                         inetFmt(alligmp3_group,s1),inetFmt(Dp->InAdr.s_addr,s2));
            k_join(Dp, alligmp3_group);
        }
    }
}

/**
*   Send join or leave requests for a route on all upstream interfaces.
*/
static void sendJoinLeaveUpstream(struct RouteTable* route, int join) {
    struct IfDesc*      upstrIf;
    int i, failed = 0;

    for(i=0; i<MAX_UPS_VIFS && upStreamIfIdx[i] != -1; i++) {
        upstrIf = getIfByIx( upStreamIfIdx[i] );
        if(upstrIf == NULL) {
            my_log(LOG_ERR, 0 ,"FATAL: Unable to get Upstream IF.");
        }

        // Check if there is a black- or whitelist for the upstream VIF
        if (upstrIf->allowedgroups != NULL) {
            bool                 allow_list = false;
            struct SubnetList   *match = NULL;
            struct SubnetList   *sn;

            for(sn = upstrIf->allowedgroups; sn != NULL; sn = sn->next) {
                if (sn->allow)
                    allow_list = true;
                if((route->group & sn->subnet_mask) == sn->subnet_addr)
                    match = sn;
            }

            // Keep in sync with request.c, note the negation
            if(!((!allow_list && match == NULL) ||
              (allow_list && match != NULL && match->allow))) {
                my_log(LOG_INFO, 0, "The group address %s may not be forwarded upstream on %s. Ignoring.",
                       inetFmt(route->group, s1), upstrIf->Name);
                continue;
            }
        }

        if(join && !BIT_TST(route->upstrJoined, i)) {
            int err;
            my_log(LOG_INFO, 0, "Joining group %s upstream on %s",
                   inetFmt(route->group, s1), upstrIf->Name);
            if ((err = k_join(upstrIf, route->group)) == 0) {
                BIT_SET(route->upstrJoined, i);
                eventLog("upstream_join", route->group, upstrIf, 0, NULL);
            } else {
                failed = 1;
                upstreamJoinFailures++;
                eventLog("upstream_join_failed", route->group, upstrIf, 0,
                         "\"error\":\"%s\",\"retry_ms\":%u", strerror(err), JOIN_RETRY_MS);
            }
        } else if (!join && BIT_TST(route->upstrJoined, i)) {
            my_log(LOG_INFO, 0, "Leaving group %s upstream on %s",
                   inetFmt(route->group, s1), upstrIf->Name);
            k_leave(upstrIf, route->group);
            BIT_CLR(route->upstrJoined, i);
            eventLog("upstream_leave", route->group, upstrIf, 0, NULL);
        }
    }
    // JOINED also when every upstream filtered the group, so we do not
    // retry the join on every tick. A refused join is retried later.
    route->upstrState = (join && !failed) ? ROUTESTATE_JOINED : ROUTESTATE_NOTJOINED;
    route->joinRetryAt = (join && failed) ? now_ms() + JOIN_RETRY_MS : 0;
}

/*
 * Host list helpers
 */
static void freeHosts(struct VifState *v) {
    struct HostEntry *h, *n;
    for (h = v->hosts; h; h = n) {
        n = h->next;
        free(h);
    }
    v->hosts = NULL;
    v->nhosts = 0;
    v->overflow = false;
}

// Returns 1 when 'addr' was not tracked before.
static int touchHost(struct VifState *v, uint32_t addr, msec_t expires, msec_t now) {
    struct Config *conf = getCommonConfig();
    struct HostEntry *h;

    for (h = v->hosts; h; h = h->next) {
        if (h->addr == addr) {
            h->expires = expires;
            h->heard = now;
            return 0;
        }
    }
    if (v->nhosts >= conf->hostLimit) {
        if (!v->overflow)
            my_log(LOG_INFO, 0, "Host limit (%u) reached, host tracking disabled for this group/interface",
                   conf->hostLimit);
        v->overflow = true;
        return 0;
    }
    h = (struct HostEntry *)malloc(sizeof(*h));
    if (h == NULL) {
        v->overflow = true;
        return 0;
    }
    h->addr = addr;
    h->expires = expires;
    h->heard = now;
    h->next = v->hosts;
    v->hosts = h;
    v->nhosts++;
    return 1;
}

static void removeHost(struct VifState *v, uint32_t addr) {
    struct HostEntry **pp, *h;
    for (pp = &v->hosts; (h = *pp); pp = &h->next) {
        if (h->addr == addr) {
            *pp = h->next;
            free(h);
            v->nhosts--;
            return;
        }
    }
}

/**
*   Bits of the VIFs that currently have members for this group.
*/
static uint32_t memberBits(const struct RouteTable *r) {
    uint32_t bits = 0;
    int i;
    for (i = 0; i < MAXVIFS && i < 32; i++)
        if (r->vif[i].expires)
            BIT_SET(bits, i);
    return bits;
}

/**
*   Bring kernel forwarding and the upstream membership in line with the
*   per interface state. Never frees the route: removal is scheduled via
*   leaveAt and performed by routeTableTick().
*/
static void syncRoute(struct RouteTable *r, msec_t now) {
    struct Config *conf = getCommonConfig();
    uint32_t bits = memberBits(r);

    if (bits != r->vifBits) {
        my_log(LOG_DEBUG, 0, "Group %s: forwarding VIFs 0x%08x -> 0x%08x",
               inetFmt(r->group, s1), r->vifBits, bits);
        r->vifBits = bits;
        internUpdateKernelRoute(r, 1);
    }

    if (bits) {
        r->leaveAt = 0;
        if (r->upstrState != ROUTESTATE_JOINED && r->joinRetryAt <= now)
            sendJoinLeaveUpstream(r, 1);
    } else if (r->leaveAt == 0) {
        r->joinRetryAt = 0;
        if (r->upstrState == ROUTESTATE_JOINED || r->upstrJoined)
            r->leaveAt = now + conf->upstreamLeaveDelay;
        else
            // Data-only route without listeners: keep the empty kernel
            // entry a while so the kernel does not keep upcalling.
            r->leaveAt = now + conf->queryInterval;
        if (r->leaveAt == 0)
            r->leaveAt = 1;
    }
}

/**
*   Clear all routes from routing table, and alerts Leaves upstream.
*/
void clearAllRoutes(void) {
    struct RouteTable   *croute, *remainroute;
    int i;

    for(croute = routing_table; croute; croute = remainroute) {
        remainroute = croute->nextroute;

        my_log(LOG_DEBUG, 0, "Removing route entry for %s", inetFmt(croute->group, s1));

        internUpdateKernelRoute(croute, 0);
        if (croute->upstrState == ROUTESTATE_JOINED || croute->upstrJoined)
            sendJoinLeaveUpstream(croute, 0);

        for (i = 0; i < MAXVIFS; i++)
            freeHosts(&croute->vif[i]);
        free(croute);
    }
    routing_table = NULL;

    my_log(LOG_NOTICE, 0, "All routes removed. Routing table is empty.");
}

static struct RouteTable *findRoute(uint32_t group) {
    struct RouteTable*  croute;

    for(croute = routing_table; croute; croute = croute->nextroute) {
        if(croute->group == group)
            return croute;
        if(croute->group > group)
            break;
    }
    return NULL;
}

/**
*   Allocates a route and links it into the (sorted) table.
*/
static struct RouteTable *createRoute(uint32_t group) {
    struct RouteTable *r, *c;

    r = (struct RouteTable *)calloc(1, sizeof(*r));
    if (r == NULL) {
        my_log(LOG_WARNING, 0, "Out of memory creating route for %s", inetFmt(group, s1));
        return NULL;
    }
    r->group = group;
    r->upstrVif = -1;
    r->upstrState = ROUTESTATE_NOTJOINED;
    r->created = now_ms();

    if (routing_table == NULL || routing_table->group > group) {
        r->nextroute = routing_table;
        if (routing_table)
            routing_table->prevroute = r;
        routing_table = r;
    } else {
        for (c = routing_table; c->nextroute && c->nextroute->group < group; c = c->nextroute)
            ;
        r->nextroute = c->nextroute;
        r->prevroute = c;
        if (c->nextroute)
            c->nextroute->prevroute = r;
        c->nextroute = r;
    }

    my_log(LOG_INFO, 0, "Created route table entry for %s", inetFmt(group, s1));
    return r;
}

/**
*   A membership report for 'group' from host 'src' arrived on VIF 'ifx'.
*   ifx < 0 only creates the entry.
*/
int insertRoute(uint32_t group, int ifx, uint32_t src, int version) {
    struct RouteTable *r;
    struct IfDesc *Dp;
    struct VifState *v;
    msec_t now = now_ms(), exp;

    if( ! IN_MULTICAST( ntohl(group) )) {
        my_log(LOG_WARNING, 0, "The group address %s is not a valid Multicast group. Table insert failed.",
            inetFmt(group, s1));
        return 0;
    }

    if(ifx >= MAXVIFS || ifx >= 32) {
        my_log(LOG_WARNING, 0, "The VIF Ix %d is out of range (0-%d). Table insert failed.", ifx, MAXVIFS-1);
        return 0;
    }

    r = findRoute(group);
    if (r == NULL && (r = createRoute(group)) == NULL)
        return 0;

    if (ifx >= 0) {
        Dp = getIfByVifIndex(ifx);
        v = &r->vif[ifx];
        exp = now + ifGroupMembershipInterval(Dp);

        if (!v->expires) {
            my_log(LOG_INFO, 0, "Group %s: start forwarding on %s (report from %s)",
                   inetFmt(group, s1), Dp ? Dp->Name : "?", inetFmt(src, s2));
            eventLog("forward_start", group, Dp, src, "\"igmp\":%d", version);
        } else if (v->leftBy) {
            eventLog("leave_answered", group, Dp, src, "\"igmp\":%d,\"left\":\"%s\"",
                     version, inetFmt(v->leftBy, s3));
        }

        v->expires = exp;
        v->lmqLeft = 0;          // a report ends the last member query phase
        v->leftBy = 0;
        if (version < 3) {
            // Older Host Present Interval = robustness * QI + QRI
            if (!v->olderHost || v->olderHost <= now) {
                my_log(LOG_INFO, 0, "Group %s: IGMPv%d host %s on %s, IGMPv2 compatibility",
                       inetFmt(group, s1), version, inetFmt(src, s2), Dp ? Dp->Name : "?");
                eventLog("compat_v2", group, Dp, src, "\"igmp\":%d", version);
            }
            v->olderHost = now + ifGroupMembershipInterval(Dp);
        }
        if (src) {
            if (touchHost(v, src, exp, now))
                eventLog("join", group, Dp, src, "\"igmp\":%d,\"hosts\":%u", version, v->nhosts);
            else if (eventLogReports())
                eventLog("report", group, Dp, src, "\"igmp\":%d", version);
        }
        if (version < 3) {
            // IGMPv1/v2 hosts that heard this report suppress their own, so
            // their silence does not mean they left: keep every known host of
            // this group alive. They leave the list by a leave message, or
            // when nobody reported the group for the membership interval.
            struct HostEntry *h;
            for (h = v->hosts; h; h = h->next)
                h->expires = exp;
        }
        r->reports++;
    }

    syncRoute(r, now);
    logRouteTable("Insert Route");
    return 1;
}

/**
*   A leave (IGMPv2 leave or IGMPv3 TO_IN{}) from host 'src' for 'group'
*   arrived on VIF 'ifx'.
*/
void routeLeave(uint32_t group, int ifx, uint32_t src) {
    struct RouteTable *r;
    struct IfDesc *Dp;
    struct VifState *v;
    msec_t now = now_ms();
    unsigned int lmqi, lmqc;
    int mode;
    bool drop, others;
    unsigned int nothers = 0;

    if (ifx < 0 || ifx >= MAXVIFS || ifx >= 32)
        return;
    r = findRoute(group);
    if (r == NULL)
        return;
    v = &r->vif[ifx];
    if (!v->expires && !v->lmqLeft) {
        eventLog("leave", group, getIfByVifIndex(ifx), src, "\"action\":\"not_forwarded\"");
        return;     // not forwarded there, nothing to do
    }

    Dp = getIfByVifIndex(ifx);
    mode = ifFastLeave(Dp);
    lmqi = ifLastMemberQueryInterval(Dp);
    lmqc = ifLastMemberQueryCount(Dp);

    removeHost(v, src);
    r->leaves++;

    // Any other host still tracked on this interface keeps the group
    // flowing, whatever the mode. A host that only stays alive through
    // others' reports (IGMPv2 suppression) never answers for itself, so how
    // long ago it was heard says nothing about whether it is still watching:
    // it is kept until the membership timeout. Retaining a possibly
    // powered-off member is the price of never cutting a silent receiver.
    {
        struct HostEntry *h;
        for (h = v->hosts; h; h = h->next)
            nothers++;
        others = nothers > 0;
    }

    // Auto fast leave is instant only when the host list can be complete:
    // IGMPv3 queries and no IGMPv1/v2 host present. With IGMPv2 (queried or
    // compatibility mode) hosts suppress each other's reports, so the list
    // may miss members and auto behaves like off (group specific queries).
    drop = !others && !v->overflow &&
           ((mode == FASTLEAVE_ON) ||
            (mode == FASTLEAVE_AUTO &&
             ifQueryVersion(Dp) != 2 && !(v->olderHost > now)));

    eventLog("leave", group, Dp, src,
             "\"action\":\"%s\",\"others\":%u,\"untracked\":%s,\"mode\":\"%s\",\"compat_v2\":%s,\"check_ms\":%u",
             drop ? "stopped" : others ? "keeps" : "checking", nothers,
             v->overflow ? "true" : "false", fastLeaveName(mode),
             v->olderHost > now ? "true" : "false", drop || others ? 0 : lmqi * lmqc);

    if (drop) {
        my_log(LOG_WARNING, 0, "Group %s: stopped on %s, fast leave (last host %s left, mode %s)",
               inetFmt(group, s1), Dp ? Dp->Name : "?", inetFmt(src, s2), fastLeaveName(mode));
        eventLog("forward_stop", group, Dp, src, "\"reason\":\"fast_leave\"");
        v->expires = 0;
        freeHosts(v);
    } else if (others) {
        my_log(LOG_INFO, 0, "Group %s: leave from %s on %s, still watched by %u other host(s), keeps flowing",
               inetFmt(group, s1), inetFmt(src, s2), Dp ? Dp->Name : "?", nothers);
    } else {
        msec_t lmqt = now + (msec_t)lmqi * lmqc;
        my_log(LOG_INFO, 0, "Group %s: leave from %s on %s, checking for other members (%ux%ums)",
               inetFmt(group, s1), inetFmt(src, s2), Dp ? Dp->Name : "?", lmqc, lmqi);
        if (v->expires > lmqt)
            v->expires = lmqt;
        v->leftBy = src;
    }

    // Last member queries let remaining (possibly untracked) hosts answer.
    // In "on"/"auto" mode forwarding has already stopped and a report
    // simply resumes it.
    if (Dp && lmqc > 0) {
        sendGroupSpecificQuery(Dp, group, v->olderHost > now);
        v->lmqLeft = lmqc - 1;
        v->lmqNext = now + lmqi;
    }

    syncRoute(r, now);
    logRouteTable("Leave");
}

/**
*   Activates a passive group. If the group is already
*   activated, it's reinstalled in the kernel.
*/
int activateRoute(uint32_t group, uint32_t originAddr, int upstrVif) {
    struct RouteTable*  croute;
    int i;

    croute = findRoute(group);
    if (croute == NULL) {
        my_log(LOG_DEBUG, 0, "No table entry for %s [From: %s]. Inserting route.",
            inetFmt(group, s1),inetFmt(originAddr, s2));
        croute = createRoute(group);
        if (croute == NULL)
            return 0;
    }

    if(originAddr > 0) {
        // find this origin, or an unused slot
        for (i = 0; i < MAX_ORIGINS; i++) {
            if (croute->originAddrs[i] == originAddr || croute->originAddrs[i] == 0)
                break;
        }

        if (i == MAX_ORIGINS) {
            i = MAX_ORIGINS - 1;
            my_log(LOG_WARNING, 0, "Too many origins for route %s; replacing %s with %s",
                inetFmt(croute->group, s1),
                inetFmt(croute->originAddrs[i], s2),
                inetFmt(originAddr, s3));
            // remove the replaced (S,G) from the kernel
            {
                uint32_t keep[MAX_ORIGINS];
                memcpy(keep, croute->originAddrs, sizeof(keep));
                memset(croute->originAddrs, 0, sizeof(keep));
                croute->originAddrs[0] = keep[i];
                internUpdateKernelRoute(croute, 0);
                memcpy(croute->originAddrs, keep, sizeof(keep));
            }
        }

        croute->originAddrs[i] = originAddr;

        // move it to the top
        while (i > 0) {
            uint32_t t = croute->originAddrs[i - 1];
            croute->originAddrs[i - 1] = croute->originAddrs[i];
            croute->originAddrs[i] = t;
            i--;
        }
    }
    croute->upstrVif = upstrVif;

    // Always install: with no listeners this is an empty entry that stops
    // repeated kernel upcalls for the same flow.
    internUpdateKernelRoute(croute, 1);
    syncRoute(croute, now_ms());

    logRouteTable("Activate Route");
    return 1;
}

/**
*   Remove a route, leaving upstream if joined.
*/
static void removeRoute(struct RouteTable*  croute) {
    int i;

    my_log(LOG_INFO, 0, "Removing group %s from table.", inetFmt(croute->group, s1));

    internUpdateKernelRoute(croute, 0);

    if (croute->upstrState == ROUTESTATE_JOINED || croute->upstrJoined)
        sendJoinLeaveUpstream(croute, 0);

    if(croute->prevroute == NULL) {
        if(croute->nextroute != NULL)
            croute->nextroute->prevroute = NULL;
        routing_table = croute->nextroute;
    } else {
        croute->prevroute->nextroute = croute->nextroute;
        if(croute->nextroute != NULL)
            croute->nextroute->prevroute = croute->prevroute;
    }

    for (i = 0; i < MAXVIFS; i++)
        freeHosts(&croute->vif[i]);
    free(croute);

    logRouteTable("Remove route");
}

static inline void minDeadline(msec_t *next, msec_t t) {
    if (t && (*next == 0 || t < *next))
        *next = t;
}

/**
*   Process all timers that are due. Returns the next deadline (absolute
*   ms) or 0 if nothing is pending.
*/
msec_t routeTableTick(msec_t now) {
    struct RouteTable *r, *nr;
    msec_t next = 0;
    int i;

    for (r = routing_table; r; r = nr) {
        nr = r->nextroute;

        for (i = 0; i < MAXVIFS && i < 32; i++) {
            struct VifState *v = &r->vif[i];
            struct HostEntry **pp, *h;

            if (!v->expires && !v->lmqLeft)
                continue;

            // host timers
            for (pp = &v->hosts; (h = *pp); ) {
                if (h->expires <= now) {
                    my_log(LOG_DEBUG, 0, "Group %s: host %s timed out",
                           inetFmt(r->group, s1), inetFmt(h->addr, s2));
                    eventLog("host_timeout", r->group, getIfByVifIndex(i), h->addr, NULL);
                    *pp = h->next;
                    free(h);
                    v->nhosts--;
                } else {
                    minDeadline(&next, h->expires);
                    pp = &h->next;
                }
            }

            // last member queries
            if (v->lmqLeft && v->lmqNext <= now) {
                struct IfDesc *Dp = getIfByVifIndex(i);
                if (Dp)
                    sendGroupSpecificQuery(Dp, r->group, v->olderHost > now);
                v->lmqLeft--;
                v->lmqNext = now + ifLastMemberQueryInterval(Dp);
            }
            if (v->lmqLeft)
                minDeadline(&next, v->lmqNext);

            // membership timer
            if (v->expires && v->expires <= now) {
                struct IfDesc *Dp = getIfByVifIndex(i);
                if (v->leftBy) {
                    my_log(LOG_WARNING, 0, "Group %s: stopped on %s, no member answered after leave from %s",
                           inetFmt(r->group, s1), Dp ? Dp->Name : "?", inetFmt(v->leftBy, s2));
                    eventLog("forward_stop", r->group, Dp, v->leftBy, "\"reason\":\"no_answer\"");
                } else {
                    my_log(LOG_WARNING, 0, "Group %s: stopped on %s, no report for %us (membership timeout)",
                           inetFmt(r->group, s1), Dp ? Dp->Name : "?", ifGroupMembershipInterval(Dp) / 1000);
                    eventLog("forward_stop", r->group, Dp, 0, "\"reason\":\"timeout\",\"timeout_s\":%u",
                             ifGroupMembershipInterval(Dp) / 1000);
                }
                v->leftBy = 0;
                v->expires = 0;
                v->lmqLeft = 0;
                v->olderHost = 0;
                freeHosts(v);
            }
            if (v->expires)
                minDeadline(&next, v->expires);
        }

        syncRoute(r, now);

        if (r->leaveAt && r->leaveAt <= now)
            removeRoute(r);
        else {
            minDeadline(&next, r->leaveAt);
            minDeadline(&next, r->joinRetryAt);
        }
    }
    return next;
}

/**
*   Updates the Kernel routing table. If activate is 1, the route
*   is (re-)activated. If activate is false, the route is removed.
*/
static int internUpdateKernelRoute(struct RouteTable *route, int activate) {
    struct   MRouteDesc mrDesc;
    struct   IfDesc     *Dp;
    unsigned            Ix;
    int i;

    for (i = 0; i < MAX_ORIGINS; i++) {
        if (route->originAddrs[i] == 0 || route->upstrVif == -1)
            continue;

        mrDesc.McAdr.s_addr     = route->group;
        mrDesc.OriginAdr.s_addr = route->originAddrs[i];
        memset( mrDesc.TtlVc, 0, sizeof( mrDesc.TtlVc ) );
        mrDesc.InVif = route->upstrVif;

        for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {
            if(Dp->state == IF_STATE_UPSTREAM)
                continue;
            if(Dp->index < 32 && BIT_TST(route->vifBits, Dp->index))
                mrDesc.TtlVc[ Dp->index ] = Dp->threshold;
        }

        if(activate)
            addMRoute( &mrDesc );
        else
            delMRoute( &mrDesc );
    }

    return 1;
}

/**
*   Debug function that writes the routing table entries to the log.
*/
static void logRouteTable(const char *header) {
    struct RouteTable   *croute;
    unsigned            rcount = 0;

    if (LogLevel < LOG_DEBUG)
        return;

    my_log(LOG_DEBUG, 0, "Current routing table (%s):", header);
    for (croute = routing_table; croute; croute = croute->nextroute, rcount++) {
        my_log(LOG_DEBUG, 0, "#%u: %s src %s, %s, OutVifs 0x%08x",
            rcount, inetFmt(croute->group, s1),
            croute->originAddrs[0] ? inetFmt(croute->originAddrs[0], s2) : "-",
            croute->upstrState == ROUTESTATE_JOINED ? "joined" : "not joined",
            croute->vifBits);
    }
    if (!rcount)
        my_log(LOG_DEBUG, 0, "No routes in table...");
}

/*
 * JSON status
 */
static void jsonStr(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        if ((unsigned char)*s >= 0x20)
            fputc(*s, f);
    }
    fputc('"', f);
}

static long long remainMs(msec_t t, msec_t now) {
    if (!t)
        return -1;
    return t > now ? (long long)(t - now) : 0;
}

void routeTableDumpJson(FILE *f, msec_t now) {
    struct Config *conf = getCommonConfig();
    struct RouteTable *r;
    struct IfDesc *Dp;
    unsigned Ix;
    int i, first;
    char buf[32];

    fprintf(f, "{\"daemon\":\"igmpproxy-plus\",\"version\":\"%s\",\"uptime_ms\":%llu,"
               "\"upstream_join_failures\":%lu,",
            PACKAGE_VERSION, (unsigned long long)(now - startTime), upstreamJoinFailures);

    fprintf(f, "\"config\":{\"robustness\":%u,\"query_interval_ms\":%u,"
               "\"query_response_interval_ms\":%u,\"startup_query_interval_ms\":%u,"
               "\"last_member_query_interval_ms\":%u,\"last_member_query_count\":%u,"
               "\"fastleave\":\"%s\",\"upstream_leave_delay_ms\":%u,\"host_limit\":%u,"
               "\"query_version\":%u},",
            conf->robustnessValue, conf->queryInterval, conf->queryResponseInterval,
            conf->startupQueryInterval, conf->lastMemberQueryInterval,
            conf->lastMemberQueryCount, fastLeaveName(conf->fastLeave),
            conf->upstreamLeaveDelay, conf->hostLimit, conf->queryVersion);

    fputs("\"interfaces\":[", f);
    first = 1;
    for (Ix = 0; (Dp = getIfByIx(Ix)); Ix++) {
        const char *st;
        if (!Dp->InAdr.s_addr || (Dp->Flags & IFF_LOOPBACK))
            continue;
        switch (Dp->state) {
        case IF_STATE_UPSTREAM:   st = "upstream"; break;
        case IF_STATE_DOWNSTREAM: st = "downstream"; break;
        case IF_STATE_DISABLED:   continue;
        default:                  st = "hidden"; break;
        }
        fprintf(f, "%s{\"name\":", first ? "" : ",");
        jsonStr(f, Dp->Name);
        fprintf(f, ",\"address\":\"%s\",\"vif\":%d,\"state\":\"%s\"",
                inetFmt(Dp->InAdr.s_addr, buf), (int)Dp->index, st);
        if (Dp->state == IF_STATE_DOWNSTREAM)
            fprintf(f, ",\"fastleave\":\"%s\",\"last_member_query_interval_ms\":%u,"
                       "\"last_member_query_count\":%u,\"group_membership_interval_ms\":%u,"
                       "\"query_version\":%u",
                    fastLeaveName(ifFastLeave(Dp)), ifLastMemberQueryInterval(Dp),
                    ifLastMemberQueryCount(Dp), ifGroupMembershipInterval(Dp),
                    ifQueryVersion(Dp));
        fputc('}', f);
        first = 0;
    }
    fputs("],\"groups\":[", f);

    for (r = routing_table; r; r = r->nextroute) {
        int j, firstSrc = 1, firstMember = 1;
        struct IfDesc *up = r->upstrVif >= 0 ? getIfByVifIndex(r->upstrVif) : NULL;

        fprintf(f, "%s{\"group\":\"%s\",\"upstream\":\"%s\",\"upstream_if\":",
                r == routing_table ? "" : ",", inetFmt(r->group, buf),
                r->upstrState == ROUTESTATE_JOINED ? "joined" : "not-joined");
        if (up) jsonStr(f, up->Name); else fputs("null", f);
        fprintf(f, ",\"age_ms\":%llu,\"reports\":%lu,\"leaves\":%lu,\"leave_in_ms\":%lld,\"sources\":[",
                (unsigned long long)(now - r->created), r->reports, r->leaves,
                remainMs(r->leaveAt, now));
        {
            // kernel forwarding counters, summed over the sources; clients
            // turn two samples into a rate (Mbit/s per stream)
            uint64_t pk = 0, by = 0, p, b;
            int counted = 0;
            for (j = 0; j < MAX_ORIGINS; j++) {
                if (!r->originAddrs[j])
                    continue;
                fprintf(f, "%s\"%s\"", firstSrc ? "" : ",", inetFmt(r->originAddrs[j], buf));
                firstSrc = 0;
                if (getSGCount(r->originAddrs[j], r->group, &p, &b) == 0) {
                    pk += p;
                    by += b;
                    counted = 1;
                }
            }
            fputs("]", f);
            if (counted)
                fprintf(f, ",\"packets\":%llu,\"bytes\":%llu",
                        (unsigned long long)pk, (unsigned long long)by);
        }
        fputs(",\"members\":[", f);

        for (i = 0; i < MAXVIFS && i < 32; i++) {
            struct VifState *v = &r->vif[i];
            struct HostEntry *h;
            struct IfDesc *Dv;
            int firstHost = 1;

            if (!v->expires && !v->lmqLeft)
                continue;
            Dv = getIfByVifIndex(i);
            fprintf(f, "%s{\"interface\":", firstMember ? "" : ",");
            jsonStr(f, Dv ? Dv->Name : "?");
            fprintf(f, ",\"forwarding\":%s,\"expires_in_ms\":%lld,\"last_member_queries_left\":%u,"
                       "\"hosts_untracked\":%s,\"compat\":\"%s\",\"hosts\":[",
                    v->expires ? "true" : "false", remainMs(v->expires, now), v->lmqLeft,
                    v->overflow ? "true" : "false",
                    (ifQueryVersion(Dv) == 2 || v->olderHost > now) ? "v2" : "v3");
            for (h = v->hosts; h; h = h->next) {
                fprintf(f, "%s{\"address\":\"%s\",\"expires_in_ms\":%lld,\"heard_ms_ago\":%llu}",
                        firstHost ? "" : ",", inetFmt(h->addr, buf), remainMs(h->expires, now),
                        (unsigned long long)(now - h->heard));
                firstHost = 0;
            }
            fputs("]}", f);
            firstMember = 0;
        }
        fputs("]}", f);
    }
    fputs("]}\n", f);
}
