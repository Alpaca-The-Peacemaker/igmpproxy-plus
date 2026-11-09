#!/usr/local/bin/php
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
 * Import the settings of the stock IGMP Proxy plugin (os-igmp-proxy) into
 * IGMP Proxy Plus. The stock settings in config.xml are left untouched, so
 * going back to the stock plugin picks them up again.
 *
 * usage: import_stock.php [--force]   (--force imports even if IGMP Proxy Plus
 *                                      already has interfaces configured)
 */

require_once 'script/load_phalcon.php';

use OPNsense\Core\Config;
use OPNsense\IGMPProxyPlus\IGMPProxyPlus;

$force = in_array('--force', $argv);
$cfg = Config::getInstance()->object();

if (!isset($cfg->igmpproxy) || !isset($cfg->igmpproxy->igmpentry)) {
    echo "no stock IGMP Proxy settings found, nothing to import\n";
    exit(0);
}

$mdl = new IGMPProxyPlus();
$existing = 0;
foreach ($mdl->interfaces->interface->iterateItems() as $item) {
    $existing++;
}
if ($existing > 0 && !$force) {
    echo "IGMP Proxy Plus already has $existing interface(s), not importing (use --force)\n";
    exit(0);
}

$count = 0;
foreach ($cfg->igmpproxy->igmpentry as $entry) {
    $ifname = trim((string)$entry->ifname);
    if ($ifname === '') {
        continue;
    }
    $node = $mdl->interfaces->interface->Add();
    $node->enabled = '1';
    $node->interface = $ifname;
    $node->direction = ((string)$entry->type === 'upstream') ? 'upstream' : 'downstream';
    $threshold = trim((string)$entry->threshold);
    if ($threshold !== '' && ctype_digit($threshold)) {
        $node->threshold = $threshold;
    }
    $nets = [];
    foreach (preg_split('/[\s,]+/', trim((string)$entry->address)) as $net) {
        if ($net === '') {
            continue;
        }
        $nets[] = strpos($net, '/') === false ? $net . '/32' : $net;
    }
    if (!empty($nets)) {
        $node->altnet = implode(',', $nets);
    }
    $descr = trim((string)$entry->descr);
    $node->description = $descr !== '' ? $descr : 'imported from IGMP Proxy';
    $count++;
}

$mdl->general->enabled = '1';
// The stock plugin always runs with "quickleave"; auto is the equivalent.
$mdl->general->fastleave = 'auto';

$messages = $mdl->performValidation();
if ($messages->count() > 0) {
    foreach ($messages as $msg) {
        echo "validation error: " . $msg->getField() . ": " . $msg->getMessage() . "\n";
    }
    exit(1);
}

$mdl->serializeToConfig();
Config::getInstance()->save();
echo "imported $count interface(s) from the stock IGMP Proxy\n";
