#!/bin/sh
# igmpproxy-plus installer for OpenWrt 24.10 (opkg) and 25.12 (apk).
#
#   sh install.sh             install or upgrade igmpproxy-plus + LuCI app
#   sh install.sh uninstall   go back to the stock igmpproxy package
#
# Your /etc/config/igmpproxy is kept: igmpproxy-plus reads the same file.
# Packages come from https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus/releases

set -e

REPO=Alpaca-The-Peacemaker/igmpproxy-plus
URL=${IGMPPROXY_PLUS_URL:-https://github.com/$REPO/releases/latest/download}
TMP=/tmp/igmpproxy-plus
BACKUP=/etc/igmpproxy.config.pre-plus
ACTION=${1:-install}

say() { echo "igmpproxy-plus: $*"; }
die() { echo "igmpproxy-plus: error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run this as root"
[ -f /etc/openwrt_release ] || die "this does not look like OpenWrt"
. /etc/openwrt_release

SERIES=$(echo "$DISTRIB_RELEASE" | cut -d. -f1,2)
ARCH=$DISTRIB_ARCH
if command -v apk >/dev/null 2>&1; then
	PM=apk
	EXT=apk
else
	PM=opkg
	EXT=ipk
fi

installed() {
	if [ "$PM" = apk ]; then
		apk info -e "$1" >/dev/null 2>&1
	else
		opkg status "$1" 2>/dev/null | grep -q '^Status:.* installed'
	fi
}

pkg_del() {
	if [ "$PM" = apk ]; then apk del "$@"; else opkg remove "$@"; fi
}

fetch() {
	wget -q -O "$TMP/$1" "$URL/$1" || {
		rm -f "$TMP/$1"
		return 1
	}
}

do_install() {
	case "$SERIES" in
		24.10|25.12) ;;
		*) die "OpenWrt $DISTRIB_RELEASE is not supported, only 24.10 and 25.12" ;;
	esac

	mkdir -p "$TMP"
	PKG=igmpproxy-plus-$SERIES-$ARCH.$EXT
	LUCI=luci-app-igmpproxy-plus-$SERIES.$EXT

	say "OpenWrt $DISTRIB_RELEASE, $ARCH, using $PM"
	say "downloading $PKG"
	fetch "$PKG" || die "no prebuilt package for $ARCH on OpenWrt $SERIES. Build it with the SDK, see the README."
	say "downloading $LUCI"
	fetch "$LUCI" || die "could not download $LUCI"

	REPLACED_STOCK=0
	if installed igmpproxy; then
		if [ -f /etc/config/igmpproxy ]; then
			cp /etc/config/igmpproxy "$BACKUP"
			say "saved your config to $BACKUP"
		fi
		say "removing the stock igmpproxy package"
		pkg_del igmpproxy
		REPLACED_STOCK=1
	fi

	say "installing"
	if [ "$PM" = apk ]; then
		apk add --allow-untrusted "$TMP/$PKG" "$TMP/$LUCI"
	else
		opkg install "$TMP/$PKG" "$TMP/$LUCI"
	fi

	# Removing the stock package can drop an unmodified config file.
	if [ "$REPLACED_STOCK" = 1 ] && [ -f "$BACKUP" ] && ! cmp -s "$BACKUP" /etc/config/igmpproxy; then
		cp "$BACKUP" /etc/config/igmpproxy
		say "restored your previous /etc/config/igmpproxy"
	fi

	/etc/init.d/igmpproxy enable
	/etc/init.d/igmpproxy restart
	rm -f /tmp/luci-indexcache* 2>/dev/null || true
	rm -rf "$TMP"

	sleep 1
	if /usr/sbin/igmpproxy -S >/dev/null 2>&1; then
		say "running. Settings: LuCI > Services > IGMP Proxy. Live table: igmpproxy -S"
	else
		say "installed, but the proxy is not running yet. Check: logread -e igmpproxy"
	fi
}

do_uninstall() {
	installed igmpproxy-plus || die "igmpproxy-plus is not installed"
	[ -f /etc/config/igmpproxy ] && cp /etc/config/igmpproxy /tmp/igmpproxy.config.plus
	say "removing igmpproxy-plus"
	installed luci-app-igmpproxy-plus && pkg_del luci-app-igmpproxy-plus
	pkg_del igmpproxy-plus

	say "installing the stock igmpproxy package"
	if [ "$PM" = apk ]; then
		apk update >/dev/null && apk add igmpproxy
	else
		opkg update >/dev/null && opkg install igmpproxy
	fi

	if [ -f "$BACKUP" ]; then
		cp "$BACKUP" /etc/config/igmpproxy
		say "restored the config you had before igmpproxy-plus"
	elif [ -f /tmp/igmpproxy.config.plus ]; then
		cp /tmp/igmpproxy.config.plus /etc/config/igmpproxy
		say "kept your config (options only igmpproxy-plus knows are ignored by stock igmpproxy)"
	fi
	/etc/init.d/igmpproxy restart
	say "back on stock igmpproxy"
}

case "$ACTION" in
	install|upgrade) do_install ;;
	uninstall|remove) do_uninstall ;;
	*) die "usage: sh install.sh [install|uninstall]" ;;
esac
