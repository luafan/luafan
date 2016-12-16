include $(TOPDIR)/rules.mk

PKG_NAME:=luafan
# Version: 1.0-1
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Sam Chang <sam.chang@me.com>

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=git://github.com/luafan/luafan.git
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_SOURCE_VERSION:=4d59422
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz

include $(INCLUDE_DIR)/package.mk

define Package/luafan
	SUBMENU:=Lua
	SECTION:=lang
	CATEGORY:=Languages
    DEPENDS:=+libevent2 +luafilesystem +liblua
	TITLE:=LUAFAN
endef

define Build/Prepare
	@echo "############## Build/Prepare"
	$(Build/Prepare/Default)
endef

define Package/luafan/install
	@echo "############## Package/luafan/install"
	$(INSTALL_DIR) $(1)/usr/lib/lua/
	$(INSTALL_DIR) $(1)/usr/share/lua/
	$(CP) $(PKG_BUILD_DIR)/modules/* $(1)/usr/share/lua/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fan.so $(1)/usr/lib/lua/
endef

MAKE_PATH = openwrt

$(eval $(call BuildPackage,luafan))
