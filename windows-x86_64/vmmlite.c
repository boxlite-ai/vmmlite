// vmmlite.c: a small VMM that boots Linux (x86_64, Windows WHP). For learning only.
// Build: make build    Run: .\vmmlite.exe bzImage initramfs.cpio
// Native Windows boot is still unverified; README.md records the validation boundary.
//
//   vmmlite (this process)             WHP / Microsoft hypervisor       CPU (VT-x / AMD-V)
//   +--------------------------+ API +---------------------------+ entry +------------------+
//   | RAM, PIC, IOAPIC, PIT     |---->| GPA mappings, LAPIC, VP 0 |------>| runs guest code  |
//   | COM1, virtio console     |<----| exit context              |<------| until a VM exit  |
//   +--------------------------+ exits +-------------------------+ exit  +------------------+
//
// Read main() as the same eight boot steps as the Linux/KVM example. The device helpers
// come first; WHP supplies the LAPIC, while this process supplies the other PC devices.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>                                     // fixed-width guest fields
#include <inttypes.h>                                   // portable register/address printing
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef VMMLITE_TEST                                    // portable tests include this same file
#define WIN32_LEAN_AND_MEAN
#include <windows.h>                                    // allocation, timer and thread lifetime
#include <winhvplatform.h>                              // partition, VP, mappings and interrupts
#include <winhvemulation.h>                             // complete the guest's IO/MMIO instruction
#include <fcntl.h>
#include <io.h>                                         // binary stdout on Windows
#endif

#define RAM_SIZE             (512u << 20)               // 512 MiB at guest physical address 0
#define PAGE_SIZE            0x1000u                    // 4 KiB
// Where each thing goes, as guest physical addresses:
#define GDT_START            0x500u                     // the boot GDT
#define ZERO_PAGE_START      0x7000u                    // Linux's boot_params, the zero page
#define PML4_START           0x9000u                    // one page for each paging level:
#define PDPT_START           (PML4_START + PAGE_SIZE)    //   PML4, PDPT, then PD
#define PD_START             (PDPT_START + PAGE_SIZE)
#define CMDLINE_START        0x20000u                   // the kernel command line
#define MPTABLE_START        0x9fc00u                   // the MP table
#define KERNEL_START         0x100000u                  // 1 MiB: decompressor + compressed kernel
#define KERNEL_ENTRY         (KERNEL_START + 0x200u)     // the decompressor's 64-bit entry
#define VIRTIO_CONSOLE_START 0xd0000000u                // the console's MMIO registers
#define VIRTIO_CONSOLE_SIZE  PAGE_SIZE
#define IO_APIC_DEFAULT_PHYS_BASE 0xfec00000u           // the IOAPIC's MMIO registers
#define APIC_DEFAULT_PHYS_BASE 0xfee00000u              // WHP's local APIC
#define COM1_PORT            0x3f8u                     // the 16550 UART's eight I/O ports
#define COM1_SIZE            8u
#define COM1_IRQ             4u
#define VIRTIO_CONSOLE_IRQ   5u                         // here ISA IRQ n = IOAPIC pin n
#define I8042_COMMAND_REG    0x64u                      // keyboard-controller command port
#define KBD_CCMD_RESET       0xfeu                      // pulse CPU reset: finish the run
#define QUEUE_SIZE_MAX       256u
#define TRANSMIT_QUEUE       1u                         // queue 0 is input; queue 1 is output

struct memory { uint8_t *bytes; size_t size; };
struct image { const uint8_t *bytes; size_t size; };

