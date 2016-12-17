include $(TOPDIR)/rules.mk

PKG_NAME:=lua-compat53
# Version: 1.0-1
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Sam Chang <sam.chang@me.com>

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=git://github.com/keplerproject/lua-compat-5.3.git
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_SOURCE_VERSION:=77aa564
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz

include $(INCLUDE_DIR)/package.mk

define Package/lua-compat53
	SUBMENU:=Lua
	SECTION:=lang
	CATEGORY:=Languages
    DEPENDS:=+liblua
	TITLE:=LUA-COMPAT53
endef

define Build/Prepare
	@echo "############## Build/Prepare"
	$(Build/Prepare/Default)
	$(CP) ./openwrt $(PKG_BUILD_DIR)
endef

define Package/lua-compat53/install
	@echo "############## Package/lua-compat53/install"
	$(INSTALL_DIR) $(1)/usr/lib/lua/compat53
	$(INSTALL_DIR) $(1)/usr/share/lua/compat53
	$(CP) $(PKG_BUILD_DIR)/compat53/*.lua $(1)/usr/share/lua/compat53
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/compat53/*.so $(1)/usr/lib/lua/compat53
endef

MAKE_PATH = openwrt

$(eval $(call BuildPackage,lua-compat53))
