<?php

namespace OPNsense\IGMPProxyPlus;

class IndexController extends \OPNsense\Base\IndexController
{
    public function indexAction()
    {
        $this->view->generalForm = $this->getForm('general');
        $this->view->formDialogInterface = $this->getForm('dialogInterface');
        $this->view->pick('OPNsense/IGMPProxyPlus/index');
    }

    public function statusAction()
    {
        $this->view->pick('OPNsense/IGMPProxyPlus/status');
    }
}