// All guest structures are little-endian; never cast unaligned guest pointers.
static inline uint16_t le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t le32(const uint8_t *p) {
  return le16(p) | (uint32_t)le16(p + 2) << 16;
}
static inline uint64_t le64(const uint8_t *p) {
  return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static inline void store16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void store32(uint8_t *p, uint32_t v) {
  store16(p, (uint16_t)v); store16(p + 2, (uint16_t)(v >> 16));
}
static inline void store64(uint8_t *p, uint64_t v) {
  store32(p, (uint32_t)v); store32(p + 4, (uint32_t)(v >> 32));
}
static inline bool in_ram(struct memory ram, uint64_t address, uint64_t size) {
  return address <= ram.size && size <= ram.size - address;
}

typedef bool (*output_fn)(void *context, const uint8_t *bytes, size_t size);
typedef bool (*interrupt_fn)(void *context, unsigned vector, unsigned destination,
                             unsigned delivery, bool logical, bool level);

// The PC devices KVM supplies in-kernel, but WHP leaves to vmmlite.
// All their state belongs to the VP thread; timer_thread() only asks that thread to wake.
//
//   device line       PIC path, before Linux masks it       IOAPIC path, after setup
//   PIT: IRQ 0   ---> pending -> mask/priority -> ExtINT     pin 0 --+
//   COM1: IRQ 4  ---> pending -> mask/priority -> ExtINT     pin 4 --+-> vector -> WHP LAPIC
//   console: IRQ 5 -> pending -> mask/priority -> ExtINT     pin 5 --+
//
// pc_irq() records the line in both controllers. Linux programs their masks and routes;
// pc_pic_pending() chooses the PIC vector, and ioapic_deliver() uses the IOAPIC's route.
#define PIT_HZ 1193182u
#define IOAPIC_MASK (1u << 16)
#define IOAPIC_LEVEL (1u << 15)
#define IOAPIC_REMOTE (1u << 14)

struct pic {
  uint8_t mask, pending, service;                       // IMR, IRR, ISR: blocked, waiting, in service
  uint8_t base, step, icw1, icw4;                        // vector base and initialization sequence
  bool read_service;
};
struct pit_channel {
  uint32_t reload;
  uint64_t started, periods;
  uint16_t write_latch, read_latch, count_latch;
  uint8_t mode, access, write_phase, read_phase, latch_bytes, status;
  bool active, gate, status_pending;
};
struct pc {
  struct pic pic[2];
  struct pit_channel pit[3];
  uint64_t ticks;                                       // host time, in 1,193,182 Hz PIT ticks
  uint64_t routes[24];                                  // IOAPIC redirection entries, one per pin
  uint32_t lines;
  uint8_t elcr[2], select, apic_id, speaker, cmos_index;
  uint8_t uart[8];
  uint16_t divisor;
  bool tx_pending, reset;
  output_fn output;
  interrupt_fn interrupt;
  void *context;
  const char *error;
};

static int first_bit(uint8_t bits) {
  for (int bit = 0; bit < 8; bit++) if (bits & (1u << bit)) return bit;
  return -1;
}

// Fixed priority: IRQ 0 wins. A request may preempt only a lower-priority in-service IRQ.
static int pic_next(const struct pic *pic) {
  uint8_t eligible = pic->pending & (uint8_t)~pic->mask;
  int in_service = first_bit(pic->service);
  if (in_service >= 0) eligible &= (uint8_t)((1u << in_service) - 1);
  return first_bit(eligible);
}

static void pic_ack(struct pic *pic, unsigned irq) {
  pic->pending &= (uint8_t)~(1u << irq);
  if (!(pic->icw4 & 2)) pic->service |= (uint8_t)(1u << irq);
}

// The slave PIC reaches the master through IRQ 2. Return its final vector, or -1.
static int pc_pic_pending(struct pc *pc) {
  for (unsigned i = 0; i < 2; i++)
    pc->pic[i].pending |= (uint8_t)(pc->lines >> (i * 8)) & pc->elcr[i];
  if (pic_next(&pc->pic[1]) >= 0) pc->pic[0].pending |= 4;
  else pc->pic[0].pending &= (uint8_t)~4u;
  int irq = pic_next(&pc->pic[0]);
  if (irq == 2) return pc->pic[1].base + pic_next(&pc->pic[1]);
  return irq < 0 ? -1 : pc->pic[0].base + irq;
}

static void pc_pic_ack(struct pc *pc) {
  int irq = pic_next(&pc->pic[0]);
  if (irq < 0) return;
  if (irq == 2) {
    int slave = pic_next(&pc->pic[1]);
    if (slave >= 0) pic_ack(&pc->pic[1], (unsigned)slave);
  }
  pic_ack(&pc->pic[0], (unsigned)irq);
}

// PIC register interface, at 0x20/0x21 (master) and 0xa0/0xa1 (slave):
//   command read   IRR, or ISR after OCW3 selects it
//   data read      IMR: a 1 bit masks that IRQ
//   ICW1..4 writes reset, vector base, cascade wiring, then mode
//   EOI write      clear an in-service bit so the next request may be delivered
static uint8_t pic_port(struct pic *pic, bool data, bool write, uint8_t value) {
  if (!write) return data ? pic->mask : (pic->read_service ? pic->service : pic->pending);
  if (data) {
    if (pic->step == 1) {
      pic->base = value & 0xf8;
      pic->step = (pic->icw1 & 2) ? ((pic->icw1 & 1) ? 3 : 0) : 2;
    } else if (pic->step == 2) pic->step = (pic->icw1 & 1) ? 3 : 0;
    else if (pic->step == 3) { pic->icw4 = value; pic->step = 0; }
    else pic->mask = value;
  } else if (value & 0x10) {
    *pic = (struct pic){.step = 1, .icw1 = value};
  } else if (value & 8) {
    if (value & 2) pic->read_service = (value & 1) != 0;
  } else if (value & 0x20) {
    int irq = (value & 0x40) ? value & 7 : first_bit(pic->service);
    if (irq >= 0) pic->service &= (uint8_t)~(1u << irq);
  }
  return 0;
}

// An IOAPIC entry: bits 0-7 vector, 8-10 delivery, 11 logical destination,
// 14 remote IRR, 15 level trigger, 16 mask, and 56-63 destination APIC ID.
// A level request waits for EOI before it can be sent again; an edge needs a new rising line.
static void ioapic_deliver(struct pc *pc, unsigned pin) {
  uint64_t entry = pc->routes[pin];
  if ((entry & IOAPIC_MASK) || (entry & IOAPIC_REMOTE)) return;
  unsigned delivery = (unsigned)(entry >> 8) & 7;
  // Fixed, lowest-priority and NMI are the routes this single-CPU machine supports.
  if (delivery != 0 && delivery != 1 && delivery != 4) {
    pc->error = "unsupported IOAPIC delivery mode";
    return;
  }
  if (!pc->interrupt(pc->context, (unsigned)entry & 255, (unsigned)(entry >> 56),
                     delivery, (entry & (1u << 11)) != 0, (entry & IOAPIC_LEVEL) != 0)) {
    pc->error = "interrupt injection failed";
    return;
  }
  if (entry & IOAPIC_LEVEL) pc->routes[pin] |= IOAPIC_REMOTE;
}

static void pc_irq(struct pc *pc, unsigned irq, bool high) {
  if (irq >= 24) return;
  uint32_t bit = 1u << irq;
  bool rising = high && !(pc->lines & bit);
  if (high) pc->lines |= bit; else pc->lines &= ~bit;
  if (irq < 16) {
    struct pic *pic = &pc->pic[irq / 8];
    uint8_t mask = (uint8_t)(1u << (irq % 8));
    if (rising) pic->pending |= mask;
    if (!high && (pc->elcr[irq / 8] & mask)) pic->pending &= (uint8_t)~mask;
  }
  if (high && ((pc->routes[irq] & IOAPIC_LEVEL) || rising)) ioapic_deliver(pc, irq);
}

// WHP reports the guest's LAPIC EOI. Clear remote IRR, then retry a still-high level line.
static void pc_eoi(struct pc *pc, unsigned vector) {
  for (unsigned pin = 0; pin < 24; pin++) {
    if ((pc->routes[pin] & IOAPIC_REMOTE) && (pc->routes[pin] & 255) == vector) {
      pc->routes[pin] &= ~(uint64_t)IOAPIC_REMOTE;
      if (pc->lines & (1u << pin)) ioapic_deliver(pc, pin);
    }
  }
}

// IOAPIC: the guest writes a register number to offset 0, then uses the window at 0x10.
//   register 0      APIC ID             register 1    version, highest pin (23)
//   register 2      arbitration ID      0x10 + 2*pin  low/high halves of that pin's route
static bool pc_mmio(struct pc *pc, unsigned offset, bool write, uint32_t *value) {
  if (offset == 0) {
    if (write) pc->select = (uint8_t)*value; else *value = pc->select;
    return true;
  }
  if (offset != 0x10) return false;
  unsigned reg = pc->select;
  if (reg >= 0x10 && reg < 0x40) {
    uint64_t *entry = &pc->routes[(reg - 0x10) / 2];
    unsigned shift = (reg & 1) * 32;
    if (!write) *value = (uint32_t)(*entry >> shift);
    else {
      uint32_t part = (reg & 1) ? (*value & 0xff000000u) :
          ((*value & 0x1afffu) | ((uint32_t)*entry & IOAPIC_REMOTE));
      *entry = (*entry & ~(UINT64_C(0xffffffff) << shift)) | (uint64_t)part << shift;
      unsigned pin = (reg - 0x10) / 2;
      if ((*entry & IOAPIC_LEVEL) && (pc->lines & (1u << pin))) ioapic_deliver(pc, pin);
    }
  } else if (write) {
    if (reg == 0) pc->apic_id = (uint8_t)(*value >> 24) & 15;
  } else {
    *value = reg == 1 ? (23u << 16) | 0x11 :
             (reg == 0 || reg == 2) ? (uint32_t)pc->apic_id << 24 : 0;
  }
  return pc->error == NULL;
}

// PIT: ports 0x40..0x42 are the channels, 0x43 is control; 0x61 gates channel 2.
// Channel 0 raises IRQ 0. Linux also polls channel 2's output to calibrate time early in boot.
// Counts derive from elapsed host time, not the number of reads the guest makes.
//   mode 0   one countdown       mode 2   rate generator       mode 3   square wave
// A latch keeps both bytes of a count together even if host time advances between reads.
static uint64_t elapsed(const struct pc *pc, const struct pit_channel *channel) {
  return channel->active && channel->gate ? pc->ticks - channel->started : 0;
}

static uint16_t pit_count(const struct pc *pc, const struct pit_channel *channel) {
  uint64_t ticks = elapsed(pc, channel);
  uint32_t count = channel->reload;
  if (channel->mode == 0) return (uint16_t)(count - ticks);
  unsigned phase = (unsigned)(ticks % count);
  if (channel->mode == 2) return (uint16_t)(count - phase);
  unsigned high = (count + 1) / 2;
  if (!(count & 1)) return (uint16_t)(count - (phase * 2) % count);
  return (uint16_t)(phase < high ? (phase ? count + 1 - phase * 2 : count) :
                                     count - 1 - 2 * (phase - high));
}

static bool pit_output(const struct pc *pc, const struct pit_channel *channel) {
  if (!channel->active) return true;
  uint64_t ticks = elapsed(pc, channel);
  if (channel->mode == 0) return ticks >= channel->reload;
  if (!channel->gate) return true;
  unsigned phase = (unsigned)(ticks % channel->reload);
  return channel->mode == 2 ? phase != channel->reload - 1 : phase < (channel->reload + 1) / 2;
}

static void pit_latch(struct pc *pc, struct pit_channel *channel) {
  if (channel->latch_bytes) return;
  channel->count_latch = pit_count(pc, channel);
  channel->latch_bytes = channel->access == 3 ? 2 : 1;
}

static void pit_control(struct pc *pc, uint8_t value) {
  unsigned select = value >> 6;
  if (select == 3) {
    for (unsigned i = 0; i < 3; i++) if (value & (2u << i)) {
      struct pit_channel *channel = &pc->pit[i];
      if (!(value & 0x20)) pit_latch(pc, channel);
      if (!(value & 0x10) && !channel->status_pending) {
        channel->status = (uint8_t)((pit_output(pc, channel) ? 0x80 : 0) |
            (!channel->active ? 0x40 : 0) | channel->access << 4 | channel->mode << 1);
        channel->status_pending = true;
      }
    }
    return;
  }
  struct pit_channel *channel = &pc->pit[select];
  if (!(value & 0x30)) { pit_latch(pc, channel); return; }
  unsigned mode = (value >> 1) & 7;
  if (mode >= 6) mode -= 4;
  if ((value & 1) || (mode != 0 && mode != 2 && mode != 3)) {
    pc->error = "PIT supports binary modes 0, 2 and 3 only";
    return;
  }
  bool gate = channel->gate;
  *channel = (struct pit_channel){.reload = 65536, .mode = (uint8_t)mode,
      .access = (value >> 4) & 3, .gate = gate};
}

static uint8_t pit_port(struct pc *pc, unsigned index, bool write, uint8_t value) {
  struct pit_channel *channel = &pc->pit[index];
  if (write) {
    if (channel->access == 3 && !channel->write_phase) {
      channel->write_latch = value; channel->write_phase = 1; return 0;
    }
    uint16_t count = channel->access == 1 ? value : channel->access == 2 ?
        (uint16_t)value << 8 : channel->write_latch | (uint16_t)value << 8;
    channel->reload = count ? count : 65536;
    channel->write_phase = 0; channel->active = true;
    channel->started = pc->ticks; channel->periods = 0;
    return 0;
  }
  if (channel->status_pending) {
    channel->status_pending = false; return channel->status;
  }
  if (channel->latch_bytes) {
    unsigned shift = channel->access == 2 ||
        (channel->access == 3 && channel->latch_bytes == 1) ? 8 : 0;
    channel->latch_bytes--;
    return (uint8_t)(channel->count_latch >> shift);
  }
  if (channel->access != 3) return (uint8_t)(pit_count(pc, channel) >> (channel->access == 2 ? 8 : 0));
  if (!channel->read_phase) channel->read_latch = pit_count(pc, channel);
  uint8_t result = (uint8_t)(channel->read_latch >> (channel->read_phase ? 8 : 0));
  channel->read_phase ^= 1;
  return result;
}

static void pc_tick(struct pc *pc, uint64_t now_ns) {
  pc->ticks = now_ns / 1000000000u * PIT_HZ + now_ns % 1000000000u * PIT_HZ / 1000000000u;
  struct pit_channel *timer = &pc->pit[0];
  if (!timer->active || !timer->gate) return;
  uint64_t periods = elapsed(pc, timer) / timer->reload;
  if (timer->mode == 0 && periods > 1) periods = 1;
  if (periods > timer->periods) {
    timer->periods = periods;
    // Coalesce missed host deadlines; never spin injecting a backlog of ticks.
    pc_irq(pc, 0, true); pc_irq(pc, 0, false);
  }
}

// COM1: the 16550 UART at ports 0x3f8-0x3ff, enough for the guest's 8250 driver to send.
//
//   reg   guest reads (IN)                   guest writes (OUT)
//   0     0, no input                       output one byte; TX becomes empty again
//   1     IER                               IER: enabling bit 1 raises "TX empty"
//   2     IIR: 2 if TX empty, else 1         FCR: stored, no FIFO is advertised
//   3     LCR                               LCR; bit 7 (DLAB) selects the baud divisor
//   5     LSR: 0x60, transmitter empty      -
//   6     MSR: 0xb0, fixed modem signals   -
//   7     scratch byte                      scratch byte
//
// With DLAB set, registers 0 and 1 hold the divisor, not text. IRQ 4 stays high while
// TX-empty is enabled and pending; reading IIR acknowledges that event.
static uint8_t com1(struct pc *pc, unsigned reg, bool write, uint8_t value) {
  if ((pc->uart[3] & 0x80) && reg < 2) {
    if (write) pc->divisor = (uint16_t)((pc->divisor & ~(0xffu << (reg * 8))) | (unsigned)value << (reg * 8));
    return (uint8_t)(pc->divisor >> (reg * 8));
  }
  uint8_t result = 0;
  if (write) {
    if (reg == 0) {
      if (!pc->output(pc->context, &value, 1)) pc->error = "serial output failed";
      pc->tx_pending = true;
    } else if (reg == 1) {
      if ((value & 2) && !(pc->uart[1] & 2)) pc->tx_pending = true;
      pc->uart[1] = value & 15;
    } else pc->uart[reg] = value;
  } else {
    result = pc->uart[reg];
    if (reg == 0) result = 0;
    if (reg == 2) {
      result = (pc->tx_pending && (pc->uart[1] & 2)) ? 2 : 1;
      if (result == 2) pc->tx_pending = false;
    }
    if (reg == 5) result = 0x60;
    if (reg == 6) result = 0xb0;
  }
  pc_irq(pc, COM1_IRQ, pc->tx_pending && (pc->uart[1] & 2));
  return result;
}

// The port bus, called by the WHP instruction emulator in section 8.
// Other small PC probes are answered here too: CMOS date, A20, and the i8042 reset command.
static uint32_t pc_port(struct pc *pc, uint16_t port, unsigned size, bool write, uint32_t value) {
  uint8_t byte = (uint8_t)value;
  if (port >= COM1_PORT && port < COM1_PORT + COM1_SIZE && size == 1) return com1(pc, port - COM1_PORT, write, byte);
  if ((port == 0x20 || port == 0x21 || port == 0xa0 || port == 0xa1) && size == 1)
    return pic_port(&pc->pic[port >= 0xa0], (port & 1) != 0, write, byte);
  if (port >= 0x40 && port <= 0x42 && size == 1) return pit_port(pc, port - 0x40, write, byte);
  if (port == 0x43 && write) { pit_control(pc, byte); return 0; }
  if (port == 0x61) {
    if (write) {
      bool gate = (byte & 1) != 0;
      if (gate && !pc->pit[2].gate) pc->pit[2].started = pc->ticks;
      pc->pit[2].gate = gate; pc->speaker = byte & 3;
    }
    return pc->speaker | (pit_output(pc, &pc->pit[2]) ? 0x20 : 0) | ((pc->ticks >> 4) & 1) << 4;
  }
  if (port == 0x4d0 || port == 0x4d1) {
    unsigned index = port & 1;
    if (write) pc->elcr[index] = byte & (index ? 0xde : 0xf8);
    return pc->elcr[index];
  }
  if (port == I8042_COMMAND_REG || port == 0x60) {
    if (write && port == I8042_COMMAND_REG && byte == KBD_CCMD_RESET) pc->reset = true;
    return 0;
  }
  if (port == 0x70) { if (write) pc->cmos_index = byte & 127; return pc->cmos_index; }
  if (port == 0x71) {
    // A fixed, valid RTC date; this sample provides no RTC interrupt or wall clock.
    switch (pc->cmos_index) {
    case 0x06: case 0x07: case 0x08: return 1;
    case 0x09: return 0x26;
    case 0x0a: return 0x26;
    case 0x0b: return 2;
    case 0x0d: return 0x80;
    case 0x32: return 0x20;
    default: return 0;
    }
  }
  if (port == 0x92) return 2; // A20 already enabled.
  // Absent ISA/PCI devices, including PCI configuration reads, return all ones.
  return size == 1 ? 0xff : size == 2 ? 0xffff : UINT32_MAX;
}

static struct pc *pc_create(output_fn output, interrupt_fn interrupt, void *context) {
  struct pc *pc = calloc(1, sizeof *pc);
  if (!pc) return NULL;
  pc->output = output; pc->interrupt = interrupt; pc->context = context;
  pc->apic_id = 1;
  pc->pic[0] = (struct pic){.mask = 255, .base = 8};
  pc->pic[1] = (struct pic){.mask = 255, .base = 0x70};
  for (unsigned i = 0; i < 24; i++) pc->routes[i] = IOAPIC_MASK;
  for (unsigned i = 0; i < 3; i++)
    pc->pit[i] = (struct pit_channel){.reload = 65536, .mode = 3, .access = 3, .gate = i != 2};
  return pc;
}
static void pc_destroy(struct pc *pc) { free(pc); }
static bool pc_reset_requested(const struct pc *pc) { return pc->reset; }
static const char *pc_error(const struct pc *pc) { return pc->error; }

// The virtio console: Linux's hvc0, at 0xd0000000 on IRQ 5. Section 4 names both.
// Queue 0 receives (there is no input source); queue 1 transmits buffers already in guest RAM.
//
//   function                    called from        what it does
//   console_create()            section 2, once     initializes the console's state
//   console_register_read()     section 8, via WHP  answers a register read
//   console_register_write()    section 8, via WHP  applies a write; a queue-1 kick calls console_transmit()
//   console_transmit()          the write above    prints buffers, returns them, raises IRQ 5
//
//   guest                             WHP                     vmmlite
//   register read/write   ----------> MMIO exit ------------> console_register_*()
//   kick queue 1          ----------> MMIO exit ------------> console_transmit(): RAM -> stdout
//   IRQ 5                 <---------- LAPIC <---------------- IOAPIC pin 5
//
// Unlike the KVM example's console worker, console_transmit() runs while the only VP is stopped.
// The doorbell carries a queue number; the text itself never crosses the MMIO register.
// IRQ 5 must agree in section 4's command line, section 5's MP route, and console_transmit().
enum {
  VIRTIO_MMIO_MAGIC_VALUE = 0x000, VIRTIO_MMIO_VERSION = 0x004,
  VIRTIO_MMIO_DEVICE_ID = 0x008, VIRTIO_MMIO_VENDOR_ID = 0x00c,
  VIRTIO_MMIO_DEVICE_FEATURES = 0x010, VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
  VIRTIO_MMIO_DRIVER_FEATURES = 0x020, VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
  VIRTIO_MMIO_QUEUE_SEL = 0x030, VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034, VIRTIO_MMIO_QUEUE_NUM = 0x038,
  VIRTIO_MMIO_QUEUE_READY = 0x044, VIRTIO_MMIO_QUEUE_NOTIFY = 0x050, VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
  VIRTIO_MMIO_INTERRUPT_ACK = 0x064, VIRTIO_MMIO_STATUS = 0x070,
  VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080, VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084,
  VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090, VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
  VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0, VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0a4
};
enum { DRIVER_OK = 4, FEATURES_OK = 8, FAILED = 128 };
#define CONSOLE_FEATURES (UINT64_C(1) << 32) // VIRTIO_F_VERSION_1; no indirect/event-index rings.

struct queue {
  uint64_t desc_table, avail_ring, used_ring;             // GPAs of descriptor table, avail, used
  unsigned size;
  uint16_t next_avail, next_used;                        // ring counters wrap at 16 bits
  bool ready;
};
struct console {
  struct memory ram;
  struct pc *pc;
  output_fn output;
  void *context;
  struct queue queues[2];
  uint64_t driver_features, transmitted;
  uint32_t device_features_select, driver_features_select, queue_select;
  uint32_t status, interrupt_status;
  const char *error;
};

static bool fail(struct console *console, const char *error) {
  console->error = error;
  return false;
}

static bool queue_valid(struct console *console, const struct queue *queue) {
  return queue->size && queue->size <= QUEUE_SIZE_MAX && !(queue->size & (queue->size - 1)) &&
      !(queue->desc_table & 15) && !(queue->avail_ring & 1) && !(queue->used_ring & 3) &&
      in_ram(console->ram, queue->desc_table, (uint64_t)queue->size * 16) &&
      in_ram(console->ram, queue->avail_ring, 6 + (uint64_t)queue->size * 2) &&
      in_ram(console->ram, queue->used_ring, 6 + (uint64_t)queue->size * 8);
}

// Validate the whole chain before output: no partial line or used entry for a malformed chain.
// A descriptor is {address:64, length:32, flags:16, next:16}; only NEXT is offered for TX.
static bool chain_valid(struct console *console, const struct queue *queue, unsigned head) {
  unsigned descriptor = head;
  for (unsigned walked = 0; walked < queue->size; walked++) {
    if (descriptor >= queue->size) return fail(console, "descriptor index exceeds queue size");
    const uint8_t *entry = console->ram.bytes + queue->desc_table + descriptor * 16;
    if (le16(entry + 12) & ~1u) return fail(console, "TX descriptor is writable, indirect or reserved");
    if (!in_ram(console->ram, le64(entry), le32(entry + 8)))
      return fail(console, "TX buffer exceeds guest RAM");
    if (!(le16(entry + 12) & 1)) return true;
    descriptor = le16(entry + 14);
  }
  return fail(console, "cyclic TX descriptor chain");
}

// console_transmit(): take each available head, print its chain, then give that head back in used.
//   avail: {flags, idx, ring[head...]}   used: {flags, idx, ring[{id, len}...]}
// The driver writes avail; this device writes used. len is 0: the device only READ the buffers.
// After publishing completions, raise IRQ 5 unless the driver asked to suppress interrupts.
static bool console_transmit(struct console *console) {
  struct queue *queue = &console->queues[TRANSMIT_QUEUE];
  if (!queue->ready || !(console->status & DRIVER_OK) || (console->status & FAILED))
    return fail(console, "TX notification before the device and queue are ready");
  uint8_t *ram = console->ram.bytes;
  uint8_t *available = ram + queue->avail_ring;
  uint8_t *used = ram + queue->used_ring;
  uint16_t pending = (uint16_t)(le16(available + 2) - queue->next_avail);
  if (pending > queue->size) return fail(console, "available ring advanced beyond queue capacity");
  if (!pending) return true;
  while (pending--) {
    unsigned head = le16(available + 4 + (queue->next_avail % queue->size) * 2);
    if (!chain_valid(console, queue, head)) return false;
    unsigned descriptor = head;
    for (;;) {
      const uint8_t *entry = ram + queue->desc_table + descriptor * 16;
      size_t size = le32(entry + 8);
      if (!console->output(console->context, ram + le64(entry), size))
        return fail(console, "virtio console output failed");
      console->transmitted += size;
      if (!(le16(entry + 12) & 1)) break;
      descriptor = le16(entry + 14);
    }
    uint8_t *completion = used + 4 + (queue->next_used % queue->size) * 8;
    store32(completion, head);
    store32(completion + 4, 0); // Device read the TX buffers; it wrote zero bytes into them.
    queue->next_avail++;
    queue->next_used++;
    // The only VP is stopped throughout this callback. No concurrent ring consumer.
    store16(used + 2, queue->next_used);
  }
  if (!(le16(available) & 1)) { // VIRTQ_AVAIL_F_NO_INTERRUPT.
    console->interrupt_status |= 1;
    pc_irq(console->pc, VIRTIO_CONSOLE_IRQ, true);
  }
  return true;
}

static void console_reset(struct console *console) {
  pc_irq(console->pc, VIRTIO_CONSOLE_IRQ, false);
  *console = (struct console){.ram = console->ram, .pc = console->pc,
      .output = console->output, .context = console->context, .transmitted = console->transmitted};
}

// The console's register interface, one aligned 32-bit access per emulator callback.
// The names below omit the VIRTIO_MMIO_ prefix used in the code:
//   offset   register              guest reads             guest writes
//   0x000    MAGIC_VALUE           "virt"                  -
//   0x004    VERSION               2, modern virtio         -
//   0x008    DEVICE_ID             3, console               -
//   0x010    DEVICE_FEATURES       selected 32 feature bits -
//   0x014    DEVICE_FEATURES_SEL   -                        selects the feature half
//   0x020/24 DRIVER_FEATURES/SEL   -                        accepted features, then selector
//   0x030    QUEUE_SEL             -                        selects queue 0 or 1
//   0x034/38 QUEUE_NUM_MAX/NUM     maximum                  chosen ring size
//   0x044    QUEUE_READY           ready                    validate geometry, then enable
//   0x050    QUEUE_NOTIFY          -                        1 sends; 0 leaves RX buffers alone
//   0x060/64 INTERRUPT_STATUS/ACK  pending bits             acknowledge bits and lower IRQ 5
//   0x070    STATUS                driver progress          0 resets; FEATURES_OK checks agreement
//   0x080/84 QUEUE_DESC_LOW/HIGH   -                        descriptor-table GPA
//   0x090/94 QUEUE_DRIVER_LOW/HIGH -                        available-ring GPA
//   0x0a0/a4 QUEUE_DEVICE_LOW/HIGH -                        used-ring GPA
//
// Linux probes the IDs, resets, accepts VERSION_1, supplies both queues, then sets DRIVER_OK.
// Queue addresses stay fixed while ready. These checks remain even in a single-file example.
static bool console_register_read(struct console *console, unsigned offset, uint32_t *value) {
  if (offset >= VIRTIO_CONSOLE_SIZE || (offset & 3)) return fail(console, "unaligned virtio register access");
  struct queue *queue = console->queue_select < 2 ? &console->queues[console->queue_select] : NULL;
  switch (offset) {
  case VIRTIO_MMIO_MAGIC_VALUE: *value = 0x74726976; break;
  case VIRTIO_MMIO_VERSION: *value = 2; break;
  case VIRTIO_MMIO_DEVICE_ID: *value = 3; break;
  case VIRTIO_MMIO_VENDOR_ID: *value = 0x4c4d5656; break;
  case VIRTIO_MMIO_DEVICE_FEATURES: *value = console->device_features_select == 1 ? 1 : 0; break;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: *value = queue ? QUEUE_SIZE_MAX : 0; break;
  case VIRTIO_MMIO_QUEUE_READY: *value = queue && queue->ready; break;
  case VIRTIO_MMIO_INTERRUPT_STATUS: *value = console->interrupt_status; break;
  case VIRTIO_MMIO_STATUS: *value = console->status; break;
  default: *value = 0; break;
  }
  return true;
}

static bool console_register_write(struct console *console, unsigned offset, uint32_t value) {
  if (offset >= VIRTIO_CONSOLE_SIZE || (offset & 3)) return fail(console, "unaligned virtio register access");
  struct queue *queue = console->queue_select < 2 ? &console->queues[console->queue_select] : NULL;
  switch (offset) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: console->device_features_select = value; break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL: console->driver_features_select = value; break;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    if (console->status & FEATURES_OK) return fail(console, "features changed after negotiation");
    if (console->driver_features_select < 2) {
      unsigned shift = console->driver_features_select * 32;
      console->driver_features = (console->driver_features & ~(UINT64_C(0xffffffff) << shift)) |
          (uint64_t)value << shift;
    }
    break;
  case VIRTIO_MMIO_QUEUE_SEL: console->queue_select = value; break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (!queue) break;
    if (value > 1) return fail(console, "invalid QueueReady value");
    if (value && (!(console->status & FEATURES_OK) || !queue_valid(console, queue)))
      return fail(console, "invalid virtqueue geometry or feature state");
    queue->ready = value != 0;
    break;
  case VIRTIO_MMIO_QUEUE_NUM:
    if (queue && queue->ready) return fail(console, "queue size changed while ready");
    if (queue) queue->size = value;
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW: case VIRTIO_MMIO_QUEUE_DESC_HIGH:
  case VIRTIO_MMIO_QUEUE_DRIVER_LOW: case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
  case VIRTIO_MMIO_QUEUE_DEVICE_LOW: case VIRTIO_MMIO_QUEUE_DEVICE_HIGH: {
    if (!queue) break;
    if (queue->ready) return fail(console, "queue address changed while ready");
    uint64_t *address = offset < VIRTIO_MMIO_QUEUE_DRIVER_LOW ? &queue->desc_table :
        offset < VIRTIO_MMIO_QUEUE_DEVICE_LOW ? &queue->avail_ring : &queue->used_ring;
    unsigned shift = (offset & 4) ? 32 : 0;
    *address = (*address & ~(UINT64_C(0xffffffff) << shift)) | (uint64_t)value << shift;
    break;
  }
  case VIRTIO_MMIO_QUEUE_NOTIFY:
    if (value > 1) return fail(console, "notification names a nonexistent queue");
    if (value == TRANSMIT_QUEUE) return console_transmit(console);
    break; // Receive buffers remain available: this console has no input source.
  case VIRTIO_MMIO_INTERRUPT_ACK:
    console->interrupt_status &= ~(value & 1u);
    pc_irq(console->pc, VIRTIO_CONSOLE_IRQ, console->interrupt_status != 0);
    break;
  case VIRTIO_MMIO_STATUS:
    if (!value) { console_reset(console); break; }
    console->status = value & 255;
    if ((console->status & FEATURES_OK) && console->driver_features != CONSOLE_FEATURES)
      console->status &= ~FEATURES_OK;
    if ((console->status & DRIVER_OK) && !(console->status & FEATURES_OK))
      return fail(console, "DRIVER_OK without accepted features");
    break;
  default: break;
  }
  return true;
}

