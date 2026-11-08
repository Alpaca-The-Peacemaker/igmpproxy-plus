#!/bin/sh
# IGMP Proxy Plus installer for OPNsense (26.7 on FreeBSD 15, 25.7/26.1 on FreeBSD 14).
#
#   sh install-opnsense.sh             back up, then install or upgrade IGMP Proxy Plus
#   sh install-opnsense.sh rollback    undo the last install/upgrade
#   sh install-opnsense.sh uninstall   go back to the stock IGMP Proxy plugin
#   sh install-opnsense.sh backup      only save a backup
#   sh install-opnsense.sh status      show what is installed and the saved backup
#
# Every install first saves /conf/config.xml and the list of installed IGMP
# proxy packages to /conf/igmpproxy-plus-backup. Settings of the stock plugin
# are imported once and left in place, so the stock plugin finds them again
# after a rollback. OPNsense also keeps its own configuration history under
# System > Configuration > History.
# Packages: https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus/releases

set -e

REPO=Alpaca-The-Peacemaker/igmpproxy-plus
RELEASES=https://github.com/$REPO/releases
URL=${IGMPPROXY_PLUS_URL:-$RELEASES/latest/download}
TMP=/tmp/igmpproxy-plus
BACKUP=/conf/igmpproxy-plus-backup
SELF=/usr/local/sbin/igmpproxy-plus-install
IMPORT=/usr/local/opnsense/scripts/OPNsense/IGMPProxyPlus/import_stock.php
ACTION=${1:-install}

say() { echo "igmpproxy-plus: $*"; }
die() { echo "igmpproxy-plus: error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run this as root"
[ -f /usr/local/opnsense/version/core ] || die "this does not look like OPNsense"

ABI=$(pkg config abi)                         # FreeBSD:15:amd64
DAEMON_PKG=igmpproxy-plus-$(echo "$ABI" | tr ':' '-').pkg
PLUGIN_PKG=os-igmp-proxy-plus.pkg

installed() { pkg info -e "$1" 2>/dev/null; }
version() { pkg query '%v' "$1" 2>/dev/null || true; }
running() { pgrep -qx igmpproxy-plus; }

fetch_pkg() {
	mkdir -p "$TMP"
	fetch -q -o "$TMP/$2" "$1/$2" || { rm -f "$TMP/$2"; return 1; }
}

stop_stock() {
	if installed os-igmp-proxy; then
		pkill -x igmpproxy 2>/dev/null || true
	fi
}

start_plus() {
	/usr/local/sbin/configctl template reload OPNsense/IGMPProxyPlus >/dev/null 2>&1 || true
	/usr/local/sbin/configctl igmpproxyplus restart >/dev/null 2>&1 || true
}

# --- backup -------------------------------------------------------------------

save_backup() {
	mkdir -p "$BACKUP"
	cp /conf/config.xml "$BACKUP/config.xml"
	{
		installed os-igmp-proxy && echo "stock $(version os-igmp-proxy)"
		installed os-igmp-proxy-plus && echo "plus $(version igmpproxy-plus) $(version os-igmp-proxy-plus)"
		true
	} > "$BACKUP/previous"
	[ -s "$BACKUP/previous" ] || echo "none" > "$BACKUP/previous"
	date > "$BACKUP/date"
	if [ -f "$0" ] && [ "$0" != "$SELF" ]; then
		cp "$0" "$SELF" && chmod 755 "$SELF"
	fi
	say "backup saved to $BACKUP (previous: $(head -n1 "$BACKUP/previous"))"
}

# --- rollback ---------------------------------------------------------------

do_rollback() {
	[ -f "$BACKUP/previous" ] || die "no backup found in $BACKUP"
	set -- $(head -n1 "$BACKUP/previous")
	kind=$1
	say "rolling back to: $* (backup from $(cat "$BACKUP/date" 2>/dev/null))"

	case "$kind" in
	plus)
		tag=v$2
		if fetch_pkg "$RELEASES/download/$tag" "$DAEMON_PKG" && fetch_pkg "$RELEASES/download/$tag" "$PLUGIN_PKG"; then
			pkg add -f "$TMP/$DAEMON_PKG" && pkg add -f "$TMP/$PLUGIN_PKG"
		else
			say "release $tag is not on GitHub, keeping the installed version"
		fi
		start_plus
		;;
	stock|none)
		pkill -x igmpproxy-plus 2>/dev/null || true
		installed os-igmp-proxy-plus && pkg delete -y os-igmp-proxy-plus
		installed igmpproxy-plus && pkg delete -y igmpproxy-plus
		if [ "$kind" = stock ]; then
			pkg install -y os-igmp-proxy || die "could not reinstall os-igmp-proxy (no internet?). Install it from System > Firmware > Plugins."
			/usr/local/etc/rc.configure_plugins POST_INSTALL >/dev/null 2>&1 || true
			/usr/local/sbin/pluginctl -s igmpproxy restart >/dev/null 2>&1 || true
		fi
		;;
	*)
		die "unknown backup state '$kind'"
		;;
	esac
	rm -rf "$TMP"
	say "rollback done. The full configuration from before is also in $BACKUP/config.xml"
}

