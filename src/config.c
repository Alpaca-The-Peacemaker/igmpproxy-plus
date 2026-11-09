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
*   config.c - Contains functions to load and parse config
*              file, and functions to configure the daemon.
*
*   igmpproxy-plus: configurable querier timers, per interface fast leave
*   and last member query tuning. All times are kept in milliseconds.
*/

#include "igmpproxy.h"

// Structure to keep configuration for VIFs...
struct vifconfig {
    char*               name;
    short               state;
    int                 ratelimit;
    int                 threshold;

    // Per interface tuning (0 / FASTLEAVE_INHERIT = use global)
    int                 fastLeave;
    unsigned int        robustness;
    unsigned int        lastMemberQueryInterval;
    unsigned int        lastMemberQueryCount;
    unsigned int        groupTimeout;
    unsigned int        queryVersion;

    // Keep allowed nets for VIF.
    struct SubnetList*  allowednets;

    // Allowed Groups
    struct SubnetList*  allowedgroups;

    // Next config in list...
    struct vifconfig*   next;
};

// Structure to keep vif configuration
struct vifconfig *vifconf;

// Keeps common settings...
static struct Config commonConfig;

// Track which derived defaults were set explicitly
static bool startupIntervalSet, startupCountSet, lmqCountSet;

// Prototypes...
struct vifconfig *parsePhyintToken(void);
struct SubnetList *parseSubnetAddress(char *addrstr);

/* Limits in ms. The IGMPv2 max response code is 8 bits of 1/10 s => 25.5 s. */
#define LMQI_MIN      100
#define LMQI_MAX    25500
#define QRI_MIN      1000
#define QRI_MAX     25500
#define QI_MIN       2000
#define QI_MAX    3600000
#define GT_MIN       1000
#define GT_MAX    7200000
#define ULD_MAX    300000

/**
*   Initializes common config..
*/
static void initCommonConfig(void) {
    commonConfig.robustnessValue = DEFAULT_ROBUSTNESS;
    commonConfig.queryInterval = INTERVAL_QUERY;
    commonConfig.queryResponseInterval = INTERVAL_QUERY_RESPONSE;

    // Derived from other settings after parsing unless set explicitly.
    commonConfig.startupQueryInterval = 0;
    commonConfig.startupQueryCount = 0;

    // RFC 2236 default of 1 s (stock igmpproxy used 10 s here, which is
    // why leaving a stream took 20+ seconds).
    commonConfig.lastMemberQueryInterval = INTERVAL_LAST_MEMBER_QUERY;
    commonConfig.lastMemberQueryCount    = DEFAULT_ROBUSTNESS;

    commonConfig.fastLeave = FASTLEAVE_OFF;
    commonConfig.upstreamLeaveDelay = 0;
    commonConfig.hostLimit = DEFAULT_HOST_LIMIT;
    commonConfig.queryVersion = 3;
    snprintf(commonConfig.controlSocket, sizeof(commonConfig.controlSocket),
             "%s", "/var/run/igmpproxy.sock");
    commonConfig.eventLog[0] = '\0';
    commonConfig.eventLogSize = 1024 * 1024;
    commonConfig.eventLogReports = true;
    commonConfig.maxMemberships = 1024;

    // aimwang: default value
    commonConfig.defaultInterfaceState = IF_STATE_DISABLED;
    commonConfig.rescanVif = 0;

    startupIntervalSet = startupCountSet = lmqCountSet = false;
}

/**
*   Returns a pointer to the common config...
*/
struct Config *getCommonConfig(void) {
    return &commonConfig;
}

/*
 * Per interface accessors - fall back to the global value.
 */
void ifTuningDefaults(struct IfDesc *Dp) {
    Dp->robustness = 0;
    Dp->fastLeave = FASTLEAVE_INHERIT;
    Dp->lastMemberQueryInterval = 0;
    Dp->lastMemberQueryCount = 0;
    Dp->groupTimeout = 0;
    Dp->queryVersion = 0;
}

unsigned int ifQueryVersion(const struct IfDesc *Dp) {
    if (Dp && Dp->queryVersion)
        return Dp->queryVersion;
    return commonConfig.queryVersion;
}

