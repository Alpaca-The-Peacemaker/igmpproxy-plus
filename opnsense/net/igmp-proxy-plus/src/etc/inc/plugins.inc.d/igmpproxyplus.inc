<?php

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
