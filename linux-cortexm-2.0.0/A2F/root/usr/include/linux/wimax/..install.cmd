cmd_/work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax/.install := /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax ./include/uapi/linux/wimax i2400m.h; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax ./include/linux/wimax ; /bin/sh scripts/headers_install.sh /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax ./include/generated/uapi/linux/wimax ; for F in ; do echo "\#include <asm-generic/$$F>" > /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax/$$F; done; touch /work/psl/SF/release/cm-2.0.0/linux-cortexm-2.0.0/A2F/root/usr//include/linux/wimax/.install