int ifFastLeave(const struct IfDesc *Dp) {
    if (Dp && Dp->fastLeave != FASTLEAVE_INHERIT)
        return Dp->fastLeave;
    return commonConfig.fastLeave;
}

unsigned int ifLastMemberQueryInterval(const struct IfDesc *Dp) {
    if (Dp && Dp->lastMemberQueryInterval)
        return Dp->lastMemberQueryInterval;
    return commonConfig.lastMemberQueryInterval;
}

unsigned int ifLastMemberQueryCount(const struct IfDesc *Dp) {
    if (Dp && Dp->lastMemberQueryCount)
        return Dp->lastMemberQueryCount;
    return commonConfig.lastMemberQueryCount;
}

/**
 * Group Membership Interval: how long a group stays forwarded on an
 * interface without hearing a report. RFC 2236: robustness * QI + QRI.
 */
unsigned int ifGroupMembershipInterval(const struct IfDesc *Dp) {
    unsigned int rv;
    if (Dp && Dp->groupTimeout)
        return Dp->groupTimeout;
    rv = (Dp && Dp->robustness) ? Dp->robustness : commonConfig.robustnessValue;
    return rv * commonConfig.queryInterval + commonConfig.queryResponseInterval;
}

const char *fastLeaveName(int mode) {
    switch (mode) {
    case FASTLEAVE_ON:   return "on";
    case FASTLEAVE_AUTO: return "auto";
    case FASTLEAVE_OFF:  return "off";
    default:             return "inherit";
    }
}

/*
 * Token helpers. A missing value or a bad number is fatal (my_log with
 * LOG_ERR exits), so a broken config never starts a half configured proxy.
 */
static char *needValue(const char *key) {
    char *token = nextConfigToken();
    if (token == NULL)
        my_log(LOG_ERR, 0, "Config: '%s' needs a value", key);
    return token;
}

static unsigned int parseUInt(const char *key, const char *token,
                              unsigned int min, unsigned int max) {
    char *end;
    long v = strtol(token, &end, 10);
    if (*token == '\0' || *end != '\0' || v < (long)min || v > (long)max)
        my_log(LOG_ERR, 0, "Config: '%s' must be %u..%u (got '%s')",
               key, min, max, token);
    return (unsigned int)v;
}

/**
 * Parses a time value into milliseconds. Accepted forms:
 *   "2" or "2s" (seconds), "0.5" or "1.5s", "500ms", "2m".
 */
static unsigned int parseTime(const char *key, const char *token,
                              unsigned int minMs, unsigned int maxMs) {
    char *end;
    double v = strtod(token, &end);
    double ms;

    if (end == token || v < 0)
        goto bad;
    if (strcmp(end, "ms") == 0)
        ms = v;
    else if (*end == '\0' || strcmp(end, "s") == 0)
        ms = v * 1000.0;
    else if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0)
        ms = v * 60000.0;
    else
        goto bad;

    ms += 0.5;
    if (ms < minMs || ms > (double)maxMs + 0.5)
        goto bad;
    return (unsigned int)ms;

bad:
    my_log(LOG_ERR, 0, "Config: '%s' must be %ums..%ums, e.g. 500ms or 2s (got '%s')",
           key, minMs, maxMs, token);
    return 0;
}

static int parseFastLeave(const char *token) {
    if (!strcmp(token, "off") || !strcmp(token, "0") || !strcmp(token, "disabled"))
        return FASTLEAVE_OFF;
    if (!strcmp(token, "on") || !strcmp(token, "1") || !strcmp(token, "immediate"))
        return FASTLEAVE_ON;
    if (!strcmp(token, "auto") || !strcmp(token, "tracked"))
        return FASTLEAVE_AUTO;
    if (!strcmp(token, "inherit") || !strcmp(token, "default"))
        return FASTLEAVE_INHERIT;
    my_log(LOG_ERR, 0, "Config: fastleave must be off, on, auto or inherit (got '%s')", token);
    return FASTLEAVE_OFF;
}