# --- install ------------------------------------------------------------------

do_install() {
	say "OPNsense $(cat /usr/local/opnsense/version/core 2>/dev/null | sed -n 's/.*"product_version": *"\([^"]*\)".*/\1/p'), $ABI"
	say "downloading $DAEMON_PKG"
	fetch_pkg "$URL" "$DAEMON_PKG" || die "no package for $ABI. Supported: OPNsense 26.7 (FreeBSD 15) and 25.7/26.1 (FreeBSD 14)."
	say "downloading $PLUGIN_PKG"
	fetch_pkg "$URL" "$PLUGIN_PKG" || die "could not download $PLUGIN_PKG"

	save_backup
	FIRST=0
	installed os-igmp-proxy-plus || FIRST=1

	if installed os-igmp-proxy; then
		say "removing the stock IGMP Proxy plugin (its settings stay in config.xml)"
		stop_stock
		pkg delete -y os-igmp-proxy
	fi

	say "installing"
	if ! { pkg add -f "$TMP/$DAEMON_PKG" && pkg add -f "$TMP/$PLUGIN_PKG"; }; then
		say "installation failed, rolling back"
		do_rollback
		exit 1
	fi

	if [ "$FIRST" = 1 ]; then
		/usr/local/bin/php "$IMPORT" || say "could not import the stock settings, configure Services > IGMP Proxy Plus by hand"
	fi
	start_plus

	if grep -q '<igmpproxyplus>' /conf/config.xml && \
	   sed -n '/<igmpproxyplus>/,/<\/igmpproxyplus>/p' /conf/config.xml | grep -q '<enabled>1</enabled>'; then
		sleep 2
		if ! running; then
			say "IGMP Proxy Plus did not start, rolling back. Log: clog /var/log/system/latest.log | grep igmpproxy"
			do_rollback
			exit 1
		fi
		say "running. Settings and status: Services > IGMP Proxy Plus"
	else
		say "installed. Enable it under Services > IGMP Proxy Plus and add your interfaces"
	fi
	rm -rf "$TMP"
	say "installed igmpproxy-plus $(version igmpproxy-plus), plugin $(version os-igmp-proxy-plus)"
	say "to undo: igmpproxy-plus-install rollback"
}

do_uninstall() {
	installed os-igmp-proxy-plus || installed igmpproxy-plus || die "IGMP Proxy Plus is not installed"
	save_backup
	echo "stock" > "$BACKUP/previous"
	do_rollback
	say "back on the stock IGMP Proxy plugin"
}

do_status() {
	echo "installed:  igmpproxy-plus $(version igmpproxy-plus)  os-igmp-proxy-plus $(version os-igmp-proxy-plus)  stock os-igmp-proxy $(version os-igmp-proxy)"
	if running; then echo "service:    running"; else echo "service:    not running"; fi
	if [ -f "$BACKUP/previous" ]; then
		echo "backup:     $(head -n1 "$BACKUP/previous") from $(cat "$BACKUP/date") in $BACKUP"
	else
		echo "backup:     none"
	fi
}

case "$ACTION" in
	install|upgrade) do_install ;;
	rollback|restore) do_rollback ;;
	uninstall|remove) do_uninstall ;;
	backup) save_backup ;;
	status) do_status ;;
	*) die "usage: sh install-opnsense.sh [install|rollback|uninstall|backup|status]" ;;
esac
