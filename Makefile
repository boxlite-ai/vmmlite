# Run from the repository root. Each platform keeps its own generated files.
.DEFAULT_GOAL := help
.DELETE_ON_ERROR:
# Delay colons in prerequisite names for GNU Make 3.81.
.SECONDEXPANSION:
colon := :
LINUX_CC ?= gcc
MACOS_CC ?= xcrun clang
MACOS_GUEST_CC ?= aarch64-linux-musl-gcc
HOST_CC ?= cc
ZIG ?= zig
DTC ?= dtc
CFLAGS := -O2 -Wall -Wextra -Werror
WINDOWS_CFLAGS = -std=c11 $(CFLAGS)
DTC_PREFIX ?= $(shell brew --prefix dtc)
MACOS_CFLAGS = $(CFLAGS) -g -mmacosx-version-min=15.0 -I$(DTC_PREFIX)/include
MACOS_LIBS = $(DTC_PREFIX)/lib/libfdt.a -framework Hypervisor
LINUX_KERNEL = $(if $(KERNEL),$(KERNEL),.build/linux-x86_64/bzImage)
LINUX_ARM64_KERNEL = $(if $(KERNEL),$(KERNEL),.build/linux-aarch64/Image)
MACOS_KERNEL = $(if $(KERNEL),$(KERNEL),.build/macos-arm64/Image)
WINDOWS_KERNEL = $(if $(KERNEL),$(KERNEL),.build/windows-x86_64/bzImage)

# Native Windows builds use PowerShell for filesystem operations.
ifeq ($(OS),Windows_NT)
mkdir = powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(1)' -ErrorAction Stop | Out-Null"
copy = powershell -NoProfile -Command "Copy-Item -LiteralPath '$(1)' -Destination '$(2)' -ErrorAction Stop"
remove = powershell -NoProfile -Command "if (Test-Path -LiteralPath '$(1)') { Remove-Item -LiteralPath '$(1)' -Recurse -Force -ErrorAction Stop }"
else
mkdir = mkdir -p "$(1)"
copy = cp "$(1)" "$(2)"
remove = rm -rf "$(1)"
endif

.PHONY: help build$$(colon)linux-x86_64 build$$(colon)linux-aarch64 build$$(colon)macos-arm64 build$$(colon)windows-x86_64
.PHONY: guest$$(colon)linux-x86_64 guest$$(colon)linux-aarch64 guest$$(colon)macos-arm64 guest$$(colon)windows-x86_64
.PHONY: run$$(colon)linux-x86_64 run$$(colon)linux-aarch64 run$$(colon)macos-arm64 boot$$(colon)windows-x86_64
.PHONY: test$$(colon)linux-x86_64 test$$(colon)linux-aarch64 test$$(colon)macos-arm64 test$$(colon)windows-x86_64
.PHONY: clean$$(colon)linux-x86_64 clean$$(colon)linux-aarch64 clean$$(colon)macos-arm64 clean$$(colon)windows-x86_64
.PHONY: kernel$$(colon)linux-aarch64 kernel$$(colon)macos-arm64 check$$(colon)windows-x86_64 check-image$$(colon)windows-x86_64

help:
	@echo "make build:linux-x86_64 | build:linux-aarch64 | build:macos-arm64 | build:windows-x86_64"
	@echo "make test:<platform> | guest:<platform> | clean:<platform>"
	@echo "make run:linux-x86_64 | run:linux-aarch64 | run:macos-arm64 | boot:windows-x86_64 [KERNEL=/path/to/kernel]"
	@echo "make kernel:linux-aarch64 | kernel:macos-arm64 | check:windows-x86_64 | check-image:windows-x86_64"

