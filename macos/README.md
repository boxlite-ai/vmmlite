# vmmlite on macOS: boot Linux with Hypervisor.framework

## TL;DR

A small C VMM boots ARM64 Linux on an Apple Silicon Mac, prints from PID 1 through a virtio console, and exits when Linux requests a reset.

## Details

This is the macOS companion to [vmmlite's x86/KVM example](../README.md). The Mac is the host; Linux is the guest. It uses the low-level Hypervisor.framework C API, so the loader, guest memory, device registers, virtqueues, and exit loop stay visible.

**For learning only.** Bounds checks catch common malformed inputs, but this is not a security boundary for untrusted guests. There is one CPU, one output console, and no disk, network, interactive input, firmware image, or snapshot support. It does not claim to implement every virtio device requirement or every ARM platform feature.

## Build and run

You need an Apple Silicon Mac, Xcode Command Line Tools or Xcode, `make`, `cpio`, Python 3 for the tests, `dtc`/libfdt, and a compiler that produces static ARM64 Linux executables. The GIC APIs require macOS 15 or later.

Install the two additional tools with Homebrew:

```sh
brew install dtc
brew install filosottile/musl-cross/musl-cross
```

From the repository root:

```sh
make -C macos all
make -C macos kernel
make -C macos test
make -C macos run
```

The Makefile builds the host executable with Apple's Clang, links Hypervisor.framework and libfdt, then applies an ad-hoc signature containing `com.apple.security.hypervisor`. Signing authorizes use of the hypervisor API; it does not sandbox the process. The guest's existing [init.c](../init.c) is compiled separately with `aarch64-linux-musl-gcc -static`. A Mach-O executable built by the Mac compiler cannot serve as Linux's PID 1.

`make kernel` fetches a fixed, public [Linux 6.1.102 ARM64 test kernel](https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102) published by Firecracker. Its SHA-256 is checked before it becomes `.build/Image`:

```text
c16c673b970b92a57b6afc0721c79cb90503900b5928c887f1f5eee508d3920a
```

This older kernel is a reproducible learning fixture, not a production recommendation. Its [configuration](https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102.config) builds in initramfs, the 8250 early console, ARM timers, GICv3, virtio-mmio, and virtio-console. A driver set to `m` cannot load from our initramfs, which contains only `/init`.

You can provide another compatible uncompressed ARM64 `Image`:

```sh
make -C macos test KERNEL=/absolute/path/to/Image
make -C macos run KERNEL=/absolute/path/to/Image
```

Use `GUEST_CC=aarch64-linux-gnu-gcc` for an alternative compiler with static Linux libc, or `DTC_PREFIX=/path/to/dtc-install` for another dtc installation. The C source targets macOS 15; libfdt must also be built for the macOS version on which you intend to run the executable. A library from a newer Homebrew bottle can impose a newer minimum.

## The machine at a glance

```text
                 vmmlite: one Mac process, one execution thread
       +------------------------------------------------------------+
       | Linux loader | RAM | device tree | early UART | virtio TX  |
       +--------------+-----+-------------+------------+------------+
                      | hv_vm_map              ^ MMIO exits
                      |                        | HVC requests
                      v                        |
       +------------------------------------------------------------+
       | Hypervisor.framework: VM, vCPU, stage-2 mappings, GICv3     |
       | GIC distributor + redistributor + CPU interface; timers    |
       +------------------------------------------------------------+
                      | hv_vcpu_run            ^
                      v                        |
       +------------------------------------------------------------+
       | Guest Linux: EL1 kernel -> EL0 /init                       |
       | /init writes -> hvc0 -> virtqueue 1 -> console doorbell     |
       +------------------------------------------------------------+
                      Apple Silicon executes guest instructions
```

| Part | What it does | Who supplies it |
| --- | --- | --- |
| vCPU | Executes the kernel and its user program; preserves their register state | CPU and Hypervisor.framework |
| Guest RAM | Stores Linux, its page tables, the device tree, initramfs, and console buffers | A 512 MiB mapping in vmmlite |
| Stage-2 translation | Maps guest physical addresses to the host's memory pages | Apple's hypervisor |
| GIC distributor | Routes the virtio console's shared interrupt | Framework GICv3 |
| GIC redistributor | Holds per-CPU interrupt state, including timer interrupts | Framework GICv3 |
| GIC CPU interface | Lets Linux acknowledge and finish interrupts through system registers | Framework GICv3 |
| ARM virtual timer | Lets Linux schedule wakeups against a counter | CPU and framework |
| Early UART | Sends the first kernel log bytes to the Mac terminal | vmmlite's MMIO handler |
| Virtio console | Transfers output in buffers through guest RAM | vmmlite's queue handler |
| Device tree | Describes the machine and passes boot arguments and initramfs bounds | DTS compiled by dtc, patched by libfdt |
| PSCI | Answers basic firmware calls; reset/off ends this VMM | vmmlite's HVC handler |

The UART exists only for early output. The DTS does not advertise a full 16550 device. Linux discovers its address from `earlycon=uart8250,mmio,0x09000000`, then switches to `hvc0` once virtio-console registers.

## 0. Background: ARM64, the hypervisor, and devices

### 0.1 What the ARM64 kernel controls

ARM64 separates privilege into exception levels. In this guest, Linux runs at EL1 and `/init` runs at EL0. The host hypervisor controls the guest's execution through virtualization support at EL2. This program does not expose a guest EL2 or run a nested hypervisor.

The CPU still executes ordinary guest instructions. Linux installs exception handlers, constructs page tables, schedules processes, and handles system calls itself.

| State | Purpose | At this example's entry |
| --- | --- | --- |
| PC | Address of the next instruction | Start of the Linux Image |
| PSTATE, exposed as `HV_REG_CPSR` | Privilege, stack selection, and interrupt masks | `0x3c5`: EL1h, D/A/I/F masked |
| `SCTLR_EL1` | Includes MMU and cache controls | `0x30d00800`: MMU off, required baseline bits set |
| `MPIDR_EL1` | CPU affinity used to match the CPU to its GIC redistributor | Affinity 0 |
| `x0` | First boot argument | Device-tree physical address |
| `x1`–`x3` | Reserved boot arguments | Zero |
| `TTBR*_EL1`, `TCR_EL1`, `MAIR_EL1` | Page tables, translation control, memory attributes | Linux configures them |
| `VBAR_EL1` | Exception-vector address | Linux installs its vectors |

The `h` in EL1h selects EL1's stack pointer. The four DAIF masks defer debug, SError, IRQ, and FIQ exceptions until Linux is ready. These are different from x86's long-mode setup: vmmlite does not build an ARM GDT or x86-style boot page tables.

**In the code:** section 6 sets the entry registers. The [ARM64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html) defines their contract.

### 0.2 VM entry and VM exit

`hv_vcpu_run(cpu)` enters the guest. It returns when the framework needs the VMM to handle an exit. An exit is not a process crash: it is a controlled transfer of execution back to the host.

There are three useful paths:

```text
guest arithmetic, page-table walks, system calls
    -> CPU / guest Linux continue without this C loop

guest GIC register access or timer interrupt
    -> framework-owned interrupt machinery

guest accesses console MMIO, or executes a firmware HVC
    -> hv_vcpu_run returns -> vmmlite handles it -> run again
```

A call can contain many hardware transitions that never return to the application. Counting iterations of the C loop would therefore not count every hardware VM exit.

The program has one execution thread. When it handles an exit, its one guest CPU is stopped. That is why console queue state needs no worker mutex here. Adding another vCPU or an asynchronous device would change that ownership rule.

### 0.3 Memory is translated twice

Once Linux enables its MMU, its memory accesses follow two translations:

```text
guest virtual address
    | Linux's page tables: stage 1
    v
guest physical address, for example 0x40080000
    | mappings created through hv_vm_map: stage 2
    v
host RAM backing vmmlite's mmap
```

The host pointer and the guest address are not interchangeable. With RAM starting at `0x40000000`, guest address `X` is the byte at:

```c
ram + (X - RAM_BASE)
```

`guest(address, size)` checks the whole range before making this conversion. Its subtraction-based bound avoids an overflowing `address + size`. The loader also keeps the kernel away from the DTB and initramfs regions.

Before Linux enables its own translation, the entry PC and `x0` already refer to guest physical addresses. Stage 2 still determines which host pages the guest can reach.

### 0.4 What an exit tells the VMM

The framework gives `hv_vcpu_create` a pointer to an `hv_vcpu_exit_t`. After a run returns, the VMM reads its reason and, for an exception, its syndrome and address.

For an MMIO data abort, the syndrome supplies:

| ESR field | Meaning | Use here |
| --- | --- | --- |
| EC, bits 31:26 | Exception class | `0x24` selects a lower-level data abort |
| ISV, bit 24 | Whether access information is valid | Reject an undecodable access |
| SAS, bits 23:22 | Access size | `1 << SAS` bytes |
| SSE, bit 21 | Sign extension on a load | Extend the reply when requested |
| SRT, bits 20:16 | Source or destination register | Read a store value or return a load value |
| SF, bit 15 | 32-bit or 64-bit destination | Truncate a 32-bit result |
| WnR, bit 6 | Store or load | Select register write or read |

Register number 31 is the zero register for these accesses. A store supplies zero; a load discards the value.

After emulating MMIO, vmmlite advances PC by four bytes. An HVC is different: the returned PC is already after that instruction. Advancing it again would skip a guest instruction. The distinction is also visible in [QEMU's HVF exception handler](https://github.com/qemu/qemu/blob/48396ad6ce9ac4ec5ec97d45435c947b582c7f39/target/arm/hvf/hvf.c#L2073-L2293).

Unexpected addresses, widths, exception classes, and unimplemented system-register accesses stop with an error. Successful PSCI shutdown is a separate path.

### 0.5 The API and its creation order

The setup sequence is:

```text
hv_vm_create
    -> hv_vm_map
    -> hv_gic_config_create
    -> set distributor and redistributor bases
    -> hv_gic_create
    -> hv_vcpu_create
    -> set CPU affinity and Linux entry registers
    -> hv_vcpu_run, repeatedly
    -> hv_vcpu_destroy
    -> hv_vm_destroy
```

Apple requires the GIC to be created after the VM and before its vCPUs. Its per-CPU resources are allocated as CPUs are created. Affinity must be established before execution starts. See [GIC creation](https://developer.apple.com/documentation/hypervisor/hv_gic_create(_:)).

This replaces KVM's file descriptors, ioctls, and shared `kvm_run` mapping with direct C calls and a framework-owned exit record. The responsibilities remain recognizable: register memory, initialize a CPU, enter it, and handle devices.

### 0.6 Devices are addresses

ARM64 has no x86 `IN`/`OUT` port-I/O path. Our devices use memory-mapped I/O: a load or store to an address has device semantics.

The RAM mapping deliberately excludes the UART and virtio windows. Their accesses return to vmmlite as data aborts. In contrast, the GIC windows belong to the framework and are not handled as console MMIO.

The early UART implements just what the early 8250 printer needs:

| Offset | Read | Write |
| --- | --- | --- |
| 0 | Zero | Print the low byte to host stdout |
| 5 | `0x60`: transmitter and holding register empty | Ignored |
| Other offsets within its eight bytes | Zero | Ignored |

There is no UART receive queue, baud-rate model, FIFO, or UART interrupt. It is an early output device, not an interactive serial terminal.

### 0.7 How Linux learns its machine

A device tree is structured data, not executable firmware. Linux receives its address in `x0`; the nodes name hardware and driver-compatible interfaces.

The readable [machine.dts](machine.dts) declares:

- A single CPU with affinity 0 and 512 MiB of RAM.
- GICv3, the ARM timer, and the virtio-MMIO console.
- PSCI calls through HVC.
- The early console command line and placeholders for initramfs bounds.

At startup, libfdt checks the input and patches the initramfs start/end, GIC region sizes, and timer interrupt numbers queried from the framework. It then packs the tree. Using libfdt avoids hiding a second binary-format implementation inside the teaching VMM.

For the console, `interrupts = <0 1 4>` means SPI, number 1 within the SPI class, level-high. SPI numbering begins at interrupt ID 32, so the host raises **INTID 33**. PPI numbers are relative to 16. Confusing these two numbering schemes can leave a device operational but unable to interrupt Linux. The [GICv3 binding](https://www.kernel.org/doc/Documentation/devicetree/bindings/interrupt-controller/arm,gic-v3.yaml) specifies those cells.

### 0.8 Interrupts and time

The console's completion path is:

```text
vmmlite writes a used-ring entry
    -> publishes used.idx
    -> sets InterruptStatus bit 0
    -> hv_gic_set_spi(33, true)
    -> GIC delivers an IRQ to Linux
    -> Linux reads InterruptStatus and writes InterruptACK
    -> vmmlite lowers INTID 33
```

A level interrupt stays asserted until the device lowers it. Acknowledging the CPU's interrupt-controller entry does not, by itself, acknowledge the device condition.

The timer is a per-CPU interrupt. With the framework GIC enabled, the framework handles the timer's delivery and idle wait machinery. This example does not implement a timer thread or a software GIC's timer-mask protocol. The separate `HV_EXIT_REASON_VTIMER_ACTIVATED` interface matters to VMMs supplying their own interrupt controller; it is not the path this configuration expects.

The guest timer test blocks in `nanosleep`, checks that at least 200 ms elapsed, then produces 300 ordered lines and reboots. Completing that test exercises scheduling, wakeup, and further console execution.

### 0.9 Virtqueues: registers for setup, RAM for data

The virtio console presents modern MMIO version 2 and device ID 3. It offers only `VIRTIO_F_VERSION_1`. It does not negotiate indirect descriptors, event-index notifications, multichannel consoles, or packed rings.

Linux creates two split queues: queue 0 receives input and queue 1 transmits output. We keep queue 0's setup but never produce input. Queue 1 has three guest-memory areas:

```text
descriptor table                 available ring              used ring
+---------------------------+    +---------------------+     +----------------------+
| address, length, flags,   |    | idx                  |     | idx                  |
| next descriptor          |<---| descriptor heads    |     | completed head, len  |
+---------------------------+    +---------------------+     +----------------------+
    points to output bytes          written by Linux            written by vmmlite
```

The register accesses negotiate features, select a queue, set its size and addresses, and mark it ready. Writing queue number 1 to `QueueNotify` starts output processing.

| Offset | Register | What this example does |
| --- | --- | --- |
| `0x000`–`0x010` | Magic, version, device ID, features | Identifies a modern virtio console |
| `0x014`, `0x024` | Feature selectors | Selects a 32-bit half of the feature bits |
| `0x020` | DriverFeatures | Records accepted bits |
| `0x030`–`0x044` | Queue selection, maximum, size, ready | Tracks two queues, up to 256 entries each |
| `0x050` | QueueNotify | Processes transmit queue 1 |
| `0x060`, `0x064` | InterruptStatus, InterruptACK | Reports and clears completed-buffer interrupts |
| `0x070` | Status | Checks VERSION_1 acceptance; zero resets the device |
| `0x080`–`0x0a4` | Descriptor, available, used addresses | Assembles their low and high halves |

For each available head, `console_kick` follows its descriptor chain and prints its readable buffers. It bounds the chain length by the queue size, checks every index and guest range, and rejects writable or indirect descriptors. It then puts the head into the used ring with length zero: the device consumed bytes, but wrote none into the supplied buffers.

The 16-bit counters wrap naturally; a slot is selected modulo the queue size. A release store publishes the new used index after the completion entry. Finally, the device raises its interrupt.

Unlike the KVM version's eventfd worker, this handler runs synchronously on an MMIO exit. That costs a return to the VMM for every notification but keeps the one-CPU example straightforward. The format and ordering come from [virtio's split-ring, MMIO, and console specifications](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).

## 1. How this machine boots Linux

| Step | Host action | What Linux sees |
| --- | --- | --- |
| 1 | Read and check the Image header | An uncompressed ARM64 kernel at its required entry |
| 2 | Load initramfs and DTB into separate regions | A root archive and a machine description |
| 3 | Create the VM, memory mapping, and GIC | RAM and an interrupt controller |
| 4 | Patch the DTB and initialize the vCPU | The ARM64 boot contract |
| 5 | Call `hv_vcpu_run` | Linux starts with the MMU off |
| 6 | Answer the early UART's MMIO accesses | The first kernel log reaches stdout |
| 7 | Answer virtio-MMIO setup and notification accesses | `hvc0` becomes the console |
| 8 | Drain the transmit queue | `/init` writes its greeting |
| 9 | Handle PSCI SYSTEM_RESET | vmmlite destroys the VM and returns zero |

There is no ARM equivalent of the x86 bzImage decompressor in this loader. The input must already be an uncompressed `Image`. Its 64-byte header supplies `text_offset`, `image_size`, flags, and the ARM64 magic. The loader rejects a bad magic, zero image size, big-endian image, or placement that overlaps the DTB workspace.

### 1.1 Where everything lands

High addresses are at the top. Sizes and addresses are the contract shared by the C source and DTS:

```text
0x60000000  +-------------------------------------------+ end of 512 MiB RAM
            | free RAM                                  |
0x50000000  + initramfs: /init, length read from file     +
            | free RAM                                  |
0x48200000  +-------------------------------------------+ DTB workspace limit
            | space available to libfdt while patching  |
0x48000000  + packed device tree                         +
            | free RAM; kernel must end below the DTB   |
            | Linux Image, including its reserved size |
0x40080000  + kernel entry for the supplied test Image  +
0x40000000  +-------------------------------------------+ RAM base
            | unmapped gap                              |
0x10000000  + GIC redistributor, size queried from HVF   +
0x0a000000  + virtio console registers, 4 KiB            +
0x09000000  + early UART registers, 8 bytes             +
0x08000000  + GIC distributor, size queried from HVF     +
```

The Image's offset is read from its header, not hardcoded to `0x80000`. The DTB workspace is a loader limit; after packing, Linux consumes the actual DTB size. The initramfs end property is exclusive. The Image and initramfs lie in the same 1 GiB-aligned physical window.

## 2. Reading the code

The full implementation is [vmmlite.c](vmmlite.c); its numbered sections correspond to the concepts above.

- **Input and guest-address helpers** bound files and translate checked guest addresses into host pointers.
- **Section 1, Image placement** follows the ARM64 header and prevents overlap with the tree.
- **Section 2, virtio console** owns queue configuration, feature negotiation, output, completions, and the interrupt line.
- **Section 3, DTB preparation** uses libfdt to update properties that depend on this run.
- **Section 4, MMIO** decodes a trapped instruction, dispatches the device access, returns load data, and advances PC.
- **Sections 5–6, machine creation** establish RAM, GIC, vCPU affinity, and the Linux entry state.
- **Section 7, execution** dispatches MMIO, a small set of debug-register traps, and PSCI calls.

Linux initializes its OS debug locks during boot. We model those locks as absent: OSLAR and OSDLR writes have no effect, and OSLSR reads zero. Other trapped system registers fail. This intentionally small model does not provide a guest debugger.

PSCI reports version 0.2. SYSTEM_OFF and SYSTEM_RESET finish the run; AFFINITY_INFO reports the sole CPU as on. Other calls return NOT_SUPPORTED. There is no CPU hotplug or suspend implementation.

The shared [init.c](../init.c) writes to file descriptor 1, drains the console, and calls Linux's reboot interface. On x86 Linux eventually uses the keyboard-controller reset path; on this ARM machine it issues a PSCI HVC. The guest program itself does not call Hypervisor.framework.

## 3. Verified output and tests

A successful run includes this sequence; timestamps vary:

```text
[    0.000000] earlycon: uart8250 at MMIO 0x0000000009000000 (options '')
[    0.000000] psci: PSCIv0.2 detected in firmware.
[    0.000000] GICv3: CPU0: found redistributor 0 region 0:0x0000000010000000
[    0.000000] arch_timer: cp15 timer(s) running at 24.00MHz (virt).
[    0.026819] printk: console [hvc0] enabled
[    0.026955] printk: bootconsole [uart8250] disabled
[    0.028796] Run /init as init process
Hello from PID 1!
vmmlite: guest requested PSCI shutdown/reset
```

The first lines travel one byte at a time through the early UART. After `bootconsole [uart8250] disabled`, output comes through virtqueue 1. The PID 1 greeting is therefore evidence of a working virtio console, not just a working early printer.

`make test` checks:

| Check | What it establishes |
| --- | --- |
| Native production-function tests | Image placement, guest range checks, queue negotiation, descriptor validation, used entries, counter wrap, interrupt acknowledgment, and reset |
| Three complete Linux boots | The signed executable reaches `hvc0`, PID 1, and successful PSCI termination repeatedly |
| Timer guest | A sleeping task wakes, then sends 300 ordered console lines and resets |
| Malformed executable inputs | Invalid Image headers, empty/oversized files, and an invalid DTB fail |
| Code-signature verification | The built executable's signature remains valid |

The native tests stub the host SPI call so they can inspect the device's completion behavior. The boot tests use the real framework and real guest kernel. Every VM run has a host-side timeout, and a kernel panic fails the test even if it requests a reset.

Logs are saved in `macos/.build/boot.log`, `boot-0.log` through `boot-2.log`, and `timer.log`. The timer guest is a test fixture; the regular example still boots the shared root `init.c`.

These results establish the tested boot path. They do not establish adversarial isolation, macOS-version coverage, performance, multi-CPU correctness, or full virtio conformance.

## 4. Why it looks like this

| Choice | Reason and cost |
| --- | --- |
| Hypervisor.framework | Exposes CPU, memory, and exit mechanics; the higher-level Virtualization framework would hide much of the lesson |
| Framework GICv3 | Keeps interrupt-controller emulation out of this tiny program; requires macOS 15+ APIs |
| One vCPU | Makes ownership of device state explicit; does not demonstrate SMP |
| Fixed RAM layout | Keeps addresses traceable across C, DTS, and the boot log; limits supported image sizes |
| Readable DTS plus libfdt | Lets readers inspect the machine and avoids a hand-written FDT parser; adds a small build dependency |
| Early UART plus virtio console | Shows both register-by-register output and shared-memory device I/O |
| Synchronous queue processing | Removes the worker/eventfd machinery; every notification returns to the VMM |
| A static PID 1 | Boots without a disk or dynamic linker; demonstrates a kernel and a user process with a tiny initramfs |
| Exact public kernel plus checksum | Makes the example repeatable; supported driver configuration matters more than a generic kernel filename |

The current x86 and macOS examples expose the same learning endpoint, but model different machines. ARM device trees, GIC interrupt IDs, EL1 entry state, and PSCI calls replace x86's zero page, MP table, APIC/PIT setup, long-mode preparation, and i8042 reset.

A production VMM also needs robust device conformance, resource accounting, lifecycle control, process confinement, fuzzing, and a sustained security review. Adding those would change the purpose and size of this example.

## Sources

- [Linux ARM64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html): Image layout, DTB placement, and entry conditions.
- [Apple Hypervisor.framework for Apple Silicon](https://developer.apple.com/documentation/hypervisor/apple-silicon): VM, CPU, memory, timer, and GIC APIs.
- [Apple GIC creation](https://developer.apple.com/documentation/hypervisor/hv_gic_create(_:)) and [SPI injection](https://developer.apple.com/documentation/hypervisor/hv_gic_set_spi(_:_:)): creation order and level semantics.
- [Linux GICv3 device-tree binding](https://www.kernel.org/doc/Documentation/devicetree/bindings/interrupt-controller/arm,gic-v3.yaml): address regions and interrupt cells.
- [Virtio 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html), sections 2.7, 4.2, and 5.3: split queues, MMIO transport, and console.
- [QEMU HVF](https://github.com/qemu/qemu/blob/48396ad6ce9ac4ec5ec97d45435c947b582c7f39/target/arm/hvf/hvf.c): instruction completion and debug-register handling.
- [Firecracker kernel setup](https://github.com/firecracker-microvm/firecracker/blob/main/docs/rootfs-and-kernel-setup.md) and [the exact fixture configuration](https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.11/aarch64/vmlinux-6.1.102.config): ARM64 Image format and built-in drivers.
- [musl cross-compiler installation](https://github.com/FiloSottile/homebrew-musl-cross): static ARM64 Linux compilation on macOS.
