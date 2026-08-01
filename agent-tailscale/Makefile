# Makefile for the webOS Tailscale VPN-agent plugin.
#
# Cross-compiles src/tailscale_agent.c into libVpnTailscaleAgent.so for the
# TouchPad (ARMv7-A, EABI5 softfp, kernel 2.6.35, glibc). Verified toolchain:
#   /opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi  (Linaro GCC 4.9.4)
#
# Target ABI confirmed from the stock libVpncAgent.so (readelf -A):
#   ARMv7-A, VFPv3, NEONv1, softfp (no Tag_ABI_VFP_args) -> -mfloat-abi=softfp.
#
# The tailscaled/tailscale binaries are NOT built here -- they are static Go
# binaries produced by `make -f Makefile.tailscaled` (see BUILD.md).
#
#   make
#   make check          # confirm 32-bit ARM ELF, softfp, entry points, NEEDED
#   make clean

# --- toolchain ---
TOOLCHAIN     ?= /opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi
CROSS_COMPILE ?= $(TOOLCHAIN)/bin/arm-linux-gnueabi-
CC            := $(CROSS_COMPILE)gcc

# --- sysroot with the device's libcjson + our json-c shim header ---
# (symlink to ../agent-openvpn/sysroot -- same device libs, no duplication)
SYSROOT ?= $(CURDIR)/sysroot

TARGET  := libVpnTailscaleAgent.so
SRC     := src/tailscale_agent.c
HDR     := src/webos_vpn_agent_abi.h

ARCH    := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -mthumb
WARN    := -Wall -Wextra -Wno-unused-parameter
STD     := -std=gnu99
OPT     := -O2 -fPIC -fvisibility=hidden

INCS    := -I$(SYSROOT)/usr/include
LIBDIRS := -L$(SYSROOT)/usr/lib
# Palm ships json-c as libcjson.so -> link -lcjson (NOT -ljson-c).
LIBS    := -lcjson -lglib-2.0

CFLAGS  := $(ARCH) $(STD) $(OPT) $(WARN) $(INCS)
LDFLAGS := -shared -Wl,-soname,$(TARGET) $(LIBDIRS)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LIBS)
	@echo "---- built $@ ----"
	@$(CROSS_COMPILE)nm -D $@ | grep -E 'initVpnAgent|cleanupVpnAgent' \
	   && echo "OK: entry points exported" || echo "WARN: entry points missing!"

check: $(TARGET)
	@$(CROSS_COMPILE)readelf -h $(TARGET) | grep -E 'Class|Machine|Type'
	@$(CROSS_COMPILE)readelf -A $(TARGET) | grep -E 'CPU_arch|FP_arch|VFP_args' || true
	@echo "-- NEEDED --"; $(CROSS_COMPILE)readelf -d $(TARGET) | grep NEEDED
	@echo "-- dynamic symbols (exported) --"; $(CROSS_COMPILE)nm -D --defined-only $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: check clean