static struct console *console_create(struct memory ram, struct pc *pc, output_fn output, void *context) {
  struct console *console = calloc(1, sizeof *console);
  if (console) *console = (struct console){.ram = ram, .pc = pc, .output = output, .context = context};
  return console;
}
static void console_destroy(struct console *console) { free(console); }
static const char *console_error(const struct console *console) { return console->error; }
static uint64_t console_bytes(const struct console *console) { return console->transmitted; }

// The byte that makes the low eight bits of the entire MP-table sum equal zero.
static uint8_t checksum(const uint8_t *bytes, size_t size) {
  uint8_t sum = 0;
  while (size--) sum = (uint8_t)(sum + *bytes++);
  return (uint8_t)-sum;
}

// Sections 3 and 4 use the Linux boot protocol's byte offsets; Windows has no asm/bootparam.h.
// Validate both files and reserve the entire decompressor initialization window before copying.
// The layout passes only the two positions section 4 needs, not a second copy of guest data.
//
//   file offset   setup-header field        what load_kernel() checks
//   0x1f1         setup_sects               sectors after the boot sector; 0 means 4
//   0x1fe/0x202   boot_flag/header          0xaa55 / "HdrS": this is a bzImage
//   0x206         version                  at least 2.12, the 64-bit entry protocol
//   0x211         loadflags                bit 0: load the payload at 1 MiB
//   0x22c         initrd_addr_max          last byte the initramfs may occupy
//   0x230/0x234   kernel_alignment/relocatable_kernel
//   0x236         xloadflags               bit 0: a 64-bit kernel
//   0x238         cmdline_size             room for section 4's command line
//   0x258/0x260   pref_address/init_size   aligned runtime address and full initialization size
struct boot_layout { size_t header_end; uint64_t initrd_start; };
static const char command_line[] =
    "earlycon=uart8250,io,0x3f8 console=hvc0 panic=-1 reboot=k "
    "virtio_mmio.device=4K@0xd0000000:5";

