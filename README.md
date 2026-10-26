# vmmlite

## TL;DR

Small C VMMs that boot Linux, print a greeting from PID 1 through a virtio console, and exit
when the guest reboots.

## Details

Choose the example for your host:

| Host | Virtualization API | Code and walkthrough |
| --- | --- | --- |
| Linux, x86_64 | KVM | [linux-x86_64/](linux-x86_64/README.md) |
| macOS, Apple Silicon | Hypervisor.framework | [macos/](macos/README.md) |

Each directory contains its VMM and platform-specific build and boot instructions. The shared
[init.c](init.c) is compiled for the guest architecture and placed in an initramfs as `/init`.

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** These examples run without a
> sandbox and model only a small boot machine. The x86 example does not validate input files
> or guest addresses; the macOS example adds bounds checks but is not hardened. To run code
> in a micro-VM, use a production VMM such as Firecracker or Cloud Hypervisor, or a sandbox
> such as [BoxLite](https://github.com/boxlite-ai/boxlite).

## Learn more

- [Building a VMM from scratch](https://blog.boxlite.ai/building-a-vmm-from-scratch-0-why-boxlite-is-replacing-libkrun), the BoxLite blog series this repo accompanies
- [BoxLite VMM design: Boot path](https://github.com/boxlite-ai/boxlite/blob/main/docs/contributing/architecture/vmm/README.md#boot-path)

## License

[Apache-2.0](LICENSE)
