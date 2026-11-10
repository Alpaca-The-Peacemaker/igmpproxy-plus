{#
 # Copyright (C) 2026 Alpaca_The_Peacemaker
 # All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions are met:
 #
 # 1. Redistributions of source code must retain the above copyright notice,
 #    this list of conditions and the following disclaimer.
 #
 # 2. Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #
 # THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 # INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 # AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 # AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 # OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 # SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 # INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 # CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 # ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 # POSSIBILITY OF SUCH DAMAGE.
 #}

<script>
    $(document).ready(function () {
        function esc(s) {
            return $('<div/>').text(s == null ? '' : String(s)).html();
        }

        let all = [];

        function draw() {
            const f = $('#ev-filter').val().toLowerCase();
            const routine = $('#ev-routine').is(':checked');
            let rows = '';
            all.forEach(function (e) {
                if (!routine && (e.ev === 'report' || e.ev === 'query')) {
                    return;
                }
                const rest = {};
                Object.keys(e).forEach(function (k) {
                    if (['ts', 'ev', 'group', 'if', 'host'].indexOf(k) < 0) {
                        rest[k] = e[k];
                    }
                });
                const details = Object.keys(rest).length ? JSON.stringify(rest) : '';
                const line = [e.ts, e.ev, e.group, e['if'], e.host, details].join(' ').toLowerCase();
                if (f && line.indexOf(f) < 0) {
                    return;
                }
                rows += '<tr><td>' + esc(new Date(e.ts).toLocaleString()) + '</td><td><b>' + esc(e.ev) + '</b></td><td>' +
                    esc(e.group || '') + '</td><td>' + esc(e['if'] || '') + '</td><td>' + esc(e.host || '') +
                    '</td><td><small>' + esc(details) + '</small></td></tr>';
            });
            $('#tbl-events tbody').html(rows || '<tr><td colspan="6"><em>{{ lang._('No events') }}</em></td></tr>');
        }

        function refresh() {
            ajaxGet('/api/igmpproxyplus/service/events', {}, function (data) {
                all = (data && data.events) || [];
                draw();
            });
        }

        $('#ev-filter').on('input', draw);
        $('#ev-routine').on('change', draw);
        $('#ev-refresh').on('click', refresh);
        refresh();
        updateServiceControlUI('igmpproxyplus');
    });
</script>

<div class="content-box">
    <div style="padding: 10px;">
        <p>{{ lang._('The last 1000 joins, leaves, stream starts and stops, timeouts and queries, newest first. The complete log is the JSON Lines file /var/log/igmpproxy-plus-events.jsonl.') }}</p>
        <div class="form-inline" style="margin-bottom: 10px;">
            <input id="ev-filter" type="text" class="form-control" placeholder="{{ lang._('Filter: device, group, event...') }}"/>
            <label style="margin: 0 10px;"><input id="ev-routine" type="checkbox"/> {{ lang._('Show routine reports and queries') }}</label>
            <button id="ev-refresh" class="btn btn-default">{{ lang._('Reload') }}</button>
        </div>
        <table id="tbl-events" class="table table-condensed table-striped">
            <thead>
            <tr>
                <th>{{ lang._('Time') }}</th>
                <th>{{ lang._('Event') }}</th>
                <th>{{ lang._('Group') }}</th>
                <th>{{ lang._('Interface') }}</th>
                <th>{{ lang._('Device') }}</th>
                <th>{{ lang._('Details') }}</th>
            </tr>
            </thead>
            <tbody></tbody>
        </table>
    </div>
</div>
