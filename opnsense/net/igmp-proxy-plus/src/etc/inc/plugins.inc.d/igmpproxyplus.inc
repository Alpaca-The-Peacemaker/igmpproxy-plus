<?php

/*
 * Copyright (C) 2026 Alpaca_The_Peacemaker
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * IGMP Proxy Plus - service registration and interface change hook.
 */

function igmpproxyplus_enabled()
{
    $mdl = new \OPNsense\IGMPProxyPlus\IGMPProxyPlus();
    return (string)$mdl->general->enabled == '1';
}

function igmpproxyplus_services()
{
    $services = [];

    if (igmpproxyplus_enabled()) {
        $services[] = [
            'description' => gettext('IGMP Proxy Plus'),
            'configd' => [
                'restart' => ['igmpproxyplus restart'],
                'start' => ['igmpproxyplus start'],
                'stop' => ['igmpproxyplus stop'],
            ],
            'name' => 'igmpproxy-plus',
        ];
    }

    return $services;
}

function igmpproxyplus_configure()
{
    return [
        'newwanip' => ['igmpproxyplus_configure_do:2'],
    ];
}

/*
 * The proxy binds to interface addresses at start, so restart it when an
 * address changes (PPPoE / DHCP on the upstream).
 */
function igmpproxyplus_configure_do($verbose = false, $interface = null)
{
    if (!igmpproxyplus_enabled()) {
        return;
    }

    service_log('Restarting IGMP Proxy Plus...', $verbose);
    configd_run('template reload OPNsense/IGMPProxyPlus');
    configd_run('igmpproxyplus restart');
    service_log("done.\n", $verbose);
}
