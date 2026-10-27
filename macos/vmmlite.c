// vmmlite.c: a tiny VMM that boots Linux (arm64, Hypervisor.framework). For learning only.
// Build: make    Run: .build/vmmlite Image .build/initramfs.cpio .build/machine.dtb
//
//   vmmlite (this process)       Hypervisor.framework           Apple Silicon
//   +--------------------+ calls +-----------------------+ entry +--------------------+
//   | ram: guest RAM     |------>| VM: stage-2 mappings, |------>| runs guest code    |
//   | exit: reason, ESR  |<------| GICv3, timer; vCPU    |<------| until a VM exit    |
//   +--------------------+ exits +-----------------------+ exit  +--------------------+
//
//   Same small machine as the KVM example: one CPU, 512 MiB, serial output, hvc0, reset.
//   ARM replaces the PC boot tables with a device tree and the i8042 reset with PSCI.
//   No host input, disk, network, firmware, second CPU or general device framework.
#include <Hypervisor/Hypervisor.h>                        // VM, vCPU, registers, exits and the framework GIC
#include <libfdt.h>                                       // edit the compiled device tree without writing an FDT parser
#include <inttypes.h>                                     // fixed-width addresses and PRIx64 for exit diagnostics
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>                                     // mmap(): the host memory that becomes guest RAM
#include <sys/stat.h>
#include <unistd.h>

#define RAM_BASE UINT64_C(0x40000000)                     // guest RAM starts at 1 GiB, a 2 MiB-aligned base
#define RAM_SIZE (512UL << 20)                            // 512 MiB; RAM ends at 0x60000000
#define DTB_BASE UINT64_C(0x48000000)                     // the device tree; x0 points here at kernel entry
#define DTB_SIZE (2UL << 20)                              // 2 MiB of workspace while libfdt edits the tree
#define INITRD_BASE UINT64_C(0x50000000)                  // the cpio archive containing /init
#define UART_BASE UINT64_C(0x09000000)                    // the 8250 UART: 8 byte-wide MMIO registers
#define CONSOLE_BASE UINT64_C(0x0a000000)                 // the virtio console: 4 KiB of MMIO registers
#define GICD_BASE UINT64_C(0x08000000)                    // GIC distributor: routes shared interrupts
#define GICR_BASE UINT64_C(0x10000000)                    // GIC redistributor: this CPU's private interrupts
#define UART_IRQ 32                                       // GIC INTID 32 = SPI 0 in the device tree
#define CONSOLE_IRQ 33                                    // GIC INTID 33 = SPI 1 in the device tree
#define QUEUE_MAX 256                                     // entries in each of the transmit queue's three areas
// The 8250 register offsets and bits used by com1(), with Linux serial_reg.h names.
enum { UART_TX = 0, UART_IER = 1, UART_IIR = 2, UART_LCR = 3, UART_LSR = 5,
       UART_IER_THRI = 2, UART_IIR_NO_INT = 1, UART_IIR_THRI = 2,
       UART_LCR_DLAB = 128, UART_LSR_TEMT = 64, UART_LSR_THRE = 32 };
// Virtio 1.0+ MMIO register offsets. The table by console_register_* explains each one.
enum { MAGIC_VALUE = 0x000, VERSION = 0x004, DEVICE_ID = 0x008,
       DEVICE_FEATURES = 0x010, DEVICE_FEATURES_SEL = 0x014,
       DRIVER_FEATURES = 0x020, DRIVER_FEATURES_SEL = 0x024,
       QUEUE_SEL = 0x030, QUEUE_NUM_MAX = 0x034, QUEUE_NUM = 0x038, QUEUE_READY = 0x044,
       QUEUE_NOTIFY = 0x050, INTERRUPT_STATUS = 0x060, INTERRUPT_ACK = 0x064,
       STATUS = 0x070, QUEUE_DESC_LOW = 0x080, QUEUE_AVAIL_LOW = 0x090, QUEUE_USED_LOW = 0x0a0 };