/**
*   Loads the configuration from file, and stores the config in
*   respective holders...
*/
int loadConfig(char *configFile) {
    struct vifconfig  *tmpPtr;
    struct vifconfig  **currPtr = &vifconf;
    char *token;
    bool seenPhyint = false;

    // Initialize common config
    initCommonConfig();

    // Test config file reader...
    if(!openConfigFile(configFile)) {
        my_log(LOG_ERR, 0, "Unable to open configfile from %s", configFile);
    }

    // Get first token...
    token = nextConfigToken();
    if(token == NULL) {
        my_log(LOG_ERR, 0, "Config file was empty.");
    }

    // Loop until all configuration is read.
    while ( token != NULL ) {
        char key[MAX_TOKEN_LENGTH];
        snprintf(key, sizeof(key), "%s", token);

        if(strcmp("phyint", token)==0) {
            my_log(LOG_DEBUG, 0, "Config: Got a phyint token.");
            seenPhyint = true;
            tmpPtr = parsePhyintToken();
            if(tmpPtr == NULL) {
                closeConfigFile();
                my_log(LOG_WARNING, 0, "Unknown token '%s' in configfile", token);
                return 0;
            }
            my_log(LOG_DEBUG, 0, "IF %s: state %d, fastleave %s",
                   tmpPtr->name, tmpPtr->state, fastLeaveName(tmpPtr->fastLeave));

            // Insert config, and move temppointer to next location...
            *currPtr = tmpPtr;
            currPtr = &tmpPtr->next;

            // Continue with the token that the phyint parser did not recognize.
            token = getCurrentConfigToken();
            continue;
        }

        if (seenPhyint && (
            !strcmp("robustness", token) || !strcmp("lastmemberinterval", token) ||
            !strcmp("lastmembercount", token) || !strcmp("fastleave", token))) {
            // These names are also per interface options; after a phyint
            // they would have been consumed by it, so this cannot happen
            // unless the phyint parser stopped early. Be explicit anyway.
            my_log(LOG_ERR, 0, "Config: global '%s' must appear before the first phyint", token);
        }

        if(strcmp("quickleave", token)==0) {
            // Legacy option: maps to tracked fast leave.
            my_log(LOG_DEBUG, 0, "Config: quickleave -> fastleave auto.");
            commonConfig.fastLeave = FASTLEAVE_AUTO;
        }
        else if(strcmp("hashtablesize", token)==0) {
            // Legacy option: host tracking is exact now.
            needValue(key);
            my_log(LOG_NOTICE, 0, "Config: hashtablesize is obsolete and ignored.");
        }
        else if(strcmp("fastleave", token)==0) {
            int m = parseFastLeave(needValue(key));
            if (m == FASTLEAVE_INHERIT)
                my_log(LOG_ERR, 0, "Config: global fastleave must be off, on or auto");
            commonConfig.fastLeave = m;
        }
        else if(strcmp("robustness", token)==0) {
            commonConfig.robustnessValue = parseUInt(key, needValue(key), 1, 7);
        }
        else if(strcmp("queryinterval", token)==0) {
            commonConfig.queryInterval = parseTime(key, needValue(key), QI_MIN, QI_MAX);
        }
        else if(strcmp("queryresponseinterval", token)==0) {
            commonConfig.queryResponseInterval = parseTime(key, needValue(key), QRI_MIN, QRI_MAX);
        }
        else if(strcmp("startupqueryinterval", token)==0) {
            commonConfig.startupQueryInterval = parseTime(key, needValue(key), QRI_MIN, QI_MAX);
            startupIntervalSet = true;
        }
        else if(strcmp("startupquerycount", token)==0) {
            commonConfig.startupQueryCount = parseUInt(key, needValue(key), 0, 10);
            startupCountSet = true;
        }
        else if(strcmp("lastmemberinterval", token)==0) {
            commonConfig.lastMemberQueryInterval = parseTime(key, needValue(key), LMQI_MIN, LMQI_MAX);
        }
        else if(strcmp("lastmembercount", token)==0) {
            commonConfig.lastMemberQueryCount = parseUInt(key, needValue(key), 1, 10);
            lmqCountSet = true;
        }
        else if(strcmp("upstreamleavedelay", token)==0) {
            commonConfig.upstreamLeaveDelay = parseTime(key, needValue(key), 0, ULD_MAX);
        }
        else if(strcmp("queryversion", token)==0) {
            commonConfig.queryVersion = parseUInt(key, needValue(key), 2, 3);
        }
        else if(strcmp("hostlimit", token)==0) {
            commonConfig.hostLimit = parseUInt(key, needValue(key), 0, 65535);
        }
        else if(strcmp("eventlog", token)==0) {
            token = needValue("eventlog");
            if (!strcmp(token, "none") || !strcmp(token, "off"))
                commonConfig.eventLog[0] = '\0';
            else if (snprintf(commonConfig.eventLog, sizeof(commonConfig.eventLog),
                              "%s", token) >= (int)sizeof(commonConfig.eventLog))
                my_log(LOG_ERR, 0, "Config: eventlog path too long");
        }
        else if(strcmp("eventlogsize", token)==0) {
            // KiB; 0 = never rotate
            commonConfig.eventLogSize = parseUInt(key, needValue(key), 0, 1024 * 1024) * 1024;
        }
        else if(strcmp("maxmemberships", token)==0) {
            commonConfig.maxMemberships = parseUInt(key, needValue(key), 0, 65535);
        }
        else if(strcmp("eventlogreports", token)==0) {
            token = needValue("eventlogreports");
            commonConfig.eventLogReports = !(!strcmp(token, "off") || !strcmp(token, "0") || !strcmp(token, "no"));
        }
        else if(strcmp("controlsocket", token)==0) {
            token = needValue("controlsocket");
            if (!strcmp(token, "none") || !strcmp(token, "off"))
                commonConfig.controlSocket[0] = '\0';
            else if (snprintf(commonConfig.controlSocket, sizeof(commonConfig.controlSocket),
                              "%s", token) >= (int)sizeof(commonConfig.controlSocket))
                my_log(LOG_ERR, 0, "Config: controlsocket path too long");
        }
        else if(strcmp("defaultdown", token)==0) {
            my_log(LOG_DEBUG, 0, "Config: interface Default as down stream.");
            commonConfig.defaultInterfaceState = IF_STATE_DOWNSTREAM;
        }
        else if(strcmp("rescanvif", token)==0) {
            my_log(LOG_DEBUG, 0, "Config: Need detect new interface.");
            commonConfig.rescanVif = 1;
        }
        else if(strcmp("chroot", token)==0) {
            token = needValue("chroot");
            if (snprintf(commonConfig.chroot, sizeof(commonConfig.chroot), "%s",
              token) >= (int)sizeof(commonConfig.chroot))
                my_log(LOG_ERR, 0, "Config: chroot is truncated");
        }
        else if(strcmp("user", token)==0) {
            token = needValue("user");
            if (snprintf(commonConfig.user, sizeof(commonConfig.user), "%s",
              token) >= (int)sizeof(commonConfig.user))
                my_log(LOG_ERR, 0, "Config: user is truncated");
        } else {
            closeConfigFile();
            my_log(LOG_WARNING, 0, "Unknown token '%s' in configfile", token);
            return 0;
        }
        token = nextConfigToken();
    }

    // Close the configfile...
    closeConfigFile();

    // Derived defaults (RFC 2236 section 8)
    if (!startupIntervalSet)
        commonConfig.startupQueryInterval = commonConfig.queryInterval / 4;
    if (!startupCountSet)
        commonConfig.startupQueryCount = commonConfig.robustnessValue;
    if (!lmqCountSet)
        commonConfig.lastMemberQueryCount = commonConfig.robustnessValue;

    if (commonConfig.queryResponseInterval >= commonConfig.queryInterval) {
        my_log(LOG_ERR, 0, "Config: queryresponseinterval (%ums) must be smaller than queryinterval (%ums)",
               commonConfig.queryResponseInterval, commonConfig.queryInterval);
    }

    my_log(LOG_INFO, 0, "Config: robustness %u, query %ums/%ums, last member %ux%ums, "
           "fastleave %s, upstream leave delay %ums, IGMPv%u queries",
           commonConfig.robustnessValue, commonConfig.queryInterval,
           commonConfig.queryResponseInterval, commonConfig.lastMemberQueryCount,
           commonConfig.lastMemberQueryInterval, fastLeaveName(commonConfig.fastLeave),
           commonConfig.upstreamLeaveDelay, commonConfig.queryVersion);

    return 1;
}

