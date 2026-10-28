# vmmlite on Linux ARM64

## TL;DR

A standalone KVM example boots an ARM64 Linux Image, prints the shared PID 1 greeting,
and exits when the guest requests PSCI reset. [macos-arm64/](../macos-arm64/README.md) is the Apple Silicon example.

## Build and run

Use a little-endian AArch64 Linux host with GICv3, PSCI support, and read/write access to
`/dev/kvm`. Virtual machines also need working nested virtualization. A cross-compiler
or a Docker container without usable KVM cannot establish that this example boots.

On Ubuntu/Debian, run from the repository root:

```sh
sudo apt-get install build-essential libfdt-dev device-tree-compiler cpio curl python3
make build:linux-aarch64
make kernel:linux-aarch64
make run:linux-aarch64
make test:linux-aarch64
```

The root [Makefile](../Makefile) writes generated files to `.build/linux-aarch64/` and
downloads and checks the SHA-256 of the same Linux 6.1.102 ARM64 fixture used by `macos-arm64/`.
Despite its upstream filename `vmlinux`, this fixture is a raw `Image`, not ELF.
Supply another compatible raw Image with `make run:linux-aarch64 KERNEL=/path/to/Image`.
It needs built-in GICv3, architected timers, PSCI, initramfs, ELF, OF 8250, virtio-MMIO and virtio-console drivers.

The early log uses an MMIO 8250 UART, then Linux switches to the virtio console `hvc0`.
KVM owns GICv3 and the architected timers; `machine.dts` describes the guest hardware.

`make test:linux-aarch64` exercises production address/loader/UART code, then requires three real
Linux boots, a timed wakeup, 300 ordered guest writes and rejection of malformed files.
Logs are retained under `.build/linux-aarch64/`. A successful run must print `Hello from PID 1!`
once and `guest requested PSCI shutdown/reset`, then exit zero.

## Limits

For learning only. One vCPU, 512 MiB RAM, output only; no firmware, disks, network,
SMP or host sandbox. Bounds checks do not make this a production security boundary.
The standalone examples intentionally duplicate small device helpers to keep each
platform's boot path readable. See [the implementation design](https://github.com/boxlite-ai/vmmlite/issues/17).
