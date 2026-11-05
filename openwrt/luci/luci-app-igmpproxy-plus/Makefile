include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for igmpproxy-plus (fast leave and timer control)
LUCI_DEPENDS:=+igmpproxy-plus +luci-base
LUCI_PKGARCH:=all
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Dave Ferreira <8hx8dfdss2@privaterelay.appleid.com>

# Works both inside the luci feed and from a separate feed.
ifneq ($(wildcard ../../luci.mk),)
include ../../luci.mk
else
include $(TOPDIR)/feeds/luci/luci.mk
endif

# call BuildPackage - OpenWrt buildroot signature
