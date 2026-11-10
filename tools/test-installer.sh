#!/bin/sh
# Exercise openwrt/install.sh against a fake OpenWrt (apk, wget, init script).
# DESTRUCTIVE for the host it runs on: writes /etc/openwrt_release,
# /etc/config/igmpproxy, /etc/init.d/igmpproxy and /usr/sbin/apk.
# Only run it in a throwaway container or CI runner, as root.
#
# usage: sh tools/test-installer.sh [path/to/install.sh]
set -u
INSTALLER=${1:-$(cd "$(dirname "$0")/.." && pwd)/openwrt/install.sh}
BUSYBOX=$(command -v busybox) || { echo "busybox is needed"; exit 1; }
SH=${SH:-$BUSYBOX ash}
MOCK=/var/mockwrt
FAILS=0

setup() {
	rm -rf "$MOCK" /etc/igmpproxy-plus-backup /usr/sbin/igmpproxy-plus-install
	mkdir -p "$MOCK/pkgs" "$MOCK/bin" /etc/config /etc/init.d
	cat > /etc/openwrt_release <<'EOF'
DISTRIB_ID='OpenWrt'
DISTRIB_RELEASE='25.12.6'
DISTRIB_ARCH='x86_64'
EOF
	echo "config igmpproxy 'user-config'" > /etc/config/igmpproxy

	# fake apk: one file per installed package holding its version
	cat > "$MOCK/bin/apk" <<'EOF'
#!/bin/sh
P=/var/mockwrt/pkgs
case "$1" in
info) [ -f "$P/$3" ] ;;
list) [ -f "$P/$3" ] && echo "$3-$(cat "$P/$3") x86_64 {$3} (GPL) [installed]" ; exit 0 ;;
del) shift; for n; do rm -f "$P/$n"; done ;;
update) exit 0 ;;
add)
	shift
	[ "$1" = --allow-untrusted ] && shift
	for f; do
		case "$f" in /*) [ -f /var/mockwrt/fail_install ] && { echo "mock: install error" >&2; exit 1; } ;; esac
		case "$f" in
		/*) set -- $(cat "$f"); echo "$2" > "$P/$1"; echo "config igmpproxy 'package-default'" > /etc/config/igmpproxy.apk-new ;;
		*) [ -f /var/mockwrt/fail_stock ] && { echo "mock: no network" >&2; exit 1; }; echo "0.4-r1" > "$P/$f" ;;
		esac
	done ;;
esac
EOF
	# fake wget: serve "packages" for latest (1.0.2) or a tag
	cat > "$MOCK/bin/wget" <<'EOF'
#!/bin/sh
out=$3; url=$4
[ -f /var/mockwrt/fail_download ] && exit 1
case "$url" in
*/latest/download/*) ver=1.0.2 ;;
*/download/v*) ver=${url#*/download/v}; ver=${ver%%/*} ;;
esac
name=${url##*/}
case "$name" in
luci-*) echo "luci-app-igmpproxy-plus 26.1.$ver" > "$out" ;;
*) echo "igmpproxy-plus $ver-r1" > "$out" ;;
esac
EOF
	# fake init script: "running" when a proxy package is installed
	cat > /etc/init.d/igmpproxy <<'EOF'
#!/bin/sh
case "$1" in
running)
	P=/var/mockwrt/pkgs
	if [ -f $P/igmpproxy-plus ]; then [ "$(cat $P/igmpproxy-plus)" != "$(cat /var/mockwrt/broken_version 2>/dev/null)" ]
	else [ -f $P/igmpproxy ]; fi ;;
restart)
	P=/var/mockwrt/pkgs
	[ -f /var/mockwrt/fail_restart ] && [ -f $P/igmpproxy-plus ] && 		[ "$(cat $P/igmpproxy-plus)" = "$(cat /var/mockwrt/broken_version 2>/dev/null)" ] && exit 1
	exit 0 ;;
