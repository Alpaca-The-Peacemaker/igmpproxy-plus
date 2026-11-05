#!/bin/sh
# igmpproxy-plus installer for OpenWrt 24.10 (opkg) and 25.12 (apk).
#
#   sh install.sh             back up, then install or upgrade igmpproxy-plus
#   sh install.sh rollback    undo the last install/upgrade (previous version + config)
#   sh install.sh uninstall   go back to the stock igmpproxy package
#   sh install.sh backup      only save a backup
#   sh install.sh status      show what is installed and the saved backup
#
# Every install saves a backup to /etc/igmpproxy-plus-backup first: your
# /etc/config/igmpproxy and which igmpproxy version was installed. If the new
# version fails to install or does not start, the installer rolls back on
# its own. Packages: https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus/releases

set -e

REPO=Alpaca-The-Peacemaker/igmpproxy-plus
RELEASES=https://github.com/$REPO/releases
URL=${IGMPPROXY_PLUS_URL:-$RELEASES/latest/download}
TMP=/tmp/igmpproxy-plus
BACKUP=/etc/igmpproxy-plus-backup
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

# --- package manager helpers ------------------------------------------------

pkg_version() {
	if [ "$PM" = apk ]; then
		apk info -e "$1" >/dev/null 2>&1 || return 0
		apk list -I "$1" 2>/dev/null | sed -n "s/^$1-\([0-9][^ ]*\) .*/\1/p" | head -n1
	else
		opkg status "$1" 2>/dev/null | grep -q '^Status:.* installed' || return 0
		opkg status "$1" | sed -n 's/^Version: //p'
	fi
}

installed() { [ -n "$(pkg_version "$1")" ]; }

pkg_del() {
	if [ "$PM" = apk ]; then apk del "$@"; else opkg remove "$@"; fi
}

pkg_add_files() {
	if [ "$PM" = apk ]; then
		apk add --allow-untrusted "$@"
	else
		opkg install --force-downgrade "$@"
	fi
}

pkg_add_stock() {
	if [ "$PM" = apk ]; then
		apk update >/dev/null && apk add igmpproxy
	else
		opkg update >/dev/null && opkg install igmpproxy
	fi
}

running() { /etc/init.d/igmpproxy running >/dev/null 2>&1; }

# download <base-url> <file>
fetch() {
	mkdir -p "$TMP"
	wget -q -O "$TMP/$2" "$1/$2" || {
		rm -f "$TMP/$2"
		return 1
	}
}

# --- backup -------------------------------------------------------------------

save_backup() {
	mkdir -p "$BACKUP"
	if [ -f /etc/config/igmpproxy ]; then
		cp /etc/config/igmpproxy "$BACKUP/igmpproxy.config"
	else
		rm -f "$BACKUP/igmpproxy.config"
	fi
	plus=$(pkg_version igmpproxy-plus)
	stock=$(pkg_version igmpproxy)
	if [ -n "$plus" ]; then
		echo "plus $plus" > "$BACKUP/previous"
	elif [ -n "$stock" ]; then
		echo "stock $stock" > "$BACKUP/previous"
	else
		echo "none" > "$BACKUP/previous"
	fi
	if running; then echo 1 > "$BACKUP/was_running"; else echo 0 > "$BACKUP/was_running"; fi
	date > "$BACKUP/date"
	# Keep this script around so rollback works after /tmp is cleared.
	if [ -f "$0" ] && [ "$0" != /usr/sbin/igmpproxy-plus-install ]; then
		cp "$0" /usr/sbin/igmpproxy-plus-install && chmod 755 /usr/sbin/igmpproxy-plus-install
	fi
	say "backup saved to $BACKUP (previous: $(cat "$BACKUP/previous"))"
}

restore_config() {
	if [ -f "$BACKUP/igmpproxy.config" ]; then
		cp "$BACKUP/igmpproxy.config" /etc/config/igmpproxy
		say "restored /etc/config/igmpproxy from the backup"
	fi
}

# --- rollback ---------------------------------------------------------------