static unsigned char *ram;                                // guest address X is at ram + (X - RAM_BASE) here
static void require(int ok, const char *why) {
  if (!ok) { fprintf(stderr, "vmmlite: %s\n", why); exit(1); }
}
#define HV(call) do { hv_return_t r = (call); if (r != HV_SUCCESS) { \
  fprintf(stderr, "%s: %#x\n", #call, r); exit(1); } } while (0)
#define FDT(call) do { int r = (call); if (r < 0) { \
  fprintf(stderr, "%s: %s\n", #call, fdt_strerror(r)); exit(1); } } while (0)

// Guest physical address -> offset from RAM_BASE -> host pointer
//   0x40001000           -> 0x1000                -> ram + 0x1000
// Check the whole range by subtraction to avoid overflow. Devices and gaps are not RAM.
static void *guest(uint64_t address, size_t size) {
  require(address >= RAM_BASE && size <= RAM_SIZE &&
          address - RAM_BASE <= RAM_SIZE - size, "guest address outside RAM");
  return ram + (address - RAM_BASE);
}

// A whole file into an already chosen part of RAM; capacity keeps it out of the next region.
// The Image, initramfs and DTB use the same helper, and errors stop this one-shot process.
static size_t load(const char *path, void *destination, size_t capacity) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); exit(1); }
  struct stat st;
  require(!fstat(fileno(file), &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
          (uint64_t)st.st_size <= capacity, "input empty, too large, or not a regular file");
  size_t size = st.st_size;
  require(fread(destination, 1, size, file) == size, "short input read");
  require(!fclose(file), "closing input failed");
  return size;
}

// The ARM64 Image header, unlike x86's bzImage: the file is already uncompressed.
//
//   byte offset   field         meaning
//   0             code0/code1   instructions: execution starts at the Image's first byte
//   8             text_offset   place the Image this far from a 2 MiB-aligned RAM base
//   16            image_size    reserve this many bytes, including space not in the file
//   24            flags         bit 0: big endian; this example accepts little endian only
//   56            magic         41 52 4d 64: "ARM" followed by byte 0x64
//
//   file -> ram (temporary) -> memmove to ram + text_offset (final)
//   PC = RAM_BASE + text_offset; the pinned test Image uses offset 0.
// The whole image_size extent must fit before the DTB; Linux builds its own page tables.

static uint64_t kernel_entry(size_t file_size) {
  uint64_t offset, image_size, flags;
  require(file_size >= 64 && !memcmp(ram + 56, "ARM\x64", 4), "expected an uncompressed ARM64 Image");
  memcpy(&offset, ram + 8, 8); memcpy(&image_size, ram + 16, 8); memcpy(&flags, ram + 24, 8);
  require(image_size && !(flags & 1), "need a modern little-endian Image");
  require(offset < DTB_BASE - RAM_BASE && image_size >= file_size &&
          image_size <= DTB_BASE - RAM_BASE - offset, "kernel overlaps device tree");
  memmove(ram + offset, ram, file_size);
  return RAM_BASE + offset;
}

// COM1: the same 8250 register subset as Linux's com1(), at 0x09000000 rather than port 0x3f8.
// ARM has no IN/OUT port space: the driver uses loads/stores, which exit through mmio().
//
//   reg      guest reads                           guest writes
//   0 TX/RX  0, no input                           putchar(byte), mark transmitter empty
//   1 IER    saved IER                             IER; enabling THRI raises "TX empty"
//   2 IIR    THRI if pending, else NO_INT           FIFO control, ignored: no FIFO
//   3 LCR    saved LCR                             LCR; DLAB selects the baud divisor
//   5 LSR    TEMT | THRE: transmitter empty        ignored
//
// DLAB=1: registers 0/1 select baud-divisor bytes; writes do not print or change IER.
//   TX-empty event + IER.THRI -> INTID 32 high -> read IIR -> clear event, lower INTID 32
//   DT SPI 0 -> INTID 32 (32 + 0); the 8250 driver discovers this UART as ttyS0.
//   earlycon polls UART -> hvc0 registers -> virtio carries later logs and PID 1 output.

