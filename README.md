## TL;DR

vmmlite is a small, educational VMM that boots Linux to a PID 1 which prints one line
through a virtio console and reboots.

## Details

> [!WARNING]
> **For learning only. Do not use vmmlite in production.** These examples use limited device models
> and run without host-process sandboxing. Validation differs between implementations and does not
> make either suitable for untrusted workloads. To run code in a micro-VM, use a production VMM such as
> Firecracker or Cloud Hypervisor, or a sandbox such as [BoxLite](https://github.com/boxlite-ai/boxlite).

## Platforms

Each host implementation lives in a directory named for its operating system and CPU architecture.

| Host | CPU architecture | Hypervisor | Build and run |
| --- | --- | --- | --- |
| Linux | x86_64 | KVM | [Linux x86_64 guide](linux-x86_64/README.md) |
| Windows | x86_64 | WHP | [Windows x86_64 guide](windows-x86_64/README.md) — native boot validation pending |

## Repository layout

- [linux-x86_64/vmmlite.c](linux-x86_64/vmmlite.c) creates the VM and emulates its devices on Linux/KVM.
- [windows-x86_64/](windows-x86_64/README.md) contains the WHP implementation, portable device tests,
  and a bounded Windows boot harness. Its [walkthrough](windows-x86_64/tutorial.md) explains the code.
- [init.c](init.c) runs as PID 1 inside the Linux guest. It is shared guest code; each host
  implementation builds it into its own initramfs.
- [.github/workflows/boot.yml](.github/workflows/boot.yml) builds and boots the Linux x86_64 example.

The Linux guide contains the quick start, boot walkthrough, kernel requirements, and tested output.
Run its commands from `linux-x86_64/`; the guest source remains at `../init.c`.

## Learn more

- [Building a VMM from scratch](https://blog.boxlite.ai/building-a-vmm-from-scratch-0-why-boxlite-is-replacing-libkrun), the BoxLite blog series this repo accompanies
- [BoxLite VMM design: Boot path](https://github.com/boxlite-ai/boxlite/blob/main/docs/contributing/architecture/vmm/README.md#boot-path)
- The [part-0.5 tag](https://github.com/boxlite-ai/vmmlite/tree/part-0.5) preserves the original 190-line version without the virtio console.

## License

[Apache-2.0](LICENSE)
