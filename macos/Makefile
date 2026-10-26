CC = xcrun clang
GUEST_CC ?= aarch64-linux-musl-gcc
DTC_PREFIX ?= $(shell brew --prefix dtc)
CFLAGS = -O2 -g -Wall -Wextra -Werror -mmacosx-version-min=15.0 -I$(DTC_PREFIX)/include
LDLIBS = $(DTC_PREFIX)/lib/libfdt.a -framework Hypervisor
KERNEL ?= .build/Image
BUILD = .build

.PHONY: all kernel run test clean
all: $(BUILD)/vmmlite $(BUILD)/machine.dtb $(BUILD)/initramfs.cpio
$(BUILD):
	mkdir -p $@
$(BUILD)/vmmlite: vmmlite.c entitlements.plist | $(BUILD)
	$(CC) $(CFLAGS) vmmlite.c $(LDLIBS) -o $@
	codesign --force --sign - --entitlements entitlements.plist $@
$(BUILD)/machine.dtb: machine.dts | $(BUILD)
	$(DTC_PREFIX)/bin/dtc -I dts -O dtb -o $@ $<
$(BUILD)/init: ../init.c | $(BUILD)
	$(GUEST_CC) -O2 -Wall -Wextra -Werror -static -o $@ $<
$(BUILD)/initramfs.cpio: $(BUILD)/init
	cd $(BUILD) && printf 'init\n' | cpio -o -H newc > initramfs.cpio
kernel: $(BUILD)/Image
$(BUILD)/Image: | $(BUILD)
	curl -fL --retry 2 --connect-timeout 30 --max-time 180 -o $@.tmp https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102
	printf 'c16c673b970b92a57b6afc0721c79cb90503900b5928c887f1f5eee508d3920a  %s\n' '$@.tmp' | shasum -a 256 -c -
	mv $@.tmp $@
$(BUILD)/test_native: tests/test_native.c vmmlite.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
$(BUILD)/timer/init: tests/timer.c | $(BUILD)
	mkdir -p $(BUILD)/timer
	$(GUEST_CC) -O2 -Wall -Wextra -Werror -static -o $@ $<
$(BUILD)/timer.cpio: $(BUILD)/timer/init
	cd $(BUILD)/timer && printf 'init\n' | cpio -o -H newc > ../timer.cpio
run: all $(KERNEL)
	$(BUILD)/vmmlite $(KERNEL) $(BUILD)/initramfs.cpio $(BUILD)/machine.dtb
test: all $(KERNEL) $(BUILD)/test_native $(BUILD)/timer.cpio
	$(BUILD)/test_native $(KERNEL)
	python3 tests/test_boot.py $(KERNEL)
clean:
	rm -rf $(BUILD)
