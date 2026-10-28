## TL;DR

The tutorial figures pair source-linked Mermaid views with editable layouts and light/dark SVGs.

## Details

| Figure | Question | Evidence |
| --- | --- | --- |
| [Machine](machine/diagram.md) | Which parts does WHP supply? | Windows source: partition setup, mappings, devices and run loop |
| [API](api/diagram.md) | What goes into and comes back from a run? | Partition creation and WHvRunVirtualProcessor |
| [Interrupts](interrupts/diagram.md) | How do device ACK and LAPIC EOI differ? | console_transmit, IOAPIC delivery, InterruptACK and pc_eoi |
| [Firecracker](firecracker/diagram.md) | How are threads and device backends separated? | Pinned Firecracker design, Threads and device sections |

Each directory contains `diagram.md`, `evidence.json`, `figure.json`, and two SVGs.
The evidence uses `WORKTREE` so future edits can be checked against the source lines
and exact tokens. The WHP figures were drawn against `windows-x86_64/vmmlite.c`
SHA-256 `4b049e4dfa389251e5193a666b3c4a4fedeef551b621ec734fe1b9a407d0dd55`.
The figure footer identifies that source hash, not a Git commit.

The readability revision retains the depicted machine and API paths. Its evidence
windows follow the reorganized source; the artwork's footer continues to identify
the source used for the original drawing. Check each manifest against the current
file rather than treating that historical hash as the current source revision.

Firecracker evidence paths belong to its own repository, not this one. Validate
that manifest against [Firecracker commit 6ec271414300fb8f0ff1ff79ad321bb7467d072f](https://github.com/firecracker-microvm/firecracker/blob/6ec271414300fb8f0ff1ff79ad321bb7467d072f/docs/design.md).
The reference blob is `4d40dffaabe1580f7dfdbca9a77e03660083e684`. No Firecracker code
is embedded or linked into vmmlite.

When updating a figure, recheck its cited source windows, update both the Mermaid
view and layout, render both themes, and inspect them for clipping and misleading
boundaries. The tutorial selects a theme with a `picture` element.

The interrupt figure is one illustrative level-triggered route with notifications
enabled. It does not report the vector or IRQ ordering from a Windows boot. Native
WHP validation remains pending.