# Shared guest source and archive recipe; the compiler follows the guest architecture.
.build/linux-x86_64/init .build/linux-aarch64/init .build/linux-aarch64/timer/init: GUEST_CC = $(LINUX_CC)
.build/linux-aarch64/init .build/linux-aarch64/timer/init: GUEST_CFLAGS = -g
.build/macos-arm64/init .build/macos-arm64/timer/init: GUEST_CC = $(MACOS_GUEST_CC)
.build/windows-x86_64/init: GUEST_CC = $(ZIG) cc -target x86_64-linux-musl
.build/windows-x86_64/init: GUEST_CFLAGS = -std=c11
.SECONDARY: .build/linux-x86_64/init .build/linux-aarch64/init .build/macos-arm64/init .build/windows-x86_64/init
.SECONDARY: .build/linux-aarch64/timer/init .build/macos-arm64/timer/init
.build/linux-x86_64/init .build/linux-aarch64/init .build/macos-arm64/init .build/windows-x86_64/init: init.c Makefile
	$(call mkdir,$(@D))
	$(GUEST_CC) $(GUEST_CFLAGS) $(CFLAGS) -static -o "$@" "$<"

.build/%/initramfs.cpio: .build/%/init
	cd "$(@D)" && printf 'init\n' | cpio -o -H newc > initramfs.cpio

guest\:linux-x86_64: .build/linux-x86_64/initramfs.cpio
guest\:linux-aarch64: .build/linux-aarch64/initramfs.cpio
guest\:macos-arm64: .build/macos-arm64/initramfs.cpio
guest\:windows-x86_64: .build/windows-x86_64/initramfs.cpio

# ARM64 examples share the image, device-tree and timer-guest build steps.
.build/macos-arm64/machine.dtb: DTC = $(DTC_PREFIX)/bin/dtc
.build/%/machine.dtb: %/machine.dts Makefile
	$(call mkdir,$(@D))
	$(DTC) -I dts -O dtb -o "$@" "$<"

.build/linux-aarch64/Image: SHA256 = sha256sum
.build/macos-arm64/Image: SHA256 = shasum -a 256
.build/linux-aarch64/Image .build/macos-arm64/Image:
	$(call mkdir,$(@D))
	curl -fL --retry 2 --connect-timeout 30 --max-time 180 -o "$@.tmp" https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102
	printf 'c16c673b970b92a57b6afc0721c79cb90503900b5928c887f1f5eee508d3920a  %s\n' '$@.tmp' | $(SHA256) -c -
	mv "$@.tmp" "$@"

.build/%/timer/init: %/tests/timer.c Makefile
	$(call mkdir,$(@D))
	$(GUEST_CC) $(GUEST_CFLAGS) $(CFLAGS) -static -o "$@" "$<"

.build/%/timer.cpio: .build/%/timer/init
	cd "$(@D)/timer" && printf 'init\n' | cpio -o -H newc > ../timer.cpio

# Linux x86_64: KVM and the host's Linux toolchain.
build\:linux-x86_64: .build/linux-x86_64/vmmlite guest$$(colon)linux-x86_64
.build/linux-x86_64/vmmlite: linux-x86_64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(LINUX_CC) $(CFLAGS) -pthread -o "$@" "$<"

run\:linux-x86_64: build$$(colon)linux-x86_64 $(LINUX_KERNEL)
	.build/linux-x86_64/vmmlite "$(LINUX_KERNEL)" .build/linux-x86_64/initramfs.cpio

test\:linux-x86_64: build$$(colon)linux-x86_64 $(LINUX_KERNEL)
	timeout 120 .build/linux-x86_64/vmmlite "$(LINUX_KERNEL)" .build/linux-x86_64/initramfs.cpio > .build/linux-x86_64/boot.log 2>&1 || { cat .build/linux-x86_64/boot.log; exit 1; }
	cat .build/linux-x86_64/boot.log
	grep -q 'Hello from PID 1!' .build/linux-x86_64/boot.log

# Linux AArch64: KVM, libfdt and a native Linux toolchain.
build\:linux-aarch64: .build/linux-aarch64/vmmlite .build/linux-aarch64/machine.dtb guest$$(colon)linux-aarch64
.build/linux-aarch64/vmmlite: linux-aarch64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(LINUX_CC) $(CFLAGS) -g "$<" -lfdt -pthread -o "$@"

.build/linux-aarch64/test_native: linux-aarch64/tests/test_native.c linux-aarch64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(LINUX_CC) $(CFLAGS) -g "$<" -lfdt -pthread -o "$@"