static unsigned char com1(int reg, int is_out, unsigned char byte) {
  static unsigned char ier, lcr, thr_empty;
  unsigned char reply = 0;
  if (lcr & UART_LCR_DLAB && reg <= UART_IER) return 0;
  if (is_out) {
    if (reg == UART_TX) { require(putchar(byte) != EOF, "UART write failed"); thr_empty = 1; }
    if (reg == UART_IER) { thr_empty |= byte & ~ier & UART_IER_THRI; ier = byte; }
    if (reg == UART_LCR) lcr = byte;
  } else {
    if (reg == UART_IER) reply = ier;
    if (reg == UART_IIR) reply = ier & UART_IER_THRI && thr_empty ? UART_IIR_THRI : UART_IIR_NO_INT;
    if (reg == UART_IIR && reply == UART_IIR_THRI) thr_empty = 0;
    if (reg == UART_LCR) reply = lcr;
    if (reg == UART_LSR) reply = UART_LSR_TEMT | UART_LSR_THRE;
  }
  HV(hv_gic_set_spi(UART_IRQ, ier & UART_IER_THRI && thr_empty));
  return reply;
}

// The virtio console: Linux's hvc0, at 0x0a000000 on INTID 33. As in the KVM example,
// queue 0 is advertised but never served; only queue 1, output, needs backing state.
//
//   function                    called from       what it does
//   console_register_read()     mmio(), section 8 answers a register read
//   console_register_write()    mmio(), section 8 keeps setup, acknowledges IRQs, dispatches kicks
//   console_kick()              QUEUE_NOTIFY      prints every new chain and returns its buffers
//
//   guest                         Hypervisor.framework             vmmlite
//   setup registers  ---------->  MMIO data-abort exit  ----------> console_register_*()
//   kick queue 1     ---------->  MMIO data-abort exit  ----------> console_kick()
//   see used buffers <------------------------------------------- writes to shared RAM
//   take IRQ         <----------  framework GIC         <--------- hv_gic_set_spi(33, true)
//
//   KVM sibling: kick -> eventfd -> worker; here: kick -> VM exit -> inline queue work.
// With one stopped vCPU and no worker, the guest cannot change buffers during console_kick().
//
// The IRQ must agree in two places:
//   machine.dts: interrupts = <0 1 4>     type 0 = SPI, number 1, flags 4 = level high
//   CONSOLE_IRQ: 33                       SPI numbers begin at INTID 32, so 32 + 1
// Linux allocates a separate software IRQ number; do not pass that number to the framework.

struct queue { uint32_t size, ready; uint64_t desc, avail, used; uint16_t next; };
static struct {
  struct queue transmit;
  uint32_t selected, feature_page, driver_page, status, interrupt;
  uint64_t features;
} console;
// One 16-byte direct descriptor; all fields are little endian, as on this host and guest.
// flags bit 0 = NEXT; bit 1 = device-writable; bit 2 = indirect. Output accepts only NEXT.
struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };

// Queue size is a power of two, no larger than 256. Each area must fit in mapped RAM:
//   desc:  16 * size bytes, aligned to 16   {address, length, flags, next} per buffer
//   avail: 6 + 2 * size bytes, aligned to 2 {flags, idx, ring[size], used_event}
//   used:  6 + 8 * size bytes, aligned to 4 {flags, idx, {id,len}[size], avail_event}
// EVENT_IDX is not offered, so the two trailing event fields are not used.
static void queue_check(struct queue *q) {
  require(q->size && q->size <= QUEUE_MAX && !(q->size & (q->size - 1)), "invalid queue size");
  require(!(q->desc & 15) && !(q->avail & 1) && !(q->used & 3), "unaligned queue");
  guest(q->desc, 16 * q->size); guest(q->avail, 6 + 2 * q->size); guest(q->used, 6 + 8 * q->size);
}

