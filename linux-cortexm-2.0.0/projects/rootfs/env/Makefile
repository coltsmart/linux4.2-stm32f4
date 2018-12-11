# Makefile for the U-boot env management tools
#
all             : apps
clean           : clean_apps

APPS            = ${INSTALL_ROOT}/u-boot/tools/env/fw_printenv
apps            : $(APPS)
$(APPS)		: 
	(cd ${INSTALL_ROOT}/u-boot/; \
	make distclean; \
	make stm-som_config; \
	make -s env CPPFLAGS="-Wall -DUSE_HOSTCC -I`pwd`/include \
		-mcpu=cortex-m4 -mthumb")

clean_apps      :
	-rm -f $(APPS)
