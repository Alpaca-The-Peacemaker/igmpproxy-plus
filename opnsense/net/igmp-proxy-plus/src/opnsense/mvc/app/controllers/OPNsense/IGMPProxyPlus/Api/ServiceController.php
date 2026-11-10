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

namespace OPNsense\IGMPProxyPlus\Api;

use OPNsense\Base\ApiMutableServiceControllerBase;
use OPNsense\Core\Backend;

class ServiceController extends ApiMutableServiceControllerBase
{
    protected static $internalServiceClass = '\OPNsense\IGMPProxyPlus\IGMPProxyPlus';
    protected static $internalServiceTemplate = 'OPNsense/IGMPProxyPlus';
    protected static $internalServiceEnabled = 'general.enabled';
    protected static $internalServiceName = 'igmpproxyplus';

    /**
     * Live group table from the daemon's status socket.
     */
    public function groupsAction()
    {
        $response = (new Backend())->configdRun('igmpproxyplus groups');
        $data = json_decode((string)$response, true);
        if (!is_array($data) || isset($data['error'])) {
            // say why: the last lines the daemon or its rc script logged
            $log = [];
            foreach (explode("\n", (string)(new Backend())->configdRun('igmpproxyplus lastlog')) as $line) {
                $line = trim(preg_replace('/^.*\] /', '', $line));
                if ($line !== '') {
                    $log[] = $line;
                }
            }
            return ['error' => gettext('IGMP Proxy Plus is not running.'), 'log' => $log];
        }
        return $data;
    }

    /**
     * Send a general query now, so every device reports what it receives.
     */
    public function refreshAction()
    {
        if (!$this->request->isPost()) {
            return ['error' => 'POST required'];
        }
        $data = json_decode((string)(new Backend())->configdRun('igmpproxyplus refresh'), true);
        return is_array($data) ? $data : ['error' => gettext('IGMP Proxy Plus is not running.')];
    }

    /**
     * The last events of the daemon's event log, newest first.
     */
    public function eventsAction()
    {
        $response = (string)(new Backend())->configdRun('igmpproxyplus events');
        $events = [];
        foreach (explode("\n", $response) as $line) {
            $e = json_decode($line, true);
            if (is_array($e) && isset($e['ev'])) {
                $events[] = $e;
            }
        }
        return ['events' => array_reverse($events)];
    }
}