// One output chain. QUEUE_NOTIFY carries a queue number, not the bytes to print.
//
//   Linux:   data -> desc[head] -> avail.ring[slot] -> avail.idx++ -> QUEUE_NOTIFY=1
//   vmmlite: follow NEXT -> fwrite(data) -> used.ring[slot] = {head, 0}
//                                      -> publish used.idx (release, per chain)
//   after draining: INTID 33 high -> Linux reads used ring -> reclaims buffers
//
// Completion length is 0: output reads guest bytes; it writes none back to those buffers.
// uint16_t indices wrap at 65536; ring slots wrap at queue size. Bound every address and
// NEXT chain. Publish the completion before used.idx so Linux sees it when it resumes.
static void console_kick(uint32_t number) {
  if (number != 1) return;                 // No host input is offered on queue 0.
  struct queue *q = &console.transmit;
  require(q->ready && (console.status & 4), "transmit before DRIVER_OK/QueueReady");
  queue_check(q);
  uint16_t *avail = guest(q->avail, 6 + 2 * q->size);
  uint16_t *used = guest(q->used, 6 + 8 * q->size);
  require((uint16_t)(avail[1] - q->next) <= q->size, "available ring overrun");
  while (q->next != avail[1]) {
    uint16_t head = avail[2 + q->next % q->size], index = head;
    for (unsigned walked = 0;; walked++) {
      require(index < q->size && walked < q->size, "invalid or cyclic descriptor chain");
      struct descriptor d;
      memcpy(&d, guest(q->desc + 16 * index, sizeof d), sizeof d);
      require(!(d.flags & ~1u), "transmit descriptor must be readable and direct");
      require(fwrite(guest(d.address, d.length), 1, d.length, stdout) == d.length, "console write failed");
      if (!(d.flags & 1)) break;
      index = d.next;
    }
    uint32_t completion[] = {head, 0};      // Device wrote no bytes into the guest's buffers.
    memcpy((char *)used + 4 + 8 * (used[1] % q->size), completion, sizeof completion);
    __atomic_store_n(&used[1], (uint16_t)(used[1] + 1), __ATOMIC_RELEASE);
    q->next++;
  }
  require(!fflush(stdout), "console flush failed");
  console.interrupt |= 1;
  HV(hv_gic_set_spi(CONSOLE_IRQ, true));
}

// The control path: one 32-bit MMIO access per register, reg relative to 0x0a000000.
//
//   offset  register              guest reads                 guest writes
//   0x000   MAGIC_VALUE           0x74726976: "virt"          ignored
//   0x004   VERSION               2: modern virtio MMIO        ignored
//   0x008   DEVICE_ID             3: console                   ignored
//   0x010   DEVICE_FEATURES       bit 32, VERSION_1, by page   ignored
//   0x014   DEVICE_FEATURES_SEL   0                            page number
//   0x020   DRIVER_FEATURES       0                            accepted features, by page
//   0x024   DRIVER_FEATURES_SEL   0                            page number
//   0x030   QUEUE_SEL             0                            selected queue
//   0x034   QUEUE_NUM_MAX         256 for queues 0 and 1      ignored
//   0x038   QUEUE_NUM             0                            queue size, for queue 1
//   0x044   QUEUE_READY           queue 1's ready bit          validate and enable queue 1
//   0x050   QUEUE_NOTIFY          0                            1: drain output; 0: ignored
//   0x060   INTERRUPT_STATUS      used-buffer bit              ignored
//   0x064   INTERRUPT_ACK         0                            clear bits, lower IRQ if none
//   0x070   STATUS                saved status                 status; 0 resets all state
//   0x080   QUEUE_DESC_LOW        0                            descriptor table address
//   0x090   QUEUE_AVAIL_LOW       0                            available ring address
//   0x0a0   QUEUE_USED_LOW        0                            used ring address
//   +4 to each address register is its high 32 bits. Other reads return 0; writes are ignored.
//
// Linux's setup, the same virtio protocol as on x86:
//   reset -> ACKNOWLEDGE -> DRIVER -> accept VERSION_1 -> FEATURES_OK -> queues -> DRIVER_OK
//   STATUS: 0, 1, 3, 0xb, 0xf. Only bit 32 is offered; no packed rings or indirect descriptors.
//   Queue 0 setup is ignored, deliberately matching the Linux learning example; no input comes.
//   Queue 1 retains its addresses. Changing them while ready is ignored; STATUS 0 resets them.
//   used buffers -> INTERRUPT_STATUS bit 0 -> INTID 33 high
//   Linux writes INTERRUPT_ACK=1 -> clear bit 0 -> INTID 33 low
// This is enough for the example's output path, not a complete virtio device implementation.
static uint32_t console_register_read(unsigned reg) {
  struct queue *q = console.selected == 1 ? &console.transmit : NULL;
  switch (reg) {
  case MAGIC_VALUE: return 0x74726976;           // MagicValue: "virt"
  case VERSION: return 2;                    // Version: modern MMIO
  case DEVICE_ID: return 3;                    // DeviceID: console
  case DEVICE_FEATURES: return console.feature_page == 1 ? 1 : 0; // VIRTIO_F_VERSION_1, bit 32
  case QUEUE_NUM_MAX: return console.selected < 2 ? QUEUE_MAX : 0;
  case QUEUE_READY: return q ? q->ready : 0;
  case INTERRUPT_STATUS: return console.interrupt;
  case STATUS: return console.status;
  default: return 0;
  }
}

