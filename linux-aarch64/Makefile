CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Werror
LDLIBS = -lfdt -pthread
KERNEL ?= .build/Image
BUILD = .build

.PHONY: all kernel run test clean
all: $(BUILD)/vmmlite $(BUILD)/machine.dtb $(BUILD)/initramfs.cpio
$(BUILD):
	mkdir -p $@
$(BUILD)/vmmlite: vmmlite.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
$(BUILD)/machine.dtb: machine.dts | $(BUILD)
	dtc -I dts -O dtb -o $@ $<
$(BUILD)/init: ../init.c | $(BUILD)
	$(CC) $(CFLAGS) -static -o $@ $<
$(BUILD)/initramfs.cpio: $(BUILD)/init
	cd $(BUILD) && printf 'init\n' | cpio -o -H newc > initramfs.cpio
kernel: $(BUILD)/Image
$(BUILD)/Image: | $(BUILD)
	curl -fL --retry 2 --connect-timeout 30 --max-time 180 -o $@.tmp https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102
	printf 'c16c673b970b92a57b6afc0721c79cb90503900b5928c887f1f5eee508d3920a  %s\n' '$@.tmp' | sha256sum -c -
	mv $@.tmp $@
$(BUILD)/test_native: tests/test_native.c vmmlite.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
$(BUILD)/timer/init: tests/timer.c | $(BUILD)
	mkdir -p $(BUILD)/timer
	$(CC) $(CFLAGS) -static -o $@ $<
$(BUILD)/timer.cpio: $(BUILD)/timer/init
	cd $(BUILD)/timer && printf 'init\n' | cpio -o -H newc > ../timer.cpio
run: all $(KERNEL)
	$(BUILD)/vmmlite $(KERNEL) $(BUILD)/initramfs.cpio $(BUILD)/machine.dtb
test: all $(KERNEL) $(BUILD)/test_native $(BUILD)/timer.cpio
	$(BUILD)/test_native $(KERNEL)
	python3 tests/test_boot.py $(KERNEL)
clean:
	rm -rf $(BUILD)