static const char *load_kernel(struct memory ram, struct image kernel, struct image initrd,
                               struct boot_layout *layout) {
  const uint8_t *image = kernel.bytes;
  if (!image || kernel.size < 0x264) return "truncated bzImage header";
  if (le16(image + 0x1fe) != 0xaa55 || le32(image + 0x202) != 0x53726448)
    return "need a Linux bzImage (boot flag and HdrS magic)";
  if (le16(image + 0x206) < 0x20c || !(le16(image + 0x236) & 1) || !image[0x234])
    return "need a relocatable x86_64 kernel with boot protocol 2.12+";
  if (!(image[0x211] & 1)) return "need a bzImage loaded high";
  size_t setup_size = ((size_t)(image[0x1f1] ? image[0x1f1] : 4) + 1) * 512;
  size_t header_end = 0x202u + image[0x201];
  if (header_end < 0x264 || header_end > setup_size || setup_size >= kernel.size ||
      kernel.size - setup_size <= 0x200)
    return "invalid setup header or missing 64-bit payload";
  if (!ram.bytes || ram.size < (32u << 20) || ram.size > RAM_SIZE)
    return "guest RAM must be between 32 and 512 MiB";
  uint64_t alignment = le32(image + 0x230);
  uint64_t runtime = le64(image + 0x258);                 // the decompressor's preferred address
  if (!alignment || (alignment & (alignment - 1)) || alignment > ram.size)
    return "invalid kernel alignment";
  if (runtime < KERNEL_START) runtime = KERNEL_START;
  if (runtime > ram.size) return "kernel preferred address exceeds RAM";
  runtime = (runtime + alignment - 1) & ~(alignment - 1);
  uint64_t footprint = le32(image + 0x260);               // init_size, not just compressed bytes
  size_t payload = kernel.size - setup_size;
  if (footprint < payload || !in_ram(ram, runtime, footprint) ||
      !in_ram(ram, KERNEL_START, payload))
    return "kernel initialization window exceeds RAM";
  uint64_t ceiling = (uint64_t)le32(image + 0x22c) + 1;
  if (ceiling > ram.size) ceiling = ram.size;
  if (!initrd.bytes || !initrd.size || initrd.size > ceiling)
    return "initramfs is empty or exceeds its address limit";
  uint64_t initrd_start = (ceiling - initrd.size) & ~UINT64_C(4095);
  if (initrd_start < runtime + footprint || initrd_start < KERNEL_START + payload)
    return "initramfs overlaps the kernel initialization window";
  if (sizeof command_line - 1 > le32(image + 0x238))
    return "kernel command-line limit is too small";

  memcpy(ram.bytes + KERNEL_START, image + setup_size, payload);
  *layout = (struct boot_layout){header_end, initrd_start};
  return NULL;
}