static void console_register_write(unsigned reg, uint32_t value) {
  struct queue *q = console.selected == 1 ? &console.transmit : NULL;
  switch (reg) {
  case DEVICE_FEATURES_SEL: console.feature_page = value; break;
  case DRIVER_FEATURES_SEL: console.driver_page = value; break;
  case DRIVER_FEATURES:
    if (console.driver_page < 2) {
      unsigned shift = 32 * console.driver_page;
      console.features = (console.features & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)value << shift);
    }
    break;
  case QUEUE_SEL: console.selected = value; break;
  case QUEUE_NOTIFY: console_kick(value); break;
  case INTERRUPT_ACK:
    console.interrupt &= ~value;
    HV(hv_gic_set_spi(CONSOLE_IRQ, console.interrupt != 0)); break;
  case STATUS:
    if (!value) { memset(&console, 0, sizeof console); HV(hv_gic_set_spi(CONSOLE_IRQ, false)); }
    else {
      if ((value & 8) && console.features != (UINT64_C(1) << 32)) value &= ~8u;
      require(!(value & 4) || (value & 8), "DRIVER_OK without accepted features");
      console.status = value;
    }
    break;
  case QUEUE_NUM: if (q && !q->ready) q->size = value; break;
  case QUEUE_READY:
    if (q) { require(value <= 1, "invalid QueueReady"); if (value) queue_check(q); q->ready = value; }
    break;
  default:
    if (q && !q->ready && reg >= QUEUE_DESC_LOW && reg <= QUEUE_USED_LOW + 4 && !(reg & 3)) {
      uint64_t *address = reg < QUEUE_AVAIL_LOW ? &q->desc : reg < QUEUE_USED_LOW ? &q->avail : &q->used;
      if ((reg & 15) == 0) *address = (*address & UINT64_C(0xffffffff00000000)) | value;
      if ((reg & 15) == 4) *address = (*address & UINT64_C(0xffffffff)) | ((uint64_t)value << 32);
    }
  }
}

// The DTB describes hardware; the framework and device helpers implement it.
//   machine.dts --dtc--> DTB --load + libfdt patch--> guest RAM <-- x0 at kernel entry
//
//   node/property             value                         Linux learns
//   /chosen/bootargs          earlycon=... console=hvc0 ... which consoles to use
//   /chosen/linux,initrd-*    0x50000000, exclusive end     where its root archive is
//   /memory@40000000/reg      0x40000000, 512 MiB           RAM it may allocate
//   /cpus/cpu@0/reg           0                             CPU affinity (MPIDR)
//   /interrupt-controller    queried GIC region sizes      where to program the GIC
//   /timer/interrupts        queried physical/virtual IDs  which PPI each timer uses
//   /serial@9000000          UART base, SPI 0              ttyS0, INTID 32
//   /virtio_mmio@a000000     console base, SPI 1           virtio console, INTID 33
//   /psci/method             "hvc"                        how to request reset/power-off
//
// DT cells are big endian even though the guest is little endian. A 64-bit address is two
// 32-bit cells. Timer tuples are {1, INTID - 16, 4}: PPI, PPI-relative number, level high.
// The secure physical and EL2 timer entries are placeholders; this guest runs at EL1.

