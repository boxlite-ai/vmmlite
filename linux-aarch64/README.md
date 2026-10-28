# vmmlite on Linux ARM64

## TL;DR

A standalone KVM example boots an ARM64 Linux Image, prints the shared PID 1 greeting,
and exits when the guest requests PSCI reset. This directory contains only this README,
`vmmlite.c`, and `machine.dts`; the guest program is the shared [../init.c](../init.c).

## Build and run

Use a little-endian AArch64 Linux host with GICv3, PSCI support, and read/write access to
`/dev/kvm`. Virtual machines also need working nested virtualization. A cross-compiler
or a Docker container without usable KVM cannot establish that this example boots.

Install the dependencies on Ubuntu/Debian:

```sh
sudo apt-get install build-essential libfdt-dev device-tree-compiler cpio curl
```

From the repository root, run these commands in one shell. The VMM, compiled device
tree, static guest program, initramfs and kernel are written to a temporary directory.

```sh
set -eu
cd linux-aarch64
build_dir=$(mktemp -d /tmp/vmmlite-linux-aarch64.XXXXXX)

gcc -O2 -g -Wall -Wextra -Werror vmmlite.c -lfdt -pthread -o "$build_dir/vmmlite"
dtc -I dts -O dtb -o "$build_dir/machine.dtb" machine.dts
gcc -O2 -g -Wall -Wextra -Werror -static -o "$build_dir/init" ../init.c
(cd "$build_dir" && printf 'init\n' | cpio -o -H newc > initramfs.cpio)

curl -fL --retry 2 --connect-timeout 30 --max-time 180 \
  -o "$build_dir/Image.tmp" \
  https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102
printf 'c16c673b970b92a57b6afc0721c79cb90503900b5928c887f1f5eee508d3920a  %s\n' \
  "$build_dir/Image.tmp" | sha256sum -c -
mv "$build_dir/Image.tmp" "$build_dir/Image"

"$build_dir/vmmlite" "$build_dir/Image" "$build_dir/initramfs.cpio" "$build_dir/machine.dtb"
```

The download is the same Linux 6.1.102 ARM64 fixture used by `macos/`. Despite its
upstream filename `vmlinux`, it is a raw `Image`, not ELF. The commands check its
SHA-256 before using it. To use another compatible raw Image, replace
`"$build_dir/Image"` in the final command with its path.
It needs built-in GICv3, architected timers, PSCI, initramfs, ELF, OF 8250, virtio-MMIO
and virtio-console drivers.

The early log uses an MMIO 8250 UART, then Linux switches to the virtio console `hvc0`.
KVM owns GICv3 and the architected timers; `machine.dts` describes the guest hardware.
The guest `init` is statically linked so its initramfs needs no dynamic loader or libraries.

A successful run prints `Hello from PID 1!` once and
`guest requested PSCI shutdown/reset`, then exits zero.

## Limits

For learning only. One vCPU, 512 MiB RAM, output only; no firmware, disks, network,
SMP or host sandbox. Bounds checks do not make this a production security boundary.
The standalone examples intentionally duplicate small device helpers to keep each
platform's boot path readable. See [the implementation design](https://github.com/boxlite-ai/vmmlite/issues/17).
