#!/bin/sh
# Build igmpproxy-plus and luci-app-igmpproxy-plus with the official OpenWrt SDK.
#
# usage: tools/openwrt-build.sh <release> <target/subtarget> <outdir> [source.tar.gz]
#   e.g. tools/openwrt-build.sh 25.12.6 x86/64 out
#
# Without a source tarball, one is made from the git checkout (git archive HEAD),
# laid out exactly like the GitHub release tarball the package downloads.
# Results are copied to <outdir> with stable names:
#   igmpproxy-plus-<series>-<arch>.<apk|ipk>, luci-app-igmpproxy-plus-<series>.<apk|ipk>
set -eu

REL=$1
TGT=$2
OUT=$3
SRC=${4:-}

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$REPO/work}
SERIES=${REL%.*}
JOBS=$(nproc 2>/dev/null || echo 2)
VER=$(sed -n 's/^PKG_VERSION:=//p' "$REPO/openwrt/net/igmpproxy-plus/Makefile")
BASE=https://downloads.openwrt.org/releases/$REL/targets/$TGT

mkdir -p "$WORK" "$OUT"
OUT=$(cd "$OUT" && pwd)

# --- source tarball ---------------------------------------------------------
TARBALL=$WORK/igmpproxy-plus-$VER.tar.gz
if [ -n "$SRC" ]; then
	cp "$SRC" "$TARBALL"
else
	git -C "$REPO" archive --format=tar.gz --prefix="igmpproxy-plus-$VER/" -o "$TARBALL" HEAD
fi

# --- SDK ----------------------------------------------------------------------
line=$(curl -fsSL "$BASE/sha256sums" | grep -E 'openwrt-sdk-.*\.tar\.(zst|xz)$')
sum=${line%% *}
file=${line##*\*}
[ -f "$WORK/$file" ] || curl -fsSL -o "$WORK/$file" "$BASE/$file"
echo "$sum  $WORK/$file" | sha256sum -c -

SDK=$WORK/sdk-$REL-$(echo "$TGT" | tr / -)
if [ ! -d "$SDK" ]; then
	mkdir -p "$SDK"
	case "$file" in
		*.zst) tar -I zstd -xf "$WORK/$file" -C "$SDK" --strip-components=1 --no-same-owner ;;
		*.xz)  tar -xJf "$WORK/$file" -C "$SDK" --strip-components=1 --no-same-owner ;;
	esac
fi

cd "$SDK"
grep -E '^src-git[^ ]* (--root=[^ ]+ )?(base|luci) ' feeds.conf.default > feeds.conf
echo "src-link igmpplus $REPO/openwrt" >> feeds.conf
./scripts/feeds update -a > "$WORK/feeds.log" 2>&1
./scripts/feeds install -f igmpproxy-plus luci-app-igmpproxy-plus >> "$WORK/feeds.log" 2>&1

mkdir -p dl
cp "$TARBALL" "dl/igmpproxy-plus-$VER.tar.gz"

make defconfig > /dev/null
rm -rf bin/packages/*/igmpplus
make package/igmpproxy-plus/compile -j"$JOBS" || make package/igmpproxy-plus/compile V=s
make package/luci-app-igmpproxy-plus/compile -j"$JOBS" || make package/luci-app-igmpproxy-plus/compile V=s

ARCH=$(sed -n 's/^CONFIG_TARGET_ARCH_PACKAGES="\(.*\)"/\1/p' .config)
for ext in apk ipk; do
	pkg=$(find bin/packages -path '*igmpplus*' -name "igmpproxy-plus*.$ext" | head -n1)
	luci=$(find bin/packages -path '*igmpplus*' -name "luci-app-igmpproxy-plus*.$ext" | head -n1)
	[ -n "$pkg" ] || continue
	cp "$pkg" "$OUT/igmpproxy-plus-$SERIES-$ARCH.$ext"
	cp "$luci" "$OUT/luci-app-igmpproxy-plus-$SERIES.$ext"
	echo "built: igmpproxy-plus-$SERIES-$ARCH.$ext luci-app-igmpproxy-plus-$SERIES.$ext"
done