/**
*   Appends extra VIF configuration from config file.
*/
void configureVifs(void) {
    unsigned Ix;
    struct IfDesc *Dp;
    struct vifconfig *confPtr;

    // If no config is available, just return...
    if(vifconf == NULL) {
        return;
    }

    // Loop through all VIFs...
    for ( Ix = 0; (Dp = getIfByIx(Ix)); Ix++ ) {
        if ( Dp->InAdr.s_addr && ! (Dp->Flags & IFF_LOOPBACK) ) {

            // Now try to find a matching config...
            for( confPtr = vifconf; confPtr; confPtr = confPtr->next) {

                if(strcmp(Dp->Name, confPtr->name)==0) {
                    struct SubnetList *vifLast;

                    my_log(LOG_DEBUG, 0, "Found config for %s", Dp->Name);

                    Dp->state = confPtr->state;
                    Dp->threshold = confPtr->threshold;
                    Dp->ratelimit = confPtr->ratelimit;

                    Dp->fastLeave = confPtr->fastLeave;
                    Dp->robustness = confPtr->robustness;
                    Dp->lastMemberQueryInterval = confPtr->lastMemberQueryInterval;
                    Dp->lastMemberQueryCount = confPtr->lastMemberQueryCount;
                    Dp->groupTimeout = confPtr->groupTimeout;
                    Dp->queryVersion = confPtr->queryVersion;

                    // Go to last allowed net on VIF...
                    for(vifLast = Dp->allowednets; vifLast->next; vifLast = vifLast->next);

                    // Insert the configured nets...
                    vifLast->next = confPtr->allowednets;

                    Dp->allowedgroups = confPtr->allowedgroups;

                    break;
                }
            }
        }
    }
}


