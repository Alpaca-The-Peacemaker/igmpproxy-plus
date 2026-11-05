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
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 */

namespace OPNsense\IGMPProxyPlus;

use OPNsense\Base\BaseModel;
use OPNsense\Base\Messages\Message;

class IGMPProxyPlus extends BaseModel
{
    public function performValidation($validateFullModel = false)
    {
        $messages = parent::performValidation($validateFullModel);

        $seen = [];
        $upstream = 0;
        $downstream = 0;
        foreach ($this->interfaces->interface->iterateItems() as $uuid => $intf) {
            if ((string)$intf->enabled != '1') {
                continue;
            }
            $name = (string)$intf->interface;
            if (isset($seen[$name])) {
                $messages->appendMessage(new Message(
                    gettext('Each interface can only be listed once.'),
                    'interface.interface'
                ));
            }
            $seen[$name] = true;
            if ((string)$intf->direction == 'upstream') {
                $upstream++;
            } else {
                $downstream++;
            }
        }

        if ((string)$this->general->enabled == '1' && ($upstream == 0 || $downstream == 0)) {
            $messages->appendMessage(new Message(
                gettext('Enable at least one upstream and one downstream interface on the Interfaces tab.'),
                'general.enabled'
            ));
        }

        return $messages;
    }
}
