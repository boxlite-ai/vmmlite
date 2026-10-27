// vmmlite.c: a tiny VMM that boots Linux (arm64, Hypervisor.framework). For learning only.
// Build: make    Run: .build/vmmlite Image .build/initramfs.cpio .build/machine.dtb
#include <Hypervisor/Hypervisor.h>
#include <libfdt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAM_BASE UINT64_C(0x40000000)
#define RAM_SIZE (512UL << 20)
#define DTB_BASE UINT64_C(0x48000000)
#define DTB_SIZE (2UL << 20)
#define INITRD_BASE UINT64_C(0x50000000)
#define UART_BASE UINT64_C(0x09000000)
#define CONSOLE_BASE UINT64_C(0x0a000000)
#define GICD_BASE UINT64_C(0x08000000)
#define GICR_BASE UINT64_C(0x10000000)
#define UART_IRQ 32
#define CONSOLE_IRQ 33
#define QUEUE_MAX 256
enum { UART_TX = 0, UART_IER = 1, UART_IIR = 2, UART_LCR = 3, UART_LSR = 5,
       UART_IER_THRI = 2, UART_IIR_NO_INT = 1, UART_IIR_THRI = 2,
       UART_LCR_DLAB = 128, UART_LSR_TEMT = 64, UART_LSR_THRE = 32 };
enum { MAGIC_VALUE = 0x000, VERSION = 0x004, DEVICE_ID = 0x008,
       DEVICE_FEATURES = 0x010, DEVICE_FEATURES_SEL = 0x014,
       DRIVER_FEATURES = 0x020, DRIVER_FEATURES_SEL = 0x024,
       QUEUE_SEL = 0x030, QUEUE_NUM_MAX = 0x034, QUEUE_NUM = 0x038, QUEUE_READY = 0x044,
       QUEUE_NOTIFY = 0x050, INTERRUPT_STATUS = 0x060, INTERRUPT_ACK = 0x064,
       STATUS = 0x070, QUEUE_DESC_LOW = 0x080, QUEUE_AVAIL_LOW = 0x090, QUEUE_USED_LOW = 0x0a0 };

static unsigned char *ram;
static void require(int ok, const char *why) {
  if (!ok) { fprintf(stderr, "vmmlite: %s\n", why); exit(1); }
}
#define HV(call) do { hv_return_t r = (call); if (r != HV_SUCCESS) { \
  fprintf(stderr, "%s: %#x\n", #call, r); exit(1); } } while (0)
#define FDT(call) do { int r = (call); if (r < 0) { \
  fprintf(stderr, "%s: %s\n", #call, fdt_strerror(r)); exit(1); } } while (0)

static void *guest(uint64_t address, size_t size) {
  require(address >= RAM_BASE && size <= RAM_SIZE &&
          address - RAM_BASE <= RAM_SIZE - size, "guest address outside RAM");
  return ram + (address - RAM_BASE);
}

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

// Linux Image header: little-endian text_offset, image_size, flags, ARM64 magic.
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

// The same transmit-only 8250 register subset as the KVM example, reached through MMIO.
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

// A modern virtio-mmio console. Only queue 1, output, needs backing state.
struct queue { uint32_t size, ready; uint64_t desc, avail, used; uint16_t next; };
static struct {
  struct queue transmit;
  uint32_t selected, feature_page, driver_page, status, interrupt;
  uint64_t features;
} console;
struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };

static void queue_check(struct queue *q) {
  require(q->size && q->size <= QUEUE_MAX && !(q->size & (q->size - 1)), "invalid queue size");
  require(!(q->desc & 15) && !(q->avail & 1) && !(q->used & 3), "unaligned queue");
  guest(q->desc, 16 * q->size); guest(q->avail, 6 + 2 * q->size); guest(q->used, 6 + 8 * q->size);
}

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

// Patch the readable DTS with this run's initramfs, GIC sizes and timer interrupt IDs.
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

// An MMIO data abort describes the faulting load/store in ESR_EL2's syndrome.
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
  // 1. A VM and its GIC; Apple requires both before creating any vCPU.
  HV(hv_vm_create(NULL));
  hv_gic_config_t gic = hv_gic_config_create();
  require(gic != NULL, "allocating GIC configuration failed");
  HV(hv_gic_config_set_distributor_base(gic, GICD_BASE));
  HV(hv_gic_config_set_redistributor_base(gic, GICR_BASE));
  HV(hv_gic_create(gic)); os_release(gic);

  // 2. 512 MiB of host memory as guest RAM. Device addresses have no RAM backing.
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  require(ram != MAP_FAILED, "allocating RAM failed");
  HV(hv_vm_map(ram, RAM_BASE, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

  // 3. The uncompressed Linux Image, at the offset its header requests.
  uint64_t entry = kernel_entry(load(argv[1], ram, DTB_BASE - RAM_BASE));

  // 4. The initramfs and device tree; /chosen in the tree carries the command line.
  size_t initrd_size = load(argv[2], guest(INITRD_BASE, 1), RAM_BASE + RAM_SIZE - INITRD_BASE);
  void *dtb = guest(DTB_BASE, DTB_SIZE);
  size_t dtb_size = load(argv[3], dtb, DTB_SIZE);

  // 5. Finish the machine description: initramfs extent, GIC regions and timer PPIs.
  prepare_dtb(dtb, dtb_size, initrd_size);

  // 6. One vCPU, affinity 0: the same CPU that /cpus/cpu@0 describes.
  hv_vcpu_t cpu; hv_vcpu_exit_t *vcpu_exit;
  HV(hv_vcpu_create(&cpu, &vcpu_exit, NULL));
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MPIDR_EL1, UINT64_C(0x80000000)));

  // 7. The ARM64 boot protocol: EL1h, DAIF masked, MMU off, x0 = DTB, x1-x3 = 0.
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800));
  HV(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5));
  HV(hv_vcpu_set_reg(cpu, HV_REG_PC, entry)); HV(hv_vcpu_set_reg(cpu, HV_REG_X0, DTB_BASE));
  for (hv_reg_t reg = HV_REG_X1; reg <= HV_REG_X3; reg++) HV(hv_vcpu_set_reg(cpu, reg, 0));

  // 8. Run. Device MMIO, OS debug locks and PSCI calls return to this loop.
  for (;;) {
    HV(hv_vcpu_run(cpu));
    require(vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION, "unexpected hypervisor exit");
    unsigned ec = (vcpu_exit->exception.syndrome >> 26) & 63;
    if (ec == 0x24) mmio(cpu, vcpu_exit);
    else if (ec == 0x18) {
      // No OS debug locks: OSLAR/OSDLR writes are ignored; OSLSR reads zero.
      uint64_t syndrome = vcpu_exit->exception.syndrome, pc;
      unsigned access = syndrome & 0x3ffc1f, rt = (syndrome >> 5) & 31;
      require(access == 0x280400 || access == 0x280406 || access == 0x280403,
              "unhandled system register");
      if ((access & 1) && rt != 31) HV(hv_vcpu_set_reg(cpu, (hv_reg_t)(HV_REG_X0 + rt), 0));
      HV(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc)); HV(hv_vcpu_set_reg(cpu, HV_REG_PC, pc + 4));
    }
    else if (ec == 0x16) {                 // HVC: the framework already advanced PC.
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