*) exit 0 ;;
esac
EOF
	# fake curl: absent unless a test creates it (the real one must not be used)
	cat > "$MOCK/bin/curl" <<'EOF'
#!/bin/sh
[ -f /var/mockwrt/fail_curl ] && exit 28
while [ $# -gt 1 ]; do [ "$1" = -o ] && out=$2; shift; done
echo "curl" > /var/mockwrt/used_curl
exec /var/mockwrt/bin/wget -q -O "$out" "$1"
EOF
	chmod +x "$MOCK/bin/apk" "$MOCK/bin/wget" "$MOCK/bin/curl" /etc/init.d/igmpproxy
	# the installer sees only the mocks and busybox, never a real curl or
	# wget of the test host; the fake curl is enabled per test (with_curl)
	mkdir -p "$MOCK/tools"
	for a in $($BUSYBOX --list); do ln -sf "$BUSYBOX" "$MOCK/tools/$a"; done
	rm -f "$MOCK/tools/wget" "$MOCK/tools/curl"
	mv "$MOCK/bin/curl" "$MOCK/curl.fake"
}

run() { PATH="$MOCK/bin:$MOCK/tools" $SH "$INSTALLER" "$@" > "$MOCK/out" 2>&1; echo $? > "$MOCK/rc"; }
with_curl() { cp "$MOCK/curl.fake" "$MOCK/bin/curl"; }
ver() { cat "$MOCK/pkgs/$1" 2>/dev/null || echo "-"; }
check() {
	if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: expected '$3', got '$2'"; FAILS=$((FAILS + 1)); sed 's/^/       | /' "$MOCK/out"; fi
}

echo "1. fresh install over stock igmpproxy"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
run
check "exit code" "$(cat "$MOCK/rc")" 0
check "plus installed" "$(ver igmpproxy-plus)" "1.0.2-r1"
check "stock removed" "$(ver igmpproxy)" "-"
check "config kept" "$(cat /etc/config/igmpproxy)" "config igmpproxy 'user-config'"
check ".apk-new removed" "$([ -f /etc/config/igmpproxy.apk-new ] && echo yes || echo no)" no
check "backup says stock" "$(cat /etc/igmpproxy-plus-backup/previous)" "stock 0.4-r1"
check "installer kept" "$([ -x /usr/sbin/igmpproxy-plus-install ] && echo yes || echo no)" yes

echo "2. rollback to stock"
echo "config igmpproxy 'changed-later'" > /etc/config/igmpproxy
run rollback
check "exit code" "$(cat "$MOCK/rc")" 0
check "plus removed" "$(ver igmpproxy-plus)" "-"
check "stock back" "$(ver igmpproxy)" "0.4-r1"
check "config restored" "$(cat /etc/config/igmpproxy)" "config igmpproxy 'user-config'"

echo "3. upgrade from an older plus, then rollback to it"
setup
echo "1.0.1-r1" > "$MOCK/pkgs/igmpproxy-plus"
run
check "upgraded" "$(ver igmpproxy-plus)" "1.0.2-r1"
check "backup says plus 1.0.1" "$(cat /etc/igmpproxy-plus-backup/previous)" "plus 1.0.1-r1"
run rollback
check "exit code" "$(cat "$MOCK/rc")" 0
check "back on 1.0.1" "$(ver igmpproxy-plus)" "1.0.1-r1"

echo "4. automatic rollback when the new version does not start"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
echo "1.0.2-r1" > "$MOCK/broken_version"   # the new release never starts
run
check "exit code" "$(cat "$MOCK/rc")" 1
check "stock back" "$(ver igmpproxy)" "0.4-r1"
check "plus removed" "$(ver igmpproxy-plus)" "-"
check "config restored" "$(cat /etc/config/igmpproxy)" "config igmpproxy 'user-config'"

echo "5. failed install rolls back"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
touch "$MOCK/fail_install"
run
rm -f "$MOCK/fail_install"
check "exit code" "$(cat "$MOCK/rc")" 1
check "stock back" "$(ver igmpproxy)" "0.4-r1"
check "config restored" "$(cat /etc/config/igmpproxy)" "config igmpproxy 'user-config'"