// Section 4: build the zero page from the kernel's header, then replace loader-owned fields.
//   offset  field             value             offset  field          value
//   0x210   type_of_loader    0xff              0x218   ramdisk_image  initramfs GPA
//   0x228   cmd_line_ptr      0x20000           0x21c   ramdisk_size   archive length
//   0x1e8   e820_entries      2                 0x2d0   e820_table     usable RAM ranges
// The MP/BIOS hole is backed by host RAM but omitted from the RAM Linux may allocate.
static void setup_zero_page(struct memory ram, struct image kernel, struct image initrd,
                            struct boot_layout layout) {
  const uint8_t *image = kernel.bytes;
  uint64_t initrd_start = layout.initrd_start;
  size_t header_end = layout.header_end;
  memcpy(ram.bytes + initrd_start, initrd.bytes, initrd.size);
  uint8_t *zero = ram.bytes + ZERO_PAGE_START;
  memcpy(zero + 0x1f1, image + 0x1f1, header_end - 0x1f1);
  zero[0x210] = 0xff; // Unassigned boot-loader ID; zero would mean no initrd.
  zero[0x211] &= (uint8_t)~(1u << 6); // The decompressor may reload segments.
  store32(zero + 0x214, KERNEL_START);                    // code32_start: payload load address
  store32(zero + 0x218, (uint32_t)initrd_start);           // ramdisk_image
  store32(zero + 0x21c, (uint32_t)initrd.size);            // ramdisk_size
  store32(zero + 0x228, CMDLINE_START);                   // cmd_line_ptr
  store32(zero + 0x23c, 0); // Ordinary PC, no paravirtual boot ABI.
  store64(zero + 0x250, 0); // No boot-loader setup_data linked list.
  memcpy(ram.bytes + CMDLINE_START, command_line, sizeof command_line);
  zero[0x1e8] = 2;
  store64(zero + 0x2d0, 0);
  store64(zero + 0x2d8, MPTABLE_START);
  store32(zero + 0x2e0, 1);
  store64(zero + 0x2e4, KERNEL_START);
  store64(zero + 0x2ec, ram.size - KERNEL_START);
  store32(zero + 0x2f4, 1);
}

// Section 5's MP 1.4 table, written bytewise so compiler packing cannot change the wire format.
//   offset  bytes    entry                    what Linux learns
//   0       16       floating pointer _MP_    the configuration header's address
//   16      44       header PCMP              entry count and LAPIC address
//   60      20       CPU                      enabled bootstrap CPU, APIC ID 0
//   80      8        bus                      bus 0 is ISA
//   88      8        IOAPIC                   ID 1, MMIO address 0xfec00000
//   96      16 x 8   IRQ routes               ISA IRQ n -> IOAPIC pin n, including PIT IRQ 0
//   224     2 x 8    local IRQ routes         ExtINT -> LINT0, NMI -> LINT1
// The two checksums cover the floating pointer and configuration table separately.
static void setup_mptable(struct memory ram) {
  uint8_t *mp = ram.bytes + MPTABLE_START;
  // MP 1.4: floating pointer, configuration header, CPU, bus, IOAPIC, routes.
  memcpy(mp, "_MP_", 4);
  store32(mp + 4, MPTABLE_START + 16);
  mp[8] = 1; mp[9] = 4;
  uint8_t *header = mp + 16;
  memcpy(header, "PCMP", 4);
  store16(header + 4, 224);
  header[6] = 4;
  memcpy(header + 8, "VMMLITE ", 8);
  memcpy(header + 16, "WHP x86_64  ", 12);
  store16(header + 34, 21);
  store32(header + 36, APIC_DEFAULT_PHYS_BASE);
  uint8_t *cpu = mp + 60;
  cpu[2] = 0x14; cpu[3] = 3; // APIC ID 0, enabled bootstrap processor.
  uint8_t *bus = mp + 80;
  bus[0] = 1;
  memcpy(bus + 2, "ISA   ", 6);
  uint8_t *ioapic = mp + 88;
  ioapic[0] = 2; ioapic[1] = 1; ioapic[2] = 0x11; ioapic[3] = 1;
  store32(ioapic + 4, IO_APIC_DEFAULT_PHYS_BASE);
  for (unsigned irq = 0; irq < 16; irq++) {
    uint8_t *route = mp + 96 + irq * 8;
    route[0] = 3; route[5] = (uint8_t)irq;
    route[6] = 1; route[7] = (uint8_t)irq; // ISA IRQ n -> IOAPIC pin n.
  }
  mp[224] = 4; mp[225] = 3; mp[230] = 0xff; // ExtINT -> LINT0.
  mp[232] = 4; mp[233] = 1; mp[238] = 0xff; mp[239] = 1; // NMI -> LINT1.
  header[7] = checksum(header, 224);
  mp[10] = checksum(mp, 16);
  store16(ram.bytes + 0x413, 640); // BIOS data: Linux searches below this conventional-memory size.
}

