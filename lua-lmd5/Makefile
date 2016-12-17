include $(TOPDIR)/rules.mk

PKG_NAME:=lua-lmd5
# Version: 1.0-1
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Sam Chang <sam.chang@me.com>

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=git://github.com/samchang8826/lua-lmd5.git
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_SOURCE_VERSION:=ca528bd
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz

include $(INCLUDE_DIR)/package.mk

define Package/lua-lmd5
	SUBMENU:=Lua
	SECTION:=lang
	CATEGORY:=Languages
    DEPENDS:=+libopenssl +liblua
	TITLE:=LUA-LMD5
endef

define Build/Prepare
	@echo "############## Build/Prepare"
	$(Build/Prepare/Default)
	$(CP) ./openwrt $(PKG_BUILD_DIR)
endef

define Package/lua-lmd5/install
	@echo "############## Package/lua-lmd5/install"
	$(INSTALL_DIR) $(1)/usr/lib/lua/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/*.so $(1)/usr/lib/lua/
endef

MAKE_PATH = openwrt

$(eval $(call BuildPackage,lua-lmd5))