echo "6. download failure changes nothing"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
touch "$MOCK/fail_download"
run
check "exit code" "$(cat "$MOCK/rc")" 1
check "stock untouched" "$(ver igmpproxy)" "0.4-r1"
check "no backup made" "$([ -d /etc/igmpproxy-plus-backup ] && echo yes || echo no)" no
check "says download failed" "$(grep -c "could not download" "$MOCK/out")" 1
check "suggests curl" "$(grep -c "add curl" "$MOCK/out")" 1

echo "7. failed restart still triggers the automatic rollback"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
echo "1.0.2-r1" > "$MOCK/broken_version"
touch "$MOCK/fail_restart"
run
rm -f "$MOCK/fail_restart"
check "exit code" "$(cat "$MOCK/rc")" 1
check "stock back" "$(ver igmpproxy)" "0.4-r1"
check "plus removed" "$(ver igmpproxy-plus)" "-"

echo "8. rollback from an opkg-style version (1.0.1-1, OpenWrt 24.10)"
setup
echo "1.0.1-1" > "$MOCK/pkgs/igmpproxy-plus"
run
check "upgraded" "$(ver igmpproxy-plus)" "1.0.2-r1"
run rollback
check "exit code" "$(cat "$MOCK/rc")" 0
check "back on 1.0.1" "$(ver igmpproxy-plus)" "1.0.1-r1"

echo "9. rollback whose download fails reports failure"
setup
echo "1.0.1-r1" > "$MOCK/pkgs/igmpproxy-plus"
run
touch "$MOCK/fail_download"
run rollback
rm -f "$MOCK/fail_download"
check "exit code" "$(cat "$MOCK/rc")" 1
check "says incomplete" "$(grep -c INCOMPLETE "$MOCK/out")" 1

echo "10. rollback whose restored version does not start reports failure"
setup
echo "1.0.1-r1" > "$MOCK/pkgs/igmpproxy-plus"
run
echo "1.0.1-r1" > "$MOCK/broken_version"   # the old release no longer starts
touch "$MOCK/fail_restart"
run rollback
rm -f "$MOCK/fail_restart" "$MOCK/broken_version"
check "exit code" "$(cat "$MOCK/rc")" 1
check "says incomplete" "$(grep -c INCOMPLETE "$MOCK/out")" 1
check "does not claim done" "$(grep -c "rollback done" "$MOCK/out")" 0
check "old version installed" "$(ver igmpproxy-plus)" "1.0.1-r1"

echo "11. rollback to stock that cannot be reinstalled restores the config and reports failure"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
run
echo "config igmpproxy 'changed-later'" > /etc/config/igmpproxy
touch "$MOCK/fail_stock"
run rollback
rm -f "$MOCK/fail_stock"
check "exit code" "$(cat "$MOCK/rc")" 1
check "says incomplete" "$(grep -c INCOMPLETE "$MOCK/out")" 1
check "config restored" "$(cat /etc/config/igmpproxy)" "config igmpproxy 'user-config'"

echo "12. curl is used when installed"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
with_curl
run
check "exit code" "$(cat "$MOCK/rc")" 0
check "plus installed" "$(ver igmpproxy-plus)" "1.0.2-r1"
check "curl used" "$(cat "$MOCK/used_curl" 2>/dev/null)" curl

echo "13. failing curl falls back to wget"
setup
echo "0.4-r1" > "$MOCK/pkgs/igmpproxy"
with_curl
touch "$MOCK/fail_curl"
run
check "exit code" "$(cat "$MOCK/rc")" 0
check "plus installed" "$(ver igmpproxy-plus)" "1.0.2-r1"

echo
if [ "$FAILS" = 0 ]; then echo "all installer tests passed"; else echo "$FAILS installer checks failed"; exit 1; fi