/**
*   Internal function to parse phyint config
*/
struct vifconfig *parsePhyintToken(void) {
    struct vifconfig  *tmpPtr;
    struct SubnetList **anetPtr, **agrpPtr;
    char *token;
    short parseError = 0;

    // First token should be the interface name....
    token = nextConfigToken();

    // Sanitycheck the name...
    if(token == NULL) return NULL;
    if(strlen(token) >= IF_NAMESIZE) return NULL;
    my_log(LOG_DEBUG, 0, "Config: IF: Config for interface %s.", token);

    tmpPtr = (struct vifconfig*)calloc(1, sizeof(struct vifconfig));
    if(tmpPtr == NULL) {
        my_log(LOG_ERR, 0, "Out of memory.");
    }

    // Set default values...
    tmpPtr->threshold = 1;
    tmpPtr->state = commonConfig.defaultInterfaceState;
    tmpPtr->fastLeave = FASTLEAVE_INHERIT;

    tmpPtr->name = strdup( token );
    if(tmpPtr->name == NULL) {
        my_log(LOG_ERR, 0, "Out of memory.");
    }

    anetPtr = &tmpPtr->allowednets;
    agrpPtr = &tmpPtr->allowedgroups;

    // Parse the rest of the config..
    token = nextConfigToken();
    while(token != NULL) {
        char key[MAX_TOKEN_LENGTH];
        snprintf(key, sizeof(key), "%s", token);

        if(strcmp("altnet", token)==0) {
            token = needValue("altnet");
            *anetPtr = parseSubnetAddress(token);
            if(*anetPtr == NULL) {
                parseError = 1;
                my_log(LOG_WARNING, 0, "Unable to parse subnet address.");
                break;
            }
            anetPtr = &(*anetPtr)->next;
        }
        else if(strcmp("whitelist", token)==0 || strcmp("blacklist", token)==0) {
            bool allow = strcmp("whitelist", token)==0;
            token = needValue(allow ? "whitelist" : "blacklist");
            *agrpPtr = parseSubnetAddress(token);
            if(*agrpPtr == NULL) {
                my_log(LOG_ERR, 0, "Unable to parse subnet address.");
            }
            (*agrpPtr)->allow = allow;
            agrpPtr = &(*agrpPtr)->next;
        }
        else if(strcmp("upstream", token)==0) {
            tmpPtr->state = IF_STATE_UPSTREAM;
        }
        else if(strcmp("downstream", token)==0) {
            tmpPtr->state = IF_STATE_DOWNSTREAM;
        }
        else if(strcmp("disabled", token)==0) {
            tmpPtr->state = IF_STATE_DISABLED;
        }
        else if(strcmp("ratelimit", token)==0) {
            tmpPtr->ratelimit = (int)parseUInt(key, needValue(key), 0, 1000000);
        }
        else if(strcmp("threshold", token)==0) {
            tmpPtr->threshold = (int)parseUInt(key, needValue(key), 1, 255);
        }
        else if(strcmp("fastleave", token)==0) {
            tmpPtr->fastLeave = parseFastLeave(needValue(key));
        }
        else if(strcmp("robustness", token)==0) {
            tmpPtr->robustness = parseUInt(key, needValue(key), 1, 7);
        }
        else if(strcmp("lastmemberinterval", token)==0) {
            tmpPtr->lastMemberQueryInterval = parseTime(key, needValue(key), LMQI_MIN, LMQI_MAX);
        }
        else if(strcmp("lastmembercount", token)==0) {
            tmpPtr->lastMemberQueryCount = parseUInt(key, needValue(key), 1, 10);
        }
        else if(strcmp("queryversion", token)==0) {
            tmpPtr->queryVersion = parseUInt(key, needValue(key), 2, 3);
        }
        else if(strcmp("grouptimeout", token)==0) {
            tmpPtr->groupTimeout = parseTime(key, needValue(key), GT_MIN, GT_MAX);
        }
        else {
            // Unknown token. Hand it back to the global parser.
            break;
        }
        token = nextConfigToken();
    }

