// One ARM64 Linux CPU on Hypervisor.framework. For learning, not untrusted guests.
// The numbered sections match README.md. All guest addresses below are physical.
#include <Hypervisor/Hypervisor.h>
#include <libfdt.h>
#include <errno.h>
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
#define CONSOLE_IRQ 33
#define QUEUE_MAX 256

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

// 1. Linux Image header: little-endian text_offset, image_size, flags, ARM64 magic.
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

// 2. A modern virtio-mmio console. Queue 0 receives; queue 1 transmits.
struct queue { uint32_t size, ready; uint64_t desc, avail, used; uint16_t next; };
static struct {
  struct queue queues[2];
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
  struct queue *q = &console.queues[1];
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

static uint32_t console_read(unsigned reg) {
  struct queue *q = console.selected < 2 ? &console.queues[console.selected] : NULL;
  switch (reg) {
  case 0x000: return 0x74726976;           // MagicValue: "virt"
  case 0x004: return 2;                    // Version: modern MMIO
  case 0x008: return 3;                    // DeviceID: console
  case 0x010: return console.feature_page == 1 ? 1 : 0; // VIRTIO_F_VERSION_1, bit 32
  case 0x034: return q ? QUEUE_MAX : 0;
  case 0x044: return q ? q->ready : 0;
  case 0x060: return console.interrupt;
  case 0x070: return console.status;
  default: return 0;
  }
}

static void console_write(unsigned reg, uint32_t value) {
  struct queue *q = console.selected < 2 ? &console.queues[console.selected] : NULL;
  switch (reg) {
  case 0x014: console.feature_page = value; break;
  case 0x024: console.driver_page = value; break;
  case 0x020:
    if (console.driver_page < 2) {
      unsigned shift = 32 * console.driver_page;
      console.features = (console.features & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)value << shift);
    }
    break;
  case 0x030: console.selected = value; break;
  case 0x050: console_kick(value); break;
  case 0x064:
    console.interrupt &= ~value;
    HV(hv_gic_set_spi(CONSOLE_IRQ, console.interrupt != 0)); break;
  case 0x070:
    if (!value) { memset(&console, 0, sizeof console); HV(hv_gic_set_spi(CONSOLE_IRQ, false)); }
    else {
      if ((value & 8) && console.features != (UINT64_C(1) << 32)) value &= ~8u;
      require(!(value & 4) || (value & 8), "DRIVER_OK without accepted features");
      console.status = value;
    }
    break;
  case 0x038: if (q && !q->ready) q->size = value; break;
  case 0x044:
    if (q) { require(value <= 1, "invalid QueueReady"); if (value) queue_check(q); q->ready = value; }
    break;
  default:
    if (q && !q->ready && reg >= 0x080 && reg <= 0x0a4 && !(reg & 3)) {
      uint64_t *address = reg < 0x090 ? &q->desc : reg < 0x0a0 ? &q->avail : &q->used;
      if ((reg & 15) == 0) *address = (*address & UINT64_C(0xffffffff00000000)) | value;
      if ((reg & 15) == 4) *address = (*address & UINT64_C(0xffffffff)) | ((uint64_t)value << 32);
    }
  }
}

// 3. Patch variable properties in the readable DTS, using libfdt for bounds/layout.
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

// 4. An MMIO data abort describes the faulting load/store in ESR_EL2's syndrome.
static void mmio(hv_vcpu_t cpu, hv_vcpu_exit_t *vcpu_exit) {
  uint64_t esr = vcpu_exit->exception.syndrome, address = vcpu_exit->exception.physical_address;
  unsigned width = 1u << ((esr >> 22) & 3), rt = (esr >> 16) & 31;
  int write = (esr >> 6) & 1;
  require((esr & (1u << 24)) && !(esr & ((1u << 7) | (1u << 8))), "unsupported MMIO syndrome");
  uint64_t value = 0, pc;
  if (write && rt != 31) HV(hv_vcpu_get_reg(cpu, (hv_reg_t)(HV_REG_X0 + rt), &value));
  if (address >= UART_BASE && address < UART_BASE + 8) {
    unsigned reg = address - UART_BASE;
    require(width == 1, "UART needs byte accesses");
    if (write && reg == 0) require(putchar(value & 255) != EOF, "UART write failed");
    value = reg == 5 ? 0x60 : 0;           // 16550 LSR: transmitter and holding register empty.
  } else {
    require(address >= CONSOLE_BASE && address < CONSOLE_BASE + 0x1000 &&
            width == 4 && !(address & 3), "unhandled MMIO address/width");
    unsigned reg = address - CONSOLE_BASE;
    if (write) console_write(reg, value); else value = console_read(reg);
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
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  require(ram != MAP_FAILED, "allocating RAM failed");
  uint64_t entry = kernel_entry(load(argv[1], ram, DTB_BASE - RAM_BASE));
  size_t initrd_size = load(argv[2], guest(INITRD_BASE, 1), RAM_BASE + RAM_SIZE - INITRD_BASE);
  void *dtb = guest(DTB_BASE, DTB_SIZE);
  size_t dtb_size = load(argv[3], dtb, DTB_SIZE);

  // 5. Create the VM and GIC before the vCPU; MMIO regions deliberately have no RAM backing.
  HV(hv_vm_create(NULL));
  HV(hv_vm_map(ram, RAM_BASE, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
  hv_gic_config_t gic = hv_gic_config_create();
  require(gic != NULL, "allocating GIC configuration failed");
  HV(hv_gic_config_set_distributor_base(gic, GICD_BASE));
  HV(hv_gic_config_set_redistributor_base(gic, GICR_BASE));
  HV(hv_gic_create(gic)); os_release(gic);
  prepare_dtb(dtb, dtb_size, initrd_size);

  // 6. The ARM64 boot protocol: EL1h, DAIF masked, MMU off, x0 = DTB, x1-x3 = 0.
  hv_vcpu_t cpu; hv_vcpu_exit_t *vcpu_exit;
  HV(hv_vcpu_create(&cpu, &vcpu_exit, NULL));
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MPIDR_EL1, UINT64_C(0x80000000)));
  HV(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800));
  HV(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5));
  HV(hv_vcpu_set_reg(cpu, HV_REG_PC, entry)); HV(hv_vcpu_set_reg(cpu, HV_REG_X0, DTB_BASE));
  for (hv_reg_t reg = HV_REG_X1; reg <= HV_REG_X3; reg++) HV(hv_vcpu_set_reg(cpu, reg, 0));

  // 7. Native GIC/timers stay in the framework. MMIO and PSCI reach this loop.
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
