include $(TOPDIR)/rules.mk

PKG_NAME:=rrd_hl
PKG_VERSION:=1.11.0
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/Reinerost/rrd_hl.git
PKG_SOURCE_VERSION:=3b76fc7f2344913b85821a70a46db64651b7a5b7
PKG_MIRROR_HASH:=skip

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

PKG_BUILD_DEPENDS:=
PKG_FIXUP:=autoreconf
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk


define Package/librrd_hl
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Headless Round Robin Database library
  URL:=https://github.com/Reinerost/rrd_hl
  DEPENDS:=+zlib
endef

define Package/librrd_hl/description
 Headless RRDtool library based on RRDtool 1.11.0.
 Provides RRD data processing and XPORT without graph rendering.
endef


define Package/rrd_hl
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Database
  TITLE:=Headless Round Robin Database tools
  URL:=https://github.com/Reinerost/rrd_hl
  DEPENDS:=+librrd_hl
endef

define Package/rrd_hl/description
 Headless RRDtool command line utility with DEF, CDEF, VDEF,
 SHIFT and XPORT support.
endef


CONFIGURE_ARGS += \
	--enable-shared \
	--disable-static \
	--disable-rrd_graph \
	--disable-perl \
	--disable-python \
	--disable-ruby \
	--disable-tcl \
	--disable-docs \
	--disable-examples


define Package/librrd_hl/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librrd.so.* $(1)/usr/lib/
endef


define Package/rrd_hl/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/rrdtool $(1)/usr/bin/
endef


define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_INSTALL_DIR)/usr/include/rrd.h $(1)/usr/include/

	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librrd.so* $(1)/usr/lib/
endef


$(eval $(call BuildPackage,librrd_hl))
$(eval $(call BuildPackage,rrd_hl))
