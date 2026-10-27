# Walkthrough figures

Each figure has a live Mermaid source and light/dark SVGs. The walkthrough selects
an SVG for the reader’s theme; Notion uses rendered light PNGs from the same layout.

| View | Mermaid source | Implementation or upstream source |
| --- | --- | --- |
| Machine | [machine.md](machine.md) | [vmmlite.c](../../vmmlite.c), [machine.dts](../../machine.dts) |
| API objects | [api.md](api.md) | [vmmlite.c](../../vmmlite.c) |
| Console interrupt | [irq.md](irq.md) | [vmmlite.c](../../vmmlite.c), [Linux 6.1.102 virtio-MMIO](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/virtio/virtio_mmio.c?h=v6.1.102) |
| Firecracker comparison | [firecracker.md](firecracker.md) | [Firecracker design at 6ec2714](https://github.com/firecracker-microvm/firecracker/blob/6ec271414300fb8f0ff1ff79ad321bb7467d072f/docs/design.md) |

When behavior changes, update the Mermaid and both SVG variants together. Keep the
machine overview, API ownership, and interrupt sequence separate. The Linux ARM64
console uses edge-triggered irqfd; its UART uses a separate level-triggered SPI.
