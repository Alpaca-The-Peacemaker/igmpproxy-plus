#!/bin/sh
# Build the OPNsense packages on FreeBSD (run as root on the FreeBSD release
# that matches OPNsense, e.g. FreeBSD 15.1 for OPNsense 26.7):
#
#   igmpproxy-plus-<abi>.pkg      the daemon (/usr/local/sbin/igmpproxy-plus + rc script)
#   os-igmp-proxy-plus.pkg        the OPNsense plugin (GUI, configd, templates)
#
# usage: sh tools/opnsense-build.sh <outdir>
set -eu

OUT=$1
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/opnsense-build}
VER=$(sed -n 's/^AC_INIT(\[igmpproxy-plus\], \[\(.*\)\])/\1/p' "$REPO/configure.ac")
ABI=$(pkg config abi)                       # e.g. FreeBSD:15:amd64
MAINT=$(sed -n 's/^PLUGIN_MAINTAINER=[[:space:]]*//p' "$REPO/opnsense/net/igmp-proxy-plus/Makefile")
WWW=https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
rm -rf "$WORK"
mkdir -p "$WORK"

pkg install -y autoconf automake gmake >/dev/null

# --- daemon -------------------------------------------------------------------
SRC=$WORK/src
cp -R "$REPO" "$SRC"
cd "$SRC"
./autogen.sh >/dev/null
./configure --prefix=/usr/local --mandir=/usr/local/share/man >/dev/null
gmake -j"$(sysctl -n hw.ncpu)"
./src/igmpproxy -h 2>&1 | tail -n1

STAGE=$WORK/stage-daemon
gmake install DESTDIR="$STAGE" >/dev/null
L=$STAGE/usr/local
mv "$L/sbin/igmpproxy" "$L/sbin/igmpproxy-plus"
mv "$L/share/man/man8/igmpproxy.8" "$L/share/man/man8/igmpproxy-plus.8"
mv "$L/share/man/man5/igmpproxy.conf.5" "$L/share/man/man5/igmpproxy-plus.conf.5"
gzip -9 "$L/share/man/man8/igmpproxy-plus.8" "$L/share/man/man5/igmpproxy-plus.conf.5"
rm -f "$L/etc/igmpproxy.conf"
install -m 644 "$SRC/igmpproxy.conf" "$L/etc/igmpproxy-plus.conf.sample"
mkdir -p "$L/etc/rc.d"
sed 's|%%PREFIX%%|/usr/local|g' "$REPO/opnsense/port/net/igmpproxy-plus/files/igmpproxy_plus.in" > "$L/etc/rc.d/igmpproxy_plus"
chmod 755 "$L/etc/rc.d/igmpproxy_plus"

META=$WORK/meta-daemon
mkdir -p "$META"
cat > "$META/+MANIFEST" <<EOF
name: igmpproxy-plus
version: "$VER"
origin: net/igmpproxy-plus
comment: "IGMP proxy with fast leave and multicast leave timer control"
maintainer: "$MAINT"
www: "$WWW"
prefix: /usr/local
abi: "$ABI"
licenselogic: single
licenses: [ "GPLv2+" ]
categories: [ "net" ]
desc: <<EOD
$(cat "$REPO/opnsense/port/net/igmpproxy-plus/pkg-descr")
EOD
EOF
(cd "$STAGE" && find . -type f | sed 's|^\./|/|' | sort) > "$META/plist"
pkg create -m "$META" -r "$STAGE" -p "$META/plist" -o "$WORK"
mv "$WORK"/igmpproxy-plus-*.pkg "$OUT/igmpproxy-plus-$(echo "$ABI" | tr ':' '-').pkg"

# --- plugin -------------------------------------------------------------------
P=$REPO/opnsense/net/igmp-proxy-plus
PSTAGE=$WORK/stage-plugin
mkdir -p "$PSTAGE/usr/local"
cp -R "$P/src/." "$PSTAGE/usr/local/"
PVER=$(sed -n 's/^PLUGIN_VERSION=[[:space:]]*//p' "$P/Makefile")
mkdir -p "$PSTAGE/usr/local/opnsense/version"
VERSION_JSON=$(cat <<EOF
{
    "product_abi": "",
    "product_arch": "",
    "product_conflicts": "os-igmp-proxy",
    "product_email": "$MAINT",
    "product_hash": "",
    "product_id": "os-igmp-proxy-plus",
    "product_name": "igmp-proxy-plus",
    "product_tier": "4",
    "product_version": "$PVER",
    "product_website": "$WWW"
}
EOF
)
echo "$VERSION_JSON" > "$PSTAGE/usr/local/opnsense/version/igmp-proxy-plus"

PMETA=$WORK/meta-plugin
mkdir -p "$PMETA"
DAEMON_VER=$VER
cat > "$PMETA/+MANIFEST" <<EOF
name: os-igmp-proxy-plus
version: "$PVER"
origin: opnsense/os-igmp-proxy-plus
comment: "IGMP proxy with fast leave and multicast leave timer control"
maintainer: "$MAINT"
www: "$WWW"
prefix: /usr/local
arch: "freebsd:*:*"
abi: "FreeBSD:*:*"
licenselogic: single
licenses: [ "BSD2CLAUSE" ]
categories: [ "net" ]
deps: { igmpproxy-plus: { version: "$DAEMON_VER", origin: "net/igmpproxy-plus" } }
annotations $VERSION_JSON
desc: <<EOD
$(cat "$P/pkg-descr")
EOD
EOF
# Same hooks the official plugin framework (Mk/plugins.mk) generates.
cat > "$PMETA/+POST_INSTALL" <<'EOF'
if [ -f /usr/local/etc/rc.d/configd ]; then /usr/local/etc/rc.d/configd restart; fi
if [ -f /usr/local/opnsense/mvc/script/run_migrations.php ]; then /usr/local/opnsense/mvc/script/run_migrations.php OPNsense/IGMPProxyPlus; fi
if [ -f /usr/local/etc/rc.configure_plugins ]; then echo "Reloading plugin configuration"; /usr/local/etc/rc.configure_plugins POST_INSTALL; fi
if [ -f /usr/local/sbin/configctl ]; then echo -n "Reloading template OPNsense/IGMPProxyPlus: "; /usr/local/sbin/configctl template reload OPNsense/IGMPProxyPlus; fi
EOF
cat > "$PMETA/+POST_DEINSTALL" <<'EOF'
if [ -f /usr/local/etc/rc.configure_plugins ]; then echo "Reloading plugin configuration"; /usr/local/etc/rc.configure_plugins POST_DEINSTALL; fi
EOF
(cd "$PSTAGE" && find . -type f | sed 's|^\./|/|' | sort) > "$PMETA/plist"
pkg create -m "$PMETA" -r "$PSTAGE" -p "$PMETA/plist" -o "$WORK"
mv "$WORK"/os-igmp-proxy-plus-*.pkg "$OUT/os-igmp-proxy-plus.pkg"

# --- quick checks -------------------------------------------------------------
pkg info -F "$OUT/igmpproxy-plus-$(echo "$ABI" | tr ':' '-').pkg" | head -n 12
pkg info -l -F "$OUT/os-igmp-proxy-plus.pkg" | head -n 5
"$L/sbin/igmpproxy-plus" -t "$SRC/igmpproxy.conf"
ls -la "$OUT"
