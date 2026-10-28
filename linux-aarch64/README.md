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

From the repository root:

```sh
make build:linux-aarch64
make kernel:linux-aarch64
make run:linux-aarch64
```

The root [Makefile](../Makefile) builds the VMM, compiles [machine.dts](machine.dts),
and packs the static guest [init.c](../init.c). Generated files stay in
`.build/linux-aarch64/`; the platform directory keeps only its three source files.

`make kernel:linux-aarch64` downloads the same Linux 6.1.102 ARM64 fixture used by
[macos-arm64/](../macos-arm64/README.md) and checks its SHA-256. Despite its upstream
filename `vmlinux`, it is a raw `Image`, not ELF. Use another compatible Image with
`make run:linux-aarch64 KERNEL=/absolute/path/to/Image`.
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
