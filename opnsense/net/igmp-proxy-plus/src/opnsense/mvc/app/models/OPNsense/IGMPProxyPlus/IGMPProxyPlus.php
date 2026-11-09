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
