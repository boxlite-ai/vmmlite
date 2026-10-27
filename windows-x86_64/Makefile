# Build the Windows VMM from one C file; the shared init.c still runs inside Linux.
ZIG ?= zig
HOST_CC ?= cc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror

.PHONY: all build test check guest boot check-image clean
all: build
build: vmmlite.exe

vmmlite.exe: vmmlite.c
	$(ZIG) cc -target x86_64-windows-gnu $(CFLAGS) -o $@ vmmlite.c -lwinhvplatform -lwinhvemulation

tests/machine-test: tests/machine_test.c vmmlite.c
	$(HOST_CC) $(CFLAGS) -g -fsanitize=address,undefined -I. -o $@ tests/machine_test.c

test: tests/machine-test
	./tests/machine-test

check: test build

boot: build
	powershell -NoProfile -File test-boot.ps1

check-image: tests/machine-test initramfs.cpio
	./tests/machine-test "$(KERNEL)" initramfs.cpio

# Run this target on Linux/macOS; copy both guest files to the Windows host.
guest: initramfs.cpio
init: ../init.c
	$(ZIG) cc -target x86_64-linux-musl $(CFLAGS) -static -o $@ $<
initramfs.cpio: init
	printf 'init\n' | cpio -o -H newc > $@

clean:
	rm -f vmmlite.exe vmmlite.pdb init initramfs.cpio tests/machine-test
	rm -rf tests/machine-test.dSYM
