<script>
    $(document).ready(function () {
        function esc(s) {
            return $('<div/>').text(s == null ? '' : String(s)).html();
        }

        function ms(v) {
            if (v == null || v < 0) {
                return '-';
            }
            if (v < 10000) {
                return (v / 1000).toFixed(1) + ' s';
            }
            let s = Math.round(v / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
            s = s % 60;
            return (h ? h + 'h ' : '') + (h || m ? m + 'm ' : '') + s + 's';
        }

        function render(st) {
            if (st.error) {
                $('#status-error').text(st.error).show();
                $('#status-body').hide();
                return;
            }
            $('#status-error').hide();
            $('#status-body').show();

            const c = st.config;
            $('#cfg-fastleave').text(c.fastleave);
            $('#cfg-lmq').text(c.last_member_query_count + ' x ' + c.last_member_query_interval_ms + ' ms');
            $('#cfg-uld').text(c.upstream_leave_delay_ms + ' ms');
            $('#cfg-qi').text(ms(c.query_interval_ms));
            $('#cfg-rob').text(c.robustness);
            $('#cfg-uptime').text(ms(st.uptime_ms));

            let rows = '';
            st.interfaces.forEach(function (i) {
                const ds = i.state === 'downstream';
                rows += '<tr><td>' + esc(i.name) + '</td><td>' + esc(i.address) + '</td><td>' + esc(i.state) + '</td><td>' +
                    (ds ? esc(i.fastleave) : '-') + '</td><td>' +
                    (ds ? i.last_member_query_count + ' x ' + i.last_member_query_interval_ms + ' ms' : '-') + '</td><td>' +
                    (ds ? ms(i.group_membership_interval_ms) : '-') + '</td></tr>';
            });
            $('#tbl-interfaces tbody').html(rows || '<tr><td colspan="6"><em>{{ lang._('No interfaces') }}</em></td></tr>');

            rows = '';
            st.groups.forEach(function (g) {
                let members = '';
                g.members.forEach(function (m) {
                    let state = m.forwarding
                        ? (m.last_member_queries_left > 0 ? '{{ lang._('checking') }} (' + m.last_member_queries_left + ')' : '{{ lang._('forwarding') }}')
                        : '{{ lang._('stopped, querying') }}';
                    let hosts = m.hosts.map(function (h) {
                        return esc(h.address) + ' <small class="text-muted">(' + ms(h.expires_in_ms) + ')</small>';
                    }).join(', ');
                    if (m.hosts_untracked) {
                        hosts += (hosts ? ', ' : '') + '{{ lang._('more, untracked') }}';
                    }
                    members += '<div><b>' + esc(m.interface) + '</b> ' + state + ' &ndash; {{ lang._('expires') }} ' +
                        ms(m.expires_in_ms) + (m.compat === 'v2' ? ' &ndash; {{ lang._('IGMPv2 host present') }}' : '') +
                        (hosts ? '<br/><small>' + hosts + '</small>' : '') + '</div>';
                });
                let up = g.upstream === 'joined'
                    ? (g.leave_in_ms >= 0 ? '{{ lang._('leaving in') }} ' + ms(g.leave_in_ms) : '{{ lang._('joined') }}')
                    : '{{ lang._('not joined') }}';
                rows += '<tr><td><b>' + esc(g.group) + '</b></td><td>' + esc(g.sources.join(', ') || '-') + '</td><td>' + up +
                    '</td><td>' + (members || '<em>{{ lang._('no listeners') }}</em>') + '</td><td>' +
                    g.reports + ' / ' + g.leaves + '</td><td>' + ms(g.age_ms) + '</td></tr>';
            });
            $('#tbl-groups tbody').html(rows || '<tr><td colspan="6"><em>{{ lang._('No active groups') }}</em></td></tr>');
        }

        function refresh() {
            ajaxGet('/api/igmpproxyplus/service/groups', {}, function (data) {
                render(data || {error: 'no data'});
            });
        }

        refresh();
        setInterval(refresh, 2000);
        updateServiceControlUI('igmpproxyplus');
    });
</script>

<div class="content-box">
    <div id="status-error" class="alert alert-warning" style="display:none; margin:10px;"></div>
    <div id="status-body" style="padding: 10px;">
        <h3>{{ lang._('Settings in use') }}</h3>
        <table class="table table-condensed">
            <thead>
            <tr>
                <th>{{ lang._('Fast leave') }}</th>
                <th>{{ lang._('Last member queries') }}</th>
                <th>{{ lang._('Upstream leave delay') }}</th>
                <th>{{ lang._('Query interval') }}</th>
                <th>{{ lang._('Robustness') }}</th>
                <th>{{ lang._('Uptime') }}</th>
            </tr>
            </thead>
            <tbody>
            <tr>
                <td id="cfg-fastleave"></td>
                <td id="cfg-lmq"></td>
                <td id="cfg-uld"></td>
                <td id="cfg-qi"></td>
                <td id="cfg-rob"></td>
                <td id="cfg-uptime"></td>
            </tr>
            </tbody>
        </table>

        <h3>{{ lang._('Interfaces') }}</h3>
        <table id="tbl-interfaces" class="table table-condensed table-striped">
            <thead>
            <tr>
                <th>{{ lang._('Interface') }}</th>
                <th>{{ lang._('Address') }}</th>
                <th>{{ lang._('Role') }}</th>
                <th>{{ lang._('Fast leave') }}</th>
                <th>{{ lang._('Last member queries') }}</th>
                <th>{{ lang._('Membership timeout') }}</th>
            </tr>
            </thead>
            <tbody></tbody>
        </table>

        <h3>{{ lang._('Multicast groups') }}</h3>
        <table id="tbl-groups" class="table table-condensed table-striped">
            <thead>
            <tr>
                <th>{{ lang._('Group') }}</th>
                <th>{{ lang._('Sources') }}</th>
                <th>{{ lang._('Upstream') }}</th>
                <th>{{ lang._('Downstream members') }}</th>
                <th>{{ lang._('Reports / leaves') }}</th>
                <th>{{ lang._('Age') }}</th>
            </tr>
            </thead>
            <tbody></tbody>
        </table>
    </div>
</div>
