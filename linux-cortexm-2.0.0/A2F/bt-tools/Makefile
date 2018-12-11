#
# BT tools makefile:
#
# to build: $ make
# to clean: $ make clean
#

# Dirs
DST_DIR		:= $(INSTALL_ROOT)/A2F/root
BLUEZ_LIB	:= bluez-libs-2.25
BLUEZ_UTILS	:= bluez-utils-2.25
OPENOBEX	:= openobex-1.5
OBEXFTP		:= obexftp-0.23

# Flags
CFLAGS		:= "-Os -mcpu=cortex-m3 -mthumb -I$(INSTALL_ROOT)/A2F/root/usr/include"
LDFLAGS		:= "-mcpu=cortex-m3 -mthumb -L$(INSTALL_ROOT)/A2F/root/usr/lib"

.PHONY:		all
all:		$(BLUEZ_LIB).build $(BLUEZ_UTILS).build $(OPENOBEX).build $(OBEXFTP).build
	arm-uclinuxeabi-flthdr -s 32768 $(DST_DIR)/usr/local/sbin/hcid

clean:
	make -C $(BLUEZ_LIB) distclean; \
	make -C $(BLUEZ_UTILS) distclean; \
	make -C $(OPENOBEX) distclean; \
	make -C $(OBEXFTP) distclean

%.build:	%/config.h
	make -C $(subst $\.build,,$@) install DESTDIR=$(DST_DIR)

$(BLUEZ_LIB)/config.h:
	cd $(BLUEZ_LIB) && \
	CFLAGS="$(subst $\",,$(CFLAGS)) -static -Dfork=vfork" \
	LDFLAGS="$(subst $\",,$(LDFLAGS))" \
	./configure --host=arm-uclinuxeabi

$(BLUEZ_UTILS)/config.h:
	cd $(BLUEZ_UTILS) && \
	CFLAGS="$(subst $\",,$(CFLAGS)) -Dfork=vfork" \
	LDFLAGS="$(subst $\",,$(LDFLAGS))" \
	./configure --host=arm-uclinuxeabi --with-bluez=$(DST_DIR)/usr/local

$(OPENOBEX)/config.h:
	cd $(OPENOBEX) && \
	CC=arm-uclinuxeabi-gcc \
	CFLAGS="$(subst $\",,$(CFLAGS)) -I$(DST_DIR)/usr/local/include" \
	LDFLAGS="$(subst $\",,$(LDFLAGS))" \
	BLUETOOTH_CFLAGS="$(subst $\",,$(CFLAGS)) -I$(DST_DIR)/usr/local/include" \
	BLUETOOTH_LIBS="-lbluetooth" \
	./configure --host=arm-uclinux-linux \
		--disable-usb --disable-shared \
		--enable-bluetooth

$(OBEXFTP)/config.h:
	cd $(OBEXFTP) && \
	CC=arm-uclinuxeabi-gcc \
	CFLAGS="$(subst $\",,$(CFLAGS)) -I$(DST_DIR)/usr/local/include" \
	LDFLAGS="$(subst $\",,$(LDFLAGS))" \
	OPENOBEX_CFLAGS="$(subst $\",,$(CFLAGS)) -I$(DST_DIR)/usr/local/include -L$(DST_DIR)/usr/local/lib" \
	OPENOBEX_LIBS="-lopenobex" \
	BLUETOOTH_CFLAGS="$(subst $\",,$(CFLAGS)) -I$(DST_DIR)/usr/local/include" \
	BLUETOOTH_LIBS="-lbluetooth" \
	./configure --host=arm-uclinux-linux \
		--disable-shared \
		--disable-perl --disable-python --disable-ruby --disable-tcl \
		--enable-bluetooth
