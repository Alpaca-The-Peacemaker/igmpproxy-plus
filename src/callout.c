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


#include "igmpproxy.h"

/*
 * igmpproxy-plus: callout queue keyed on absolute monotonic milliseconds.
 * Only a handful of timers are ever queued (general queries), the
 * per-group membership timers live in the route table.
 */
struct timeOutQueue {
    struct timeOutQueue    *next;
    int                     id;
    timer_f                 func;
    void                   *data;
    msec_t                  when;
};

static int id = 0;
static struct timeOutQueue *queue = NULL;

void callout_init(void) {
    queue = NULL;
}

void free_all_callouts(void) {
    struct timeOutQueue *p;
    while (queue) {
        p = queue;
        queue = queue->next;
        free(p);
    }
}

/**
 * Run every callout whose deadline has passed.
 */
void callout_run(msec_t now) {
    while (queue && queue->when <= now) {
        struct timeOutQueue *p = queue;
        queue = p->next;
        my_log(LOG_DEBUG, 0, "About to call timeout %d", p->id);
        if (p->func)
            p->func(p->data);
        free(p);
    }
}

/**
 * Milliseconds until the next callout is due, or -1 if none is pending.
 */
int timer_nextTimer(msec_t now) {
    if (!queue)
        return -1;
    if (queue->when <= now)
        return 0;
    return (int)(queue->when - now);
}

/**
 * Inserts a timer firing in 'delay' milliseconds.
 */
int timer_setTimer(int delay, timer_f action, void *data) {
    struct timeOutQueue *node, **pp;

    node = (struct timeOutQueue *)malloc(sizeof(*node));
    if (node == NULL) {
        my_log(LOG_WARNING, 0, "Malloc Failed in timer_setTimer");
        return -1;
    }
    node->func = action;
    node->data = data;
    node->when = now_ms() + (delay > 0 ? (msec_t)delay : 0);
    node->id   = ++id;

    for (pp = &queue; *pp && (*pp)->when <= node->when; pp = &(*pp)->next)
        ;
    node->next = *pp;
    *pp = node;

    my_log(LOG_DEBUG, 0, "Created timeout %d - delay %d ms", node->id, delay);
    return node->id;
}
