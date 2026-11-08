#!/usr/local/bin/php
<?php

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
