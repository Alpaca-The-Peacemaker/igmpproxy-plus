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
        my_log(LOG_WARNING, 0, "No interfaces found for source %s",
            inetFmt(src,s1));
        return;
    }

    if(sourceVif->InAdr.s_addr == src) {
        my_log(LOG_DEBUG, 0, "The IGMP message was from myself. Ignoring.");
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
        my_log(LOG_WARNING, 0, "No interfaces found for source %s",
            inetFmt(src,s1));
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
static void sendQuery(struct IfDesc *Dp, uint32_t dst, uint32_t group, unsigned int maxRespMs) {
    struct Config *conf = getCommonConfig();
    unsigned int tenths = (maxRespMs + 99) / 100;

    if (tenths < 1)
        tenths = 1;

    if (ifQueryVersion(Dp) == 3) {
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
void sendGroupSpecificQuery(struct IfDesc *Dp, uint32_t group) {
    unsigned int lmqi = ifLastMemberQueryInterval(Dp);

    if (!Dp->InAdr.s_addr || Dp->state != IF_STATE_DOWNSTREAM)
        return;

    sendQuery(Dp, group, group, lmqi);

    my_log(LOG_DEBUG, 0, "Sent group specific query for %s on %s. Max resp: %ums",
           inetFmt(group, s1), Dp->Name, lmqi);
}

/**
*   Sends a general membership query on downstream VIFs
*/
void sendGeneralMembershipQuery(void) {
    struct  Config  *conf = getCommonConfig();
    struct  IfDesc  *Dp;
    int             Ix;

    for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {
        if ( Dp->InAdr.s_addr && ! (Dp->Flags & IFF_LOOPBACK) &&
             Dp->state == IF_STATE_DOWNSTREAM) {
            sendQuery(Dp, allhosts_group, 0, conf->queryResponseInterval);

            my_log(LOG_DEBUG, 0, "Sent membership query from %s ifIndex %d to %s. Max resp: %ums",
                inetFmt(Dp->InAdr.s_addr,s1), Dp->ifIndex,
                inetFmt(allhosts_group,s2), conf->queryResponseInterval);
        }
    }

    // Install timer for next general query...
    if(conf->startupQueryCount > 1) {
        conf->startupQueryCount--;
        timer_setTimer(conf->startupQueryInterval, (timer_f)sendGeneralMembershipQuery, NULL);
    } else {
        conf->startupQueryCount = 0;
        timer_setTimer(conf->queryInterval, (timer_f)sendGeneralMembershipQuery, NULL);
    }
}