static void prepare_dtb(void *dtb, size_t size, size_t initrd_size) {
  FDT(fdt_check_full(dtb, size));
  FDT(fdt_open_into(dtb, dtb, DTB_SIZE));
  int chosen = fdt_path_offset(dtb, "/chosen");
  FDT(fdt_setprop_u64(dtb, chosen, "linux,initrd-start", INITRD_BASE));
  FDT(fdt_setprop_u64(dtb, chosen, "linux,initrd-end", INITRD_BASE + initrd_size));
  size_t dist_size, redist_size;
  HV(hv_gic_get_distributor_size(&dist_size)); HV(hv_gic_get_redistributor_size(&redist_size));
  fdt64_t regions[] = {cpu_to_fdt64(GICD_BASE), cpu_to_fdt64(dist_size),
                       cpu_to_fdt64(GICR_BASE), cpu_to_fdt64(redist_size)};
  FDT(fdt_setprop(dtb, fdt_path_offset(dtb, "/interrupt-controller@8000000"), "reg", regions, sizeof regions));
  uint32_t physical, virtual;
  HV(hv_gic_get_intid(HV_GIC_INT_EL1_PHYSICAL_TIMER, &physical));
  HV(hv_gic_get_intid(HV_GIC_INT_EL1_VIRTUAL_TIMER, &virtual));
  fdt32_t timers[] = {1, 13, 4, 1, physical - 16, 4, 1, virtual - 16, 4, 1, 10, 4};
  for (unsigned i = 0; i < sizeof timers / sizeof timers[0]; i++) timers[i] = cpu_to_fdt32(timers[i]);
  FDT(fdt_setprop(dtb, fdt_path_offset(dtb, "/timer"), "interrupts", timers, sizeof timers));
  FDT(fdt_pack(dtb));
}

// Finish an MMIO instruction. The unmapped guest physical address identifies the device;
// the data-abort syndrome identifies the access, so no general ARM instruction decoder is needed.
//
//   ESR bit(s)   field   use
//   31:26        EC      0x24: data abort from the lower exception level
//   24           ISV     the access fields below are valid
//   23:22        SAS     log2(bytes): 0 = byte, 2 = 32-bit word
//   21           SSE     sign-extend a load's result
//   20:16        SRT     source/destination register; 31 means the zero register
//   15           SF      1 = 64-bit X register, 0 = 32-bit W register
//   8, 7         CM,S1PTW reject cache-maintenance/page-table-walk aborts here
//   6            WnR     1 = store, 0 = load
//
//   store: Xn (or 0 for register 31) -> device helper
//   load:  device helper -> extend result -> Xn/Wn (discard for register 31)
//   both:  PC += 4 -> resume Linux
// GIC accesses stay in the framework; only UART/virtio reach these helpers.

static void mmio(hv_vcpu_t cpu, hv_vcpu_exit_t *vcpu_exit) {
  uint64_t esr = vcpu_exit->exception.syndrome, address = vcpu_exit->exception.physical_address;
  unsigned width = 1u << ((esr >> 22) & 3), rt = (esr >> 16) & 31;
  int write = (esr >> 6) & 1;
  require((esr & (1u << 24)) && !(esr & ((1u << 7) | (1u << 8))), "unsupported MMIO syndrome");
  uint64_t value = 0, pc;
  if (write && rt != 31) HV(hv_vcpu_get_reg(cpu, (hv_reg_t)(HV_REG_X0 + rt), &value));
  if (address >= UART_BASE && address < UART_BASE + 8) {
    require(width == 1, "UART needs byte accesses");
    value = com1(address - UART_BASE, write, value);
  } else {
    require(address >= CONSOLE_BASE && address < CONSOLE_BASE + 0x1000 &&
            width == 4 && !(address & 3), "unhandled MMIO address/width");
    unsigned reg = address - CONSOLE_BASE;
    if (write) console_register_write(reg, value); else value = console_register_read(reg);
  }
  if (!write && rt != 31) {
    if (esr & (1u << 21)) value = (uint64_t)((int64_t)(value << (64 - width * 8)) >> (64 - width * 8));
    if (!(esr & (1u << 15))) value = (uint32_t)value;
    HV(hv_vcpu_set_reg(cpu, (hv_reg_t)(HV_REG_X0 + rt), value));
  }
  HV(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc)); HV(hv_vcpu_set_reg(cpu, HV_REG_PC, pc + 4));
}

