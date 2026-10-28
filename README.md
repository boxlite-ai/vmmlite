# vmmlite

## TL;DR

Small C VMMs that boot Linux, print a greeting from PID 1 through a virtio console, and exit
when the guest reboots.

## Details

Choose the example for your host:

| Host | Virtualization API | Code and walkthrough |
| --- | --- | --- |
| Linux, x86_64 | KVM | [linux-x86_64/](linux-x86_64/README.md) |
| macOS, arm64 (Apple Silicon) | Hypervisor.framework | [macos-arm64/](macos-arm64/README.md) |
| Windows, x86_64 | WHP | [windows-x86_64/](windows-x86_64/README.md) — native boot validation pending |

Each directory contains its VMM and platform-specific build and boot instructions. The shared
[init.c](init.c) is compiled for the guest architecture and placed in an initramfs as `/init`.

Build from the repository root with the shared [Makefile](Makefile):

```sh
make build:linux-x86_64
make build:macos-arm64
make build:windows-x86_64
```

Choose the target for your host; the Windows executable can also be cross-built on Linux/macOS.
Outputs stay in `.build/<platform>/`. `make test:<platform>` runs that platform's checks,
and `make clean:<platform>` removes only its output directory. The Windows test target runs
portable device checks; `make boot:windows-x86_64` runs the native acceptance harness.
Use `make help` for the remaining targets and each platform README for prerequisites.

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** These examples run without a
> sandbox and model only a small boot machine. The Linux/KVM example does not validate input
> files or guest addresses; the macOS and WHP examples add bounds checks but are not hardened. To run code
> in a micro-VM, use a production VMM such as Firecracker or Cloud Hypervisor, or a sandbox
> such as [BoxLite](https://github.com/boxlite-ai/boxlite).

## Learn more

- [Building a VMM from scratch](https://blog.boxlite.ai/building-a-vmm-from-scratch-0-why-boxlite-is-replacing-libkrun), the BoxLite blog series this repo accompanies
- [BoxLite VMM design: Boot path](https://github.com/boxlite-ai/boxlite/blob/main/docs/contributing/architecture/vmm/README.md#boot-path)

## License

[Apache-2.0](LICENSE)