// Section 6: a flat GDT and three paging levels. The guest's first GiB is identity mapped;
// section 2 backs only its first 512 MiB with RAM. Linux replaces these boot tables later.
static void setup_page_tables(struct memory ram) {
  store64(ram.bytes + GDT_START + 16, UINT64_C(0x00af9b000000ffff));
  store64(ram.bytes + GDT_START + 24, UINT64_C(0x00cf93000000ffff));
  store64(ram.bytes + PML4_START, PDPT_START | 3);
  store64(ram.bytes + PDPT_START, PD_START | 3);
  for (unsigned page = 0; page < 512; page++)
    store64(ram.bytes + PD_START + page * 8, (uint64_t)page * (2u << 20) | 0x83);
}

#ifndef VMMLITE_TEST
struct vm {
  WHV_PARTITION_HANDLE partition;
  WHV_EMULATOR_HANDLE emulator;
  struct memory ram;
  struct pc *pc;
  struct console *console;
  HANDLE stop, timer, timer_thread;
  volatile LONG timer_error;
  LARGE_INTEGER frequency;
};

static bool checked(HRESULT result, const char *operation) {
  if (SUCCEEDED(result)) return true;
  fprintf(stderr, "%s: HRESULT 0x%08lx\n", operation, (unsigned long)result);
  return false;
}

static bool win_checked(BOOL result, const char *operation) {
  if (result) return true;
  fprintf(stderr, "%s: Win32 error %lu\n", operation, GetLastError());
  return false;
}

static uint64_t now_ns(const struct vm *vm) {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  uint64_t ticks = (uint64_t)counter.QuadPart, frequency = (uint64_t)vm->frequency.QuadPart;
  return ticks / frequency * 1000000000u + ticks % frequency * 1000000000u / frequency;
}

static bool output(void *context, const uint8_t *bytes, size_t size) {
  (void)context;
  return fwrite(bytes, 1, size, stdout) == size;
}

static bool interrupt(void *context, unsigned vector, unsigned destination,
                      unsigned delivery, bool logical, bool level) {
  struct vm *vm = context;
  WHV_INTERRUPT_CONTROL request = {0};
  request.Type = delivery;
  request.DestinationMode = logical ? WHvX64InterruptDestinationModeLogical : WHvX64InterruptDestinationModePhysical;
  request.TriggerMode = level ? WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
  request.Destination = destination;
  request.Vector = vector;
  return checked(WHvRequestInterrupt(vm->partition, &request, sizeof request), "WHvRequestInterrupt");
}

// WinHvEmulation asks these callbacks for register state and translated guest pages.
// It uses the same VP 0 as section 8; it may also read RAM to fetch instruction bytes.
static HRESULT CALLBACK get_registers(void *context, const WHV_REGISTER_NAME *names,
                                      UINT32 count, WHV_REGISTER_VALUE *values) {
  struct vm *vm = context;
  return WHvGetVirtualProcessorRegisters(vm->partition, 0, names, count, values);
}
static HRESULT CALLBACK set_registers(void *context, const WHV_REGISTER_NAME *names,
                                      UINT32 count, const WHV_REGISTER_VALUE *values) {
  struct vm *vm = context;
  return WHvSetVirtualProcessorRegisters(vm->partition, 0, names, count, values);
}
static HRESULT CALLBACK translate(void *context, WHV_GUEST_VIRTUAL_ADDRESS address,
    WHV_TRANSLATE_GVA_FLAGS flags, WHV_TRANSLATE_GVA_RESULT_CODE *result,
    WHV_GUEST_PHYSICAL_ADDRESS *physical) {
  struct vm *vm = context;
  WHV_TRANSLATE_GVA_RESULT translation = {0};
  HRESULT hr = WHvTranslateGva(vm->partition, 0, address, flags, &translation, physical);
  if (SUCCEEDED(hr)) { *result = translation.ResultCode; *physical &= ~UINT64_C(4095); }
  return hr;
}

// One decoded IN/OUT. Advance the PIT first so a count read observes elapsed host time.
static HRESULT CALLBACK io(void *context, WHV_EMULATOR_IO_ACCESS_INFO *access) {
  struct vm *vm = context;
  if (access->AccessSize != 1 && access->AccessSize != 2 && access->AccessSize != 4) return E_INVALIDARG;
  pc_tick(vm->pc, now_ns(vm));
  access->Data = pc_port(vm->pc, access->Port, access->AccessSize, access->Direction != 0, access->Data);
  return pc_error(vm->pc) ? E_FAIL : S_OK;
}

// One decoded memory access: RAM, console registers, or the IOAPIC selector/window.
// WHP owns the local APIC. An unsupported MMIO access fails with its guest physical address.
static HRESULT CALLBACK memory(void *context, WHV_EMULATOR_MEMORY_ACCESS_INFO *access) {
  struct vm *vm = context;
  uint64_t address = access->GpaAddress;
  unsigned size = access->AccessSize;
  bool write = access->Direction != 0;
  if (!size || size > sizeof access->Data) return E_INVALIDARG;
  // The instruction emulator may fetch instruction/operand bytes from ordinary RAM.
  if (in_ram(vm->ram, address, size)) {
    if (write) memcpy(vm->ram.bytes + address, access->Data, size);
    else memcpy(access->Data, vm->ram.bytes + address, size);
    return S_OK;
  }
  if (size != 4 || (address & 3)) return E_INVALIDARG;
  uint32_t value = le32(access->Data);
  bool handled = false;
  if (address >= VIRTIO_CONSOLE_START && address - VIRTIO_CONSOLE_START < 4096)
    handled = write ? console_register_write(vm->console, (unsigned)(address - VIRTIO_CONSOLE_START), value) :
        console_register_read(vm->console, (unsigned)(address - VIRTIO_CONSOLE_START), &value);
  else if (address >= IO_APIC_DEFAULT_PHYS_BASE && address - IO_APIC_DEFAULT_PHYS_BASE < 4096)
    handled = pc_mmio(vm->pc, (unsigned)(address - IO_APIC_DEFAULT_PHYS_BASE), write, &value);
  if (!handled) {
    fprintf(stderr, "unhandled MMIO %s at GPA 0x%" PRIx64 "\n", write ? "write" : "read", address);
    return E_FAIL;
  }
  if (!write) store32(access->Data, value);
  return S_OK;
}

// Section 7's long-mode handoff. Values below are the cached CPU state, not just RAM's GDT.
// Set control registers and segments in one batch: intermediate CPU modes may be invalid.
static bool boot_registers(struct vm *vm) {
  const struct { WHV_REGISTER_NAME name; uint64_t value; } scalar[] = {
    {WHvX64RegisterRip, KERNEL_ENTRY}, {WHvX64RegisterRsi, ZERO_PAGE_START},
    {WHvX64RegisterRsp, ZERO_PAGE_START}, {WHvX64RegisterRflags, 2},
    {WHvX64RegisterRbp, 0}, {WHvX64RegisterRbx, 0}, {WHvX64RegisterRdi, 0},
    {WHvX64RegisterCr0, 0x80000031}, {WHvX64RegisterCr3, PML4_START},
    {WHvX64RegisterCr4, 0x20}, {WHvX64RegisterEfer, 0x500},
    {WHvX64RegisterApicBase, APIC_DEFAULT_PHYS_BASE | 0x900u}
  };
  const WHV_REGISTER_NAME segment_names[] = {WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
      WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterSs, WHvX64RegisterTr,
      WHvX64RegisterGdtr, WHvX64RegisterIdtr};
  enum { SCALARS = sizeof scalar / sizeof scalar[0], REGISTERS = SCALARS + 9 };
  WHV_REGISTER_NAME names[REGISTERS];
  WHV_REGISTER_VALUE values[REGISTERS] = {0};
  for (unsigned i = 0; i < SCALARS; i++) {
    names[i] = scalar[i].name; values[i].Reg64 = scalar[i].value;
  }
  for (unsigned i = 0; i < 9; i++) names[SCALARS + i] = segment_names[i];
  WHV_REGISTER_VALUE *segments = values + SCALARS;
  for (unsigned i = 0; i < 6; i++) {
    segments[i].Segment.Limit = UINT32_MAX;
    segments[i].Segment.Selector = i ? 0x18 : 0x10;
    segments[i].Segment.Attributes = i ? 0xc093 : 0xa09b;
  }
  segments[6].Segment.Base = 0x1000;
  segments[6].Segment.Limit = 0x67;
  segments[6].Segment.Attributes = 0x8b;
  segments[7].Table.Base = GDT_START;
  segments[7].Table.Limit = 31;
  // Install CR0/CR4/EFER and segment state together, without intermediate modes.
  return checked(set_registers(vm, names, REGISTERS, values), "set initial CPU state");
}