    if(parseError) {
        free(tmpPtr->name);
        free(tmpPtr);
        tmpPtr = NULL;
    }

    return tmpPtr;
}

/**
*   Parses a subnet address string on the format
*   a.b.c.d/n into a SubnetList entry.
*/
struct SubnetList *parseSubnetAddress(char *addrstr) {
    struct SubnetList   *tmpSubnet;
    char                *tmpStr;
    uint32_t            addr = 0x00000000;
    uint32_t            mask = 0xFFFFFFFF;

    // First get the network part of the address...
    tmpStr = strtok(addrstr, "/");
    addr = inet_addr(tmpStr);

    tmpStr = strtok(NULL, "/");
    if(tmpStr != NULL) {
        int bitcnt = atoi(tmpStr);
        if(bitcnt < 0 || bitcnt > 32) {
            my_log(LOG_WARNING, 0, "The bits part of the address is invalid : %s.", tmpStr);
            return NULL;
        }

        if (bitcnt == 0)
            mask = 0;
        else
            mask <<= (32 - bitcnt);
    }

    if(addr == (uint32_t)-1) {
        my_log(LOG_WARNING, 0, "Unable to parse address token '%s'.", addrstr);
        return NULL;
    }

    tmpSubnet = (struct SubnetList*) malloc(sizeof(struct SubnetList));
    if (tmpSubnet == NULL)
        my_log(LOG_ERR, 0, "Out of memory.");
    tmpSubnet->subnet_addr = addr;
    tmpSubnet->subnet_mask = ntohl(mask);
    tmpSubnet->allow = false;
    tmpSubnet->next = NULL;

    my_log(LOG_DEBUG, 0, "Config: IF: Altnet: Parsed altnet to %s.",
            inetFmts(tmpSubnet->subnet_addr, tmpSubnet->subnet_mask,s1));

    return tmpSubnet;
}
