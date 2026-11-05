<script>
    $(document).ready(function () {
        mapDataToFormUI({'frm_general': "/api/igmpproxyplus/settings/get"}).done(function () {
            formatTokenizersUI();
            $('.selectpicker').selectpicker('refresh');
            updateServiceControlUI('igmpproxyplus');
        });

        $("#grid-interfaces").UIBootgrid({
            search: '/api/igmpproxyplus/settings/search_interface',
            get: '/api/igmpproxyplus/settings/get_interface/',
            set: '/api/igmpproxyplus/settings/set_interface/',
            add: '/api/igmpproxyplus/settings/add_interface/',
            del: '/api/igmpproxyplus/settings/del_interface/',
            toggle: '/api/igmpproxyplus/settings/toggle_interface/'
        });

        $("#reconfigureAct").SimpleActionButton({
            onPreAction: function () {
                const dfObj = new $.Deferred();
                saveFormToEndpoint("/api/igmpproxyplus/settings/set", 'frm_general', function () {
                    dfObj.resolve();
                }, true, function () {
                    dfObj.reject();
                });
                return dfObj;
            },
            onAction: function () {
                updateServiceControlUI('igmpproxyplus');
            }
        });
    });
</script>

<ul class="nav nav-tabs" data-tabs="tabs" id="maintabs">
    <li class="active"><a data-toggle="tab" href="#general">{{ lang._('General') }}</a></li>
    <li><a data-toggle="tab" href="#interfaces">{{ lang._('Interfaces') }}</a></li>
</ul>

<div class="tab-content content-box">
    <div id="general" class="tab-pane fade in active">
        {{ partial("layout_partials/base_form", ['fields': generalForm, 'id': 'frm_general']) }}
    </div>
    <div id="interfaces" class="tab-pane fade in">
        <table id="grid-interfaces" class="table table-condensed table-hover table-striped table-responsive"
               data-editDialog="DialogInterface" data-editAlert="ChangeMessageChangeMessage">
            <thead>
            <tr>
                <th data-column-id="uuid" data-type="string" data-identifier="true" data-visible="false">{{ lang._('ID') }}</th>
                <th data-column-id="enabled" data-width="6em" data-type="string" data-formatter="rowtoggle">{{ lang._('Enabled') }}</th>
                <th data-column-id="interface" data-type="string">{{ lang._('Interface') }}</th>
                <th data-column-id="direction" data-type="string">{{ lang._('Direction') }}</th>
                <th data-column-id="fastleave" data-type="string">{{ lang._('Fast leave') }}</th>
                <th data-column-id="last_member_interval" data-type="string">{{ lang._('LMQ interval (ms)') }}</th>
                <th data-column-id="last_member_count" data-type="string">{{ lang._('LMQ count') }}</th>
                <th data-column-id="description" data-type="string">{{ lang._('Description') }}</th>
                <th data-column-id="commands" data-width="7em" data-formatter="commands" data-sortable="false">{{ lang._('Commands') }}</th>
            </tr>
            </thead>
            <tbody>
            </tbody>
            <tfoot>
            <tr>
                <td></td>
                <td>
                    <button data-action="add" type="button" class="btn btn-xs btn-primary"><span class="fa fa-fw fa-plus"></span></button>
                    <button data-action="deleteSelected" type="button" class="btn btn-xs btn-default"><span class="fa fa-fw fa-trash-o"></span></button>
                </td>
            </tr>
            </tfoot>
        </table>
        <div id="ChangeMessageChangeMessage" class="alert alert-info" style="display: none" role="alert">
            {{ lang._('After changing interfaces, press Apply to restart the proxy.') }}
        </div>
    </div>
</div>

<section class="page-content-main">
    <div class="content-box">
        <div class="col-md-12">
            <br/>
            <button class="btn btn-primary" id="reconfigureAct"
                    data-endpoint="/api/igmpproxyplus/service/reconfigure"
                    data-label="{{ lang._('Apply') }}"
                    data-error-title="{{ lang._('Error reconfiguring IGMP Proxy Plus') }}"
                    type="button">
            </button>
            <br/><br/>
        </div>
    </div>
</section>

{{ partial("layout_partials/base_dialog", ['fields': formDialogInterface, 'id': 'DialogInterface', 'label': lang._('Edit interface')]) }}