// A legacy PIC interrupt is an ExtINT event, with the vector the PIC selected.
// Wait for IF=1, no interrupt shadow, and no other pending event; otherwise ask WHP for a window.
static bool pic_interrupt(struct vm *vm) {
  int vector = pc_pic_pending(vm->pc);
  if (vector < 0) return true;
  const WHV_REGISTER_NAME names[] = {WHvX64RegisterRflags, WHvRegisterInterruptState,
      WHvRegisterPendingInterruption, WHvRegisterPendingEvent, WHvRegisterInternalActivityState};
  WHV_REGISTER_VALUE values[5] = {0};
  if (!checked(get_registers(vm, names, 5, values), "read PIC delivery state")) return false;
  bool ready = (values[0].Reg64 & 0x200) && !values[1].InterruptState.InterruptShadow &&
      !values[2].PendingInterruption.InterruptionPending && !values[3].ExtIntEvent.EventPending;
  WHV_REGISTER_NAME notification_name = WHvX64RegisterDeliverabilityNotifications;
  WHV_REGISTER_VALUE notification = {0};
  notification.DeliverabilityNotifications.InterruptNotification = !ready;
  if (!checked(set_registers(vm, &notification_name, 1, &notification), "set PIC interrupt window")) return false;
  if (!ready) return true;
  const WHV_REGISTER_NAME event_names[] = {WHvRegisterPendingEvent, WHvRegisterInternalActivityState};
  WHV_REGISTER_VALUE events[2] = {0};
  events[0].ExtIntEvent.EventPending = 1;
  events[0].ExtIntEvent.EventType = WHvX64PendingEventExtInt;
  events[0].ExtIntEvent.Vector = (uint8_t)vector;
  events[1] = values[4];
  events[1].InternalActivity.HaltSuspend = 0; // The external interrupt wakes a halted VP.
  if (!checked(set_registers(vm, event_names, 2, events), "inject PIC external interrupt")) return false;
  pc_pic_ack(vm->pc);
  return true;
}

// Complete an intercepted CPUID using WHP's partition defaults. This sample does not provide
// the Hyper-V guest ABI, so hide its identification. This handler, unlike WinHvEmulation,
// must advance RIP itself.
static bool cpuid(struct vm *vm, const WHV_RUN_VP_EXIT_CONTEXT *exit) {
  const WHV_X64_CPUID_ACCESS_CONTEXT *result = &exit->CpuidAccess;
  const WHV_REGISTER_NAME names[] = {WHvX64RegisterRax, WHvX64RegisterRbx,
      WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRip};
  WHV_REGISTER_VALUE values[] = {{.Reg64 = result->DefaultResultRax},
      {.Reg64 = result->DefaultResultRbx}, {.Reg64 = result->DefaultResultRcx},
      {.Reg64 = result->DefaultResultRdx},
      {.Reg64 = exit->VpContext.Rip + exit->VpContext.InstructionLength}};
  // Use WHP's filtered CPU model. No Hyper-V guest ABI is implemented by this VMM.
  if (result->Rax == 1) values[2].Reg64 &= ~(UINT64_C(1) << 31);
  if (result->Rax >= 0x40000000 && result->Rax < 0x50000000)
    for (unsigned i = 0; i < 4; i++) values[i].Reg64 = 0;
  return checked(set_registers(vm, names, 5, values), "complete CPUID");
}

// Wake the VP loop every millisecond so it can service the PIT, even while the guest is halted.
// This helper never touches guest RAM or device state. Cleanup joins it before deleting VP 0.
static DWORD WINAPI timer_thread(void *context) {
  struct vm *vm = context;
  HANDLE events[] = {vm->stop, vm->timer};
  for (;;) {
    DWORD event = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (event == WAIT_OBJECT_0) return 0;
    HRESULT result = event == WAIT_OBJECT_0 + 1 ? WHvCancelRunVirtualProcessor(vm->partition, 0, 0) : E_FAIL;
    if (FAILED(result)) { InterlockedExchange(&vm->timer_error, result); return 1; }
  }
}

static bool read_image(const char *path, struct image *image) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); return false; }
  bool success = false;
  uint8_t *bytes = NULL;
  if (fseek(file, 0, SEEK_END) != 0) goto done;
  long size = ftell(file);
  if (size <= 0 || (unsigned long)size > RAM_SIZE || fseek(file, 0, SEEK_SET) != 0) goto done;
  bytes = malloc((size_t)size);
  if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) goto done;
  *image = (struct image){bytes, (size_t)size};
  success = true;
done:
  if (!success) { fprintf(stderr, "cannot read %s (expected 1..512 MiB)\n", path); free(bytes); }
  fclose(file);
  return success;
}

int main(int argc, char **argv) {
  if (argc != 3 && !(argc == 2 && !strcmp(argv[1], "--probe"))) {
    fputs("usage: vmmlite.exe bzImage initramfs.cpio\n       vmmlite.exe --probe\n", stderr);
    return 1;
  }
  BOOL present = FALSE;
  WHV_CAPABILITY_FEATURES features = {0};
  if (!checked(WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &present, sizeof present, NULL), "query hypervisor") ||
      !checked(WHvGetCapability(WHvCapabilityCodeFeatures, &features, sizeof features, NULL), "query WHP features")) return 1;
  if (!present || !features.LocalApicEmulation) {
    fputs("Enable Windows Hypervisor Platform and CPU virtualization, then reboot Windows. Local APIC emulation is required.\n", stderr);
    return 1;
  }
  if (argc == 2) { puts("WHP hypervisor and local APIC emulation are available."); return 0; }

  struct vm vm = {0};
  struct image kernel = {0}, initrd = {0};
  bool mapped = false, processor = false;
  int result = 1;
  _setmode(_fileno(stdout), _O_BINARY);
  setvbuf(stdout, NULL, _IONBF, 0);
