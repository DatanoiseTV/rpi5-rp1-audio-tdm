# rp1-audio-tdm - RP1 multi-peripheral I2S/TDM audio driver
#
# Build targets:
#   make               - build kernel module (on RPi5 or with cross-compile)
#   make dtbo          - compile device tree overlay
#   make install       - install module and overlay
#   make clean         - remove build artifacts
#
# Cross-compilation:
#   make KERNEL_DIR=/path/to/rpi-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
INSTALL_MOD_DIR ?= extra
OVERLAY_DIR ?= /boot/overlays

# Device tree compiler
DTC ?= dtc
DTC_FLAGS ?= -@ -I dts -O dtb -W no-unit_address_vs_reg
CPP ?= cpp
KERNEL_INCLUDE ?= $(KERNEL_DIR)

.PHONY: all module dtbo install install-module install-dtbo clean help

all: module dtbo

module:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

dtbo: dts/rp1-audio-tdm.dtbo

dts/%.dtbo: dts/%-overlay.dts
	$(CPP) -nostdinc -I $(KERNEL_INCLUDE)/include -undef -x assembler-with-cpp $< | \
		$(DTC) $(DTC_FLAGS) -o $@ -

install: install-module install-dtbo

install-module: module
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) INSTALL_MOD_DIR=$(INSTALL_MOD_DIR) modules_install
	depmod -a

install-dtbo: dtbo
	install -m 644 dts/rp1-audio-tdm.dtbo $(OVERLAY_DIR)/

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
	rm -f dts/*.dtbo

help:
	@echo "rp1-audio-tdm build system"
	@echo ""
	@echo "Targets:"
	@echo "  all            - build module and overlay (default)"
	@echo "  module         - build kernel module only"
	@echo "  dtbo           - compile device tree overlay only"
	@echo "  install        - install module and overlay"
	@echo "  install-module - install kernel module only"
	@echo "  install-dtbo   - install device tree overlay only"
	@echo "  clean          - remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_DIR     - kernel build directory (default: running kernel)"
	@echo "  OVERLAY_DIR    - overlay install path (default: /boot/overlays)"
