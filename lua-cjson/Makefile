include $(TOPDIR)/rules.mk

PKG_NAME:=lua-cjson
# Version: 1.0-1
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Sam Chang <sam.chang@me.com>

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=git://github.com/mpx/lua-cjson.git
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_SOURCE_VERSION:=e8972ac
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz

include $(INCLUDE_DIR)/package.mk

define Package/lua-cjson
	SUBMENU:=Lua
	SECTION:=lang
	CATEGORY:=Languages
    DEPENDS:=+liblua
	TITLE:=LUA-CJSON
endef

define Build/Prepare
	@echo "############## Build/Prepare"
	$(Build/Prepare/Default)
	$(CP) ./openwrt $(PKG_BUILD_DIR)
endef

define Package/lua-cjson/install
	@echo "############## Package/lua-cjson/install"
	$(INSTALL_DIR) $(1)/usr/lib/lua/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cjson.so $(1)/usr/lib/lua/
endef

MAKE_PATH = openwrt

$(eval $(call BuildPackage,lua-cjson))
