{% if not helpers.empty('OPNsense.igmpproxyplus.general.enabled') %}
{%   set g = OPNsense.igmpproxyplus.general %}
igmpproxy_plus_enable="YES"
igmpproxy_plus_flags="{% if g.verbose == '2' %}-v{% elif g.verbose == '3' %}-v -v{% endif %}"
igmpproxy_plus_igmp_version="{{ g.igmp_version|default('') }}"
{% else %}
igmpproxy_plus_enable="NO"
{% endif %}