int main(int argc, char **argv) {
  require(argc == 4, "usage: vmmlite Image initramfs.cpio machine.dtb");
  setbuf(stdout, NULL);
  // ------------------------------------------------------------------------------------------
  // 1. A VM and its GIC; Apple requires both before creating any vCPU.
  //
  //   hv_vm_create -> hv_gic_create -> hv_vcpu_create (section 6)
  //   distributor at 0x08000000, redistributor at 0x10000000; sizes are queried in section 5.
  //
  // The framework implements the GIC; we choose its addresses, then create it for this VM.
  // Leave GIC ranges out of RAM mappings. os_release drops the configuration, not the GIC.
  HV(hv_vm_create(NULL));
  hv_gic_config_t gic = hv_gic_config_create();
  require(gic != NULL, "allocating GIC configuration failed");
  HV(hv_gic_config_set_distributor_base(gic, GICD_BASE));
  HV(hv_gic_config_set_redistributor_base(gic, GICR_BASE));
  HV(hv_gic_create(gic)); os_release(gic);

  // ------------------------------------------------------------------------------------------
  // 2. 512 MiB of host memory as guest RAM. Device addresses have no RAM backing.
  //
  //   guest virtual --Linux tables--> guest physical --framework stage 2--> host RAM
  //   host pointer for guest address X: ram + (X - RAM_BASE).
  // At entry the guest MMU is off: start at the guest-physical step above.
  // mmap supplies zero-filled RAM for [0x40000000, 0x60000000).
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  require(ram != MAP_FAILED, "allocating RAM failed");
  HV(hv_vm_map(ram, RAM_BASE, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

  // ------------------------------------------------------------------------------------------
  // 3. The uncompressed Linux Image, at the offset its header requests.
  //
  //   header text_offset 0 in the pinned test Image -> entry 0x40000000
  //   image_size reserves the whole kernel footprint, not just the bytes read from the file.
  //   This is not x86's bzImage: there is no decompressor to enter at a separate +0x200 offset.
  uint64_t entry = kernel_entry(load(argv[1], ram, DTB_BASE - RAM_BASE));

  // ------------------------------------------------------------------------------------------
  // 4. The initramfs and device tree; /chosen in the tree carries the command line.
  //
  //   0x50000000   initramfs.cpio, with one executable, /init
  //   0x48000000   machine.dtb, aligned to 8 bytes and no larger than 2 MiB
  //
  // The Image and archive share the same 1 GiB-aligned window, as the ARM64 boot protocol
  // requires. The DTB tells Linux the archive's bounds; no disk or firmware finds it for us.
  size_t initrd_size = load(argv[2], guest(INITRD_BASE, 1), RAM_BASE + RAM_SIZE - INITRD_BASE);
  void *dtb = guest(DTB_BASE, DTB_SIZE);
  size_t dtb_size = load(argv[3], dtb, DTB_SIZE);

  // ------------------------------------------------------------------------------------------
  // 5. Finish the machine description: initramfs extent, GIC regions and timer PPIs.
  //
  // ARM Linux reads the tree passed in x0. It does not scan an x86 zero page or MP table.
  // Query the framework instead of assuming its GIC region sizes or EL1 timer INTIDs.
  // fdt_pack() leaves a compact tree; Linux consumes the blob's size, not the whole workspace.
  prepare_dtb(dtb, dtb_size, initrd_size);

  // ------------------------------------------------------------------------------------------
  // 6. One vCPU, affinity 0: the same CPU that /cpus/cpu@0 describes.
  //
  //   cpu        an opaque framework handle, used by hv_vcpu_* calls
  //   vcpu_exit  a framework-owned report updated by hv_vcpu_run: reason and exception fields
  //   MPIDR_EL1  bit 31 is RES1; affinity fields are all zero, so the GIC routes to CPU 0
  //
  // The exit report is not KVM's mmap'ed run page. Register values are read/written by API.
  hv_vcpu_t cpu; hv_vcpu_exit_t *vcpu_exit;
  HV(hv_vcpu_create(&cpu, &vcpu_exit, NULL));
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MPIDR_EL1, UINT64_C(0x80000000)));

  // ------------------------------------------------------------------------------------------
  // 7. The ARM64 boot protocol: EL1h, DAIF masked, MMU off, x0 = DTB, x1-x3 = 0.
  //
  //   register     value          meaning
  //   PC           Image entry    first instruction of the Image, from section 3
  //   x0           0x48000000     pointer to the machine's DTB
  //   x1, x2, x3   0              reserved by the boot protocol
  //   CPSR/PSTATE  0x3c5          DAIF = 1111, EL1h: kernel uses SP_EL1
  //   SCTLR_EL1    0x30d00800     RES1 bits; M, C and I clear: MMU and caches off at entry
  //
  // Unlike x86's 64-bit entry, this entry needs no GDT or page tables from the VMM.
  // Linux builds its own tables, installs its exception vectors, then unmasks interrupts.
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800));
  HV(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5));
  HV(hv_vcpu_set_reg(cpu, HV_REG_PC, entry)); HV(hv_vcpu_set_reg(cpu, HV_REG_X0, DTB_BASE));
  for (hv_reg_t reg = HV_REG_X1; reg <= HV_REG_X3; reg++) HV(hv_vcpu_set_reg(cpu, reg, 0));

  // ------------------------------------------------------------------------------------------
  // 8. Run. Device MMIO, OS debug locks and PSCI calls return to this loop.
  //
  //   exception EC   guest action                        vmmlite
  //   0x24           load/store UART or virtio MMIO       mmio(): device access, PC += 4
  //   0x18           OS debug-lock system register       absent locks, PC += 4
  //   0x16           HVC, the PSCI firmware interface     answer in x0; PC already advanced
  //   other          unsupported exception               report ESR/address and stop
  //
  // Linux reboot -> PSCI SYSTEM_RESET -> leave loop -> destroy VM and exit 0.
  for (;;) {
    HV(hv_vcpu_run(cpu));
    require(vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION, "unexpected hypervisor exit");
    unsigned ec = (vcpu_exit->exception.syndrome >> 26) & 63;
    if (ec == 0x24) mmio(cpu, vcpu_exit);
    else if (ec == 0x18) {
      // Debug registers configure breakpoints (code addresses) and watchpoints
      // (memory accesses). OS locks protect this state during save/restore;
      // OSDLR adds protection around CPU power-down.
      //
      // Linux CPU startup, even without a debugger:
      //   unlock controls -> reset unused breakpoints/watchpoints -> ready
      //   Unlocking permits setup; it does not activate a breakpoint.
      //
      //   Linux access                 vmmlite: no guest locks
      //   OSLAR/OSDLR write ---------> ignore
      //   OSLSR read ----------------> return 0
      //
      // After either: PC += 4 -> resume Linux, without repeating the instruction.
      uint64_t syndrome = vcpu_exit->exception.syndrome, pc;
      unsigned access = syndrome & 0x3ffc1f, rt = (syndrome >> 5) & 31;
      require(access == 0x280400 || access == 0x280406 || access == 0x280403,
              "unhandled system register");
      if ((access & 1) && rt != 31) HV(hv_vcpu_set_reg(cpu, (hv_reg_t)(HV_REG_X0 + rt), 0));
      HV(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc)); HV(hv_vcpu_set_reg(cpu, HV_REG_PC, pc + 4));
    }
    else if (ec == 0x16) {
      // HVC calls firmware services; it does not enable a guest hypervisor.
      //
      //   Linux: x0 = PSCI request --HVC--> vmmlite
      //                                     |-- OFF/RESET -----> stop this VM run
      //                                     |-- VERSION -------> x0 = 2  (v0.2)
      //                                     |-- AFFINITY_INFO -> x0 = 0  (CPU on)
      //                                     `-- other ---------> x0 = -1 (unsupported)
      //
      // Reply paths resume Linux. The framework already advanced PC past HVC.
      uint64_t function, result = UINT64_MAX;
      HV(hv_vcpu_get_reg(cpu, HV_REG_X0, &function));
      if (function == 0x84000008 || function == 0x84000009) break; // PSCI SYSTEM_OFF/RESET
      if (function == 0x84000000) result = 2; // PSCI_VERSION: 0.2
      if (function == 0x84000006 || function == 0xc4000006) result = 0; // AFFINITY_INFO: on
      HV(hv_vcpu_set_reg(cpu, HV_REG_X0, result));
    } else {
      fprintf(stderr, "unexpected exception: ESR=%#" PRIx64 " IPA=%#" PRIx64 "\n",
              vcpu_exit->exception.syndrome, vcpu_exit->exception.physical_address);
      return 1;
    }
  }
  HV(hv_vcpu_destroy(cpu)); HV(hv_vm_destroy());
  require(!munmap(ram, RAM_SIZE), "unmapping RAM failed");
  fputs("vmmlite: guest requested PSCI shutdown/reset\n", stderr);
  return 0;
}
