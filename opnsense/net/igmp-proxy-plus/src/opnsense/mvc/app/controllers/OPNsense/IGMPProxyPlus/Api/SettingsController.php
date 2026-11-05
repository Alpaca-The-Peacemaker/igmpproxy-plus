<?php

namespace OPNsense\IGMPProxyPlus\Api;

use OPNsense\Base\ApiMutableModelControllerBase;

class SettingsController extends ApiMutableModelControllerBase
{
    protected static $internalModelName = 'igmpproxyplus';
    protected static $internalModelClass = 'OPNsense\IGMPProxyPlus\IGMPProxyPlus';

    public function searchInterfaceAction()
    {
        return $this->searchBase(
            'interfaces.interface',
            ['enabled', 'interface', 'direction', 'fastleave', 'last_member_interval', 'last_member_count', 'description'],
            'interface'
        );
    }

    public function getInterfaceAction($uuid = null)
    {
        return $this->getBase('interface', 'interfaces.interface', $uuid);
    }

    public function addInterfaceAction()
    {
        return $this->addBase('interface', 'interfaces.interface');
    }

    public function setInterfaceAction($uuid)
    {
        return $this->setBase('interface', 'interfaces.interface', $uuid);
    }

    public function delInterfaceAction($uuid)
    {
        return $this->delBase('interfaces.interface', $uuid);
    }

    public function toggleInterfaceAction($uuid, $enabled = null)
    {
        return $this->toggleBase('interfaces.interface', $uuid, $enabled);
    }
}