do_rollback() {
	[ -f "$BACKUP/previous" ] || die "no backup found in $BACKUP"
	set -- $(cat "$BACKUP/previous")
	kind=$1
	ver=${2:-}
	say "rolling back to: $kind $ver (backup from $(cat "$BACKUP/date" 2>/dev/null))"

	case "$kind" in
	stock)
		installed luci-app-igmpproxy-plus && pkg_del luci-app-igmpproxy-plus
		installed igmpproxy-plus && pkg_del igmpproxy-plus
		pkg_add_stock || die "could not reinstall stock igmpproxy (no internet?). Try: $([ "$PM" = apk ] && echo "apk add" || echo "opkg install") igmpproxy"
		;;
	plus)
		tag=v${ver%-r*}
		pkg=igmpproxy-plus-$SERIES-$ARCH.$EXT
		luci=luci-app-igmpproxy-plus-$SERIES.$EXT
		if fetch "$RELEASES/download/$tag" "$pkg" && fetch "$RELEASES/download/$tag" "$luci"; then
			pkg_add_files "$TMP/$pkg" "$TMP/$luci"
		else
			say "release $tag is not on GitHub, keeping the installed version and restoring only the config"
		fi
		;;
	none)
		installed luci-app-igmpproxy-plus && pkg_del luci-app-igmpproxy-plus
		installed igmpproxy-plus && pkg_del igmpproxy-plus
		;;
	*)
		die "unknown backup state '$kind'"
		;;
	esac

	restore_config
	if [ "$kind" != none ]; then
		/etc/init.d/igmpproxy enable
		/etc/init.d/igmpproxy restart
	fi
	rm -f /etc/config/igmpproxy.apk-new /etc/config/igmpproxy-opkg
	rm -rf "$TMP"
	say "rollback done"
}

# --- install ------------------------------------------------------------------

do_install() {
	case "$SERIES" in
		24.10|25.12) ;;
		*) die "OpenWrt $DISTRIB_RELEASE is not supported, only 24.10 and 25.12" ;;
	esac

	PKG=igmpproxy-plus-$SERIES-$ARCH.$EXT
	LUCI=luci-app-igmpproxy-plus-$SERIES.$EXT

	say "OpenWrt $DISTRIB_RELEASE, $ARCH, using $PM"
	say "downloading $PKG"
	fetch "$URL" "$PKG" || die "no prebuilt package for $ARCH on OpenWrt $SERIES. Build it with the SDK, see the README."
	say "downloading $LUCI"
	fetch "$URL" "$LUCI" || die "could not download $LUCI"

	save_backup

	if installed igmpproxy; then
		say "removing the stock igmpproxy package"
		pkg_del igmpproxy
	fi

	say "installing"
	if ! pkg_add_files "$TMP/$PKG" "$TMP/$LUCI"; then
		say "installation failed, rolling back"
		do_rollback
		exit 1
	fi

	# Removing the stock package can drop an unmodified config file, and an
	# upgrade leaves the package default next to yours as .apk-new / -opkg.
	if [ -f "$BACKUP/igmpproxy.config" ] && ! cmp -s "$BACKUP/igmpproxy.config" /etc/config/igmpproxy; then
		cp "$BACKUP/igmpproxy.config" /etc/config/igmpproxy
		say "kept your /etc/config/igmpproxy"
	fi
	rm -f /etc/config/igmpproxy.apk-new /etc/config/igmpproxy-opkg

	/etc/init.d/igmpproxy enable
	/etc/init.d/igmpproxy restart
	rm -f /tmp/luci-indexcache* 2>/dev/null || true

	# Automatic rollback if it was running before and does not come up now.
	if [ "$(cat "$BACKUP/was_running")" = 1 ]; then
		i=0
		until running; do
			i=$((i + 1))
			if [ "$i" -ge 10 ]; then
				say "the proxy did not start, rolling back. Log: logread -e igmpproxy"
				do_rollback
				exit 1
			fi
			sleep 1
		done
	fi
	rm -rf "$TMP"

	say "installed $(pkg_version igmpproxy-plus)"
	if running; then
		say "running. Settings: LuCI > Services > IGMP Proxy. Live table: igmpproxy -S"
	else
		say "installed but not running: it starts once an upstream and a downstream interface are configured and up"
	fi
	say "to undo: igmpproxy-plus-install rollback"
}

do_uninstall() {
	installed igmpproxy-plus || die "igmpproxy-plus is not installed"
	save_backup
	say "removing igmpproxy-plus"
	installed luci-app-igmpproxy-plus && pkg_del luci-app-igmpproxy-plus
	pkg_del igmpproxy-plus
	say "installing the stock igmpproxy package"
	pkg_add_stock || die "could not install stock igmpproxy (no internet?). Undo with: igmpproxy-plus-install rollback"
	restore_config
	/etc/init.d/igmpproxy enable
	/etc/init.d/igmpproxy restart
	say "back on stock igmpproxy. Options only igmpproxy-plus knows are ignored."
}

do_status() {
	echo "installed:  igmpproxy-plus $(pkg_version igmpproxy-plus)  luci-app $(pkg_version luci-app-igmpproxy-plus)  stock igmpproxy $(pkg_version igmpproxy)"
	if running; then echo "service:    running"; else echo "service:    not running"; fi
	if [ -f "$BACKUP/previous" ]; then
		echo "backup:     $(cat "$BACKUP/previous") from $(cat "$BACKUP/date") in $BACKUP"
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
	*) die "usage: sh install.sh [install|rollback|uninstall|backup|status]" ;;
esac