kernel\:linux-aarch64: .build/linux-aarch64/Image

run\:linux-aarch64: build$$(colon)linux-aarch64 $(LINUX_ARM64_KERNEL)
	.build/linux-aarch64/vmmlite "$(LINUX_ARM64_KERNEL)" .build/linux-aarch64/initramfs.cpio .build/linux-aarch64/machine.dtb

test\:linux-aarch64: build$$(colon)linux-aarch64 $(LINUX_ARM64_KERNEL) .build/linux-aarch64/test_native .build/linux-aarch64/timer.cpio
	.build/linux-aarch64/test_native "$(LINUX_ARM64_KERNEL)"
	python3 linux-aarch64/tests/test_boot.py "$(LINUX_ARM64_KERNEL)"

# macOS arm64: Hypervisor.framework, libfdt, signing and an ARM64 Linux guest.
build\:macos-arm64: .build/macos-arm64/vmmlite .build/macos-arm64/machine.dtb guest$$(colon)macos-arm64
.build/macos-arm64/vmmlite: macos-arm64/vmmlite.c macos-arm64/entitlements.plist Makefile
	$(call mkdir,$(@D))
	$(MACOS_CC) $(MACOS_CFLAGS) "$<" $(MACOS_LIBS) -o "$@"
	codesign --force --sign - --entitlements macos-arm64/entitlements.plist "$@"

kernel\:macos-arm64: .build/macos-arm64/Image

.build/macos-arm64/test_native: macos-arm64/tests/test_native.c macos-arm64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(MACOS_CC) $(MACOS_CFLAGS) "$<" $(MACOS_LIBS) -o "$@"

run\:macos-arm64: build$$(colon)macos-arm64 $(MACOS_KERNEL)
	.build/macos-arm64/vmmlite "$(MACOS_KERNEL)" .build/macos-arm64/initramfs.cpio .build/macos-arm64/machine.dtb

test\:macos-arm64: build$$(colon)macos-arm64 $(MACOS_KERNEL) .build/macos-arm64/test_native .build/macos-arm64/timer.cpio
	.build/macos-arm64/test_native "$(MACOS_KERNEL)"
	python3 macos-arm64/tests/test_boot.py "$(MACOS_KERNEL)"

# Windows x86_64: cross-build with Zig; prepare the Linux guest on Linux/macOS.
build\:windows-x86_64: .build/windows-x86_64/vmmlite.exe .build/windows-x86_64/test-boot.ps1
.build/windows-x86_64/vmmlite.exe: windows-x86_64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(ZIG) cc -target x86_64-windows-gnu $(WINDOWS_CFLAGS) -o "$@" "$<" -lwinhvplatform -lwinhvemulation

.build/windows-x86_64/test-boot.ps1: windows-x86_64/test-boot.ps1 Makefile
	$(call mkdir,$(@D))
	$(call copy,$<,$@)

.build/windows-x86_64/machine-test: windows-x86_64/tests/machine_test.c windows-x86_64/vmmlite.c Makefile
	$(call mkdir,$(@D))
	$(HOST_CC) $(WINDOWS_CFLAGS) -g -fsanitize=address,undefined -o "$@" "$<"

test\:windows-x86_64: .build/windows-x86_64/machine-test
	./.build/windows-x86_64/machine-test

check\:windows-x86_64: test$$(colon)windows-x86_64 build$$(colon)windows-x86_64

check-image\:windows-x86_64: .build/windows-x86_64/machine-test guest$$(colon)windows-x86_64
	./.build/windows-x86_64/machine-test "$(WINDOWS_KERNEL)" .build/windows-x86_64/initramfs.cpio

boot\:windows-x86_64: build$$(colon)windows-x86_64
	powershell -NoProfile -File .build/windows-x86_64/test-boot.ps1 -Kernel "$(WINDOWS_KERNEL)" -Initramfs .build/windows-x86_64/initramfs.cpio

clean\:linux-x86_64 clean\:linux-aarch64 clean\:macos-arm64 clean\:windows-x86_64:
	$(call remove,.build/$(patsubst clean:%,%,$@))
