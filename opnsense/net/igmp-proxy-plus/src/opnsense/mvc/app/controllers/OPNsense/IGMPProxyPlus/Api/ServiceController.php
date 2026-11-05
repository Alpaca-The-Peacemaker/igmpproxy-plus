<?php

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
        if (!is_array($data)) {
            return ['error' => gettext('IGMP Proxy Plus is not running.')];
        }
        return $data;
    }
}