#define CHECK(call) do { if (!checked((call), #call)) goto done; } while (0)
  if (!read_image(argv[1], &kernel) || !read_image(argv[2], &initrd)) goto done;
  if (!win_checked(QueryPerformanceFrequency(&vm.frequency), "QueryPerformanceFrequency")) goto done;
  // ------------------------------------------------------------------------------------------
  // 1. A VM: an empty partition with one processor and a local APIC.
  //
  //   create partition -> configure processor count, xAPIC, CPUID exits -> setup partition
  //   WHP owns the LAPIC; pc_create() supplies the PIC, IOAPIC and PIT in this process.
  //
  CHECK(WHvCreatePartition(&vm.partition));
  UINT32 count = 1;
  CHECK(WHvSetPartitionProperty(vm.partition, WHvPartitionPropertyCodeProcessorCount, &count, sizeof count));
  WHV_X64_LOCAL_APIC_EMULATION_MODE apic = WHvX64LocalApicEmulationModeXApic;
  CHECK(WHvSetPartitionProperty(vm.partition, WHvPartitionPropertyCodeLocalApicEmulationMode, &apic, sizeof apic));
  WHV_EXTENDED_VM_EXITS exits = {0};
  exits.X64CpuidExit = 1;
  CHECK(WHvSetPartitionProperty(vm.partition, WHvPartitionPropertyCodeExtendedVmExits, &exits, sizeof exits));
  UINT32 cpuid_exits[] = {1, 0x40000000, 0x40000001};
  CHECK(WHvSetPartitionProperty(vm.partition, WHvPartitionPropertyCodeCpuidExitList, cpuid_exits, sizeof cpuid_exits));
  CHECK(WHvSetupPartition(vm.partition));
  vm.pc = pc_create(output, interrupt, &vm);
  if (!vm.pc) { fputs("device allocation failed\n", stderr); goto done; }

  // ------------------------------------------------------------------------------------------
  // 2. Guest RAM: plain host memory mapped at guest physical address 0.
  //
  //   vm.ram.bytes + RAM_SIZE -> +---------+         +---------+ <- 0x20000000
  //                              | 512 MiB |<=======>| 512 MiB |
  //   vm.ram.bytes ------------> +---------+         +---------+ <- 0x00000000
  //   The same bytes: guest physical X is vm.ram.bytes[X] in this process.
  //   The console, IOAPIC and LAPIC addresses are above RAM, outside this mapping.
  //
  vm.ram = (struct memory){VirtualAlloc(NULL, RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), RAM_SIZE};
  if (!vm.ram.bytes) { win_checked(FALSE, "VirtualAlloc guest RAM"); goto done; }
  CHECK(WHvMapGpaRange(vm.partition, vm.ram.bytes, 0, RAM_SIZE,
      WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute));
  mapped = true;
  vm.console = console_create(vm.ram, vm.pc, output, &vm);
  if (!vm.console) { fputs("device allocation failed\n", stderr); goto done; }

  // ------------------------------------------------------------------------------------------
  // 3. The kernel: skip the boot sector and 16-bit setup code; copy the payload to 1 MiB.
  //
  //   bzImage                               guest RAM
  //   boot sector + setup sectors           skipped; the header is reused in section 4
  //   decompressor + compressed vmlinux --> 0x100000; 64-bit entry at +0x200
  //
  //   load_kernel() checks both images first: the initramfs must sit above the entire
  //   aligned kernel initialization window, including relocation to pref_address.
  struct boot_layout layout;
  const char *boot_error = load_kernel(vm.ram, kernel, initrd, &layout);
  if (boot_error) { fprintf(stderr, "%s\n", boot_error); goto done; }

  // ------------------------------------------------------------------------------------------
  // 4. The zero page: the header, command line, initramfs address/size and E820 RAM map.
  //
  //   guest physical    contents, high addresses on top       written by
  //   0x20000000      +-----------------------------------+  end of RAM
  //                   | initramfs, page aligned           |  here, below the initrd ceiling
  //   initrd_start    +-----------------------------------+
  //                   | free RAM and kernel runtime area  |  Linux unpacks here
  //   0x00100000      | decompressor + compressed kernel  |  section 3
  //                   +-----------------------------------+
  //   0x0009fc00      | MP table, then the legacy hole    |  section 5; omitted from E820 RAM
  //   0x00020000      | command line                      |  here
  //   0x00009000      | PML4, PDPT, PD                    |  section 6
  //   0x00007000      | zero page: boot_params            |  here; RSI points to it
  //   0x00000500      | GDT                               |  section 6
  //   0x00000000      +-----------------------------------+
  //
  //   earlycon -> COM1 until hvc0 takes over; reboot=k -> i8042 reset in section 8.
  setup_zero_page(vm.ram, kernel, initrd, layout);

  // ------------------------------------------------------------------------------------------
  // 5. The MP table: one CPU, one IOAPIC, ISA IRQ n -> IOAPIC pin n.
  // Linux searches below the conventional-memory ceiling for _MP_, then follows it to PCMP.
  // These are hardware descriptions in RAM; there is no BIOS or UEFI code to execute.
  setup_mptable(vm.ram);

  // ------------------------------------------------------------------------------------------
  // 6. Long mode needs a GDT and page tables in guest RAM, where the CPU reads them.
  //
  //   0x0500 GDT   [0] null  [1] unused  [2] 0x10: 64-bit code  [3] 0x18: data
  //   0x9000 PML4  [0] -> 0xa000
  //   0xa000 PDPT  [0] -> 0xb000
  //   0xb000 PD    [n] -> n * 2 MiB, n < 512: virtual X = physical X below 1 GiB
  setup_page_tables(vm.ram);

  // ------------------------------------------------------------------------------------------
  // 7. One vCPU: the registers the 64-bit boot protocol wants, installed together.
  //
  //   CS / data        0x10 / 0x18           the GDT entries from section 6
  //   GDTR / CR3       0x500 / 0x9000        GDT and PML4
  //   CR0 / CR4 / EFER 0x80000031 / 0x20 / 0x500: protected mode, paging, PAE, long mode
  //   RIP / RSI        0x100200 / 0x7000     kernel entry and zero page
  //   RFLAGS           2                    interrupts initially off; bit 1 fixed
  //   CPUID            WHP defaults         selected leaves exit to cpuid() in section 8
  CHECK(WHvCreateVirtualProcessor(vm.partition, 0, 0));
  processor = true;
  if (!boot_registers(&vm)) goto done;

  // ------------------------------------------------------------------------------------------
  // 8. Run. WHP returns an exit context; WinHvEmulation completes each IO/MMIO instruction.
  //
  //   access / exit        handled by                       result
  //   IN / OUT             io() -> pc_port()                PIC, PIT, COM1; 0x64=0xfe resets
  //   console MMIO         memory() -> console_register_*() registers, queue 1 -> stdout, IRQ 5
  //   IOAPIC MMIO          memory() -> pc_mmio()             selector and redirection entries
  //   CPUID                cpuid()                          filtered defaults, advance RIP
  //   LAPIC EOI            pc_eoi()                         rearm level interrupts
  //   timer cancellation   next loop's pc_tick()             service elapsed PIT deadlines
  //
  // The emulator owns RIP for IO/MMIO. CPUID is the only exit completed manually here.
  WHV_EMULATOR_CALLBACKS callbacks = {.Size = sizeof callbacks,
      .WHvEmulatorIoPortCallback = io, .WHvEmulatorMemoryCallback = memory,
      .WHvEmulatorGetVirtualProcessorRegisters = get_registers,
      .WHvEmulatorSetVirtualProcessorRegisters = set_registers,
      .WHvEmulatorTranslateGvaPage = translate};
  CHECK(WHvEmulatorCreateEmulator(&callbacks, &vm.emulator));
  vm.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  vm.timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!vm.stop || !vm.timer) { win_checked(FALSE, "create timer events"); goto done; }
  LARGE_INTEGER due = {.QuadPart = -10000}; // 1 ms, expressed in 100 ns units.
  if (!win_checked(SetWaitableTimer(vm.timer, &due, 1, NULL, NULL, FALSE), "SetWaitableTimer")) goto done;
  vm.timer_thread = CreateThread(NULL, 0, timer_thread, &vm, 0, NULL);
  if (!vm.timer_thread) { win_checked(FALSE, "CreateThread"); goto done; }
  while (!pc_reset_requested(vm.pc)) {
    HRESULT timer_result = InterlockedCompareExchange(&vm.timer_error, 0, 0);
    if (!checked(timer_result, "timer thread")) goto done;
    pc_tick(vm.pc, now_ns(&vm));
    if (pc_error(vm.pc) || !pic_interrupt(&vm)) goto done;
    WHV_RUN_VP_EXIT_CONTEXT exit = {0};
    if (!checked(WHvRunVirtualProcessor(vm.partition, 0, &exit, sizeof exit), "WHvRunVirtualProcessor")) goto done;
    WHV_EMULATOR_STATUS status = {0};
    HRESULT emulation_result;
    switch (exit.ExitReason) {
    case WHvRunVpExitReasonX64IoPortAccess:
      emulation_result = WHvEmulatorTryIoEmulation(vm.emulator, &vm, &exit.VpContext, &exit.IoPortAccess, &status);
      break;
    case WHvRunVpExitReasonMemoryAccess:
      emulation_result = WHvEmulatorTryMmioEmulation(vm.emulator, &vm, &exit.VpContext, &exit.MemoryAccess, &status);
      break;
    case WHvRunVpExitReasonX64Cpuid:
      if (!cpuid(&vm, &exit)) goto done;
      continue;
    case WHvRunVpExitReasonX64ApicEoi:
      pc_eoi(vm.pc, exit.ApicEoi.InterruptVector);
      continue;
    case WHvRunVpExitReasonCanceled:
    case WHvRunVpExitReasonX64InterruptWindow:
      continue;
    case WHvRunVpExitReasonX64Halt:
      // Modern WHP handles HLT internally. Avoid a tight loop on older hosts.
      Sleep(1);
      continue;
    default:
      fprintf(stderr, "unexpected VP exit 0x%x at RIP 0x%" PRIx64 "\n",
          (unsigned)exit.ExitReason, (uint64_t)exit.VpContext.Rip);
      goto done;
    }
    if (!checked(emulation_result, "WHP instruction emulator") || !status.EmulationSuccessful) {
      fprintf(stderr, "emulation status 0x%08x at RIP 0x%" PRIx64 "\n",
          (unsigned)status.AsUINT32, (uint64_t)exit.VpContext.Rip);
      goto done;
    }
  }
  fprintf(stderr, "guest reset; virtio TX bytes=%" PRIu64 "\n", console_bytes(vm.console));
  result = 0;
done:
  // Stop the helper before freeing anything it can reach, including the partition and VP.
  if (vm.timer_thread) { SetEvent(vm.stop); WaitForSingleObject(vm.timer_thread, INFINITE); CloseHandle(vm.timer_thread); }
  if (vm.timer) CloseHandle(vm.timer);
  if (vm.stop) CloseHandle(vm.stop);
  if (vm.pc && pc_error(vm.pc)) fprintf(stderr, "%s\n", pc_error(vm.pc));
  if (vm.console && console_error(vm.console)) fprintf(stderr, "%s\n", console_error(vm.console));
  if (vm.emulator && !checked(WHvEmulatorDestroyEmulator(vm.emulator), "destroy emulator")) result = 1;
  console_destroy(vm.console);
  pc_destroy(vm.pc);
  if (processor && !checked(WHvDeleteVirtualProcessor(vm.partition, 0), "delete VP")) result = 1;
  if (mapped && !checked(WHvUnmapGpaRange(vm.partition, 0, RAM_SIZE), "unmap RAM")) result = 1;
  if (vm.partition && !checked(WHvDeletePartition(vm.partition), "delete partition")) result = 1;
  if (vm.ram.bytes) VirtualFree(vm.ram.bytes, 0, MEM_RELEASE);
  free((void *)kernel.bytes);
  free((void *)initrd.bytes);
  return result;
}
#endif
