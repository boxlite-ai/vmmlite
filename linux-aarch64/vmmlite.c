// vmmlite.c: a tiny VMM that boots Linux (AArch64, KVM). For learning only.
// From the repo root: make build:linux-aarch64    Run: make run:linux-aarch64
//
//   vmmlite (this process)       KVM (host kernel)                ARM64 CPU
//   +--------------------+ ioctl +-----------------------+ entry +--------------------+
//   | ram: guest RAM     |------>| VM: memory slots,     |------>| runs guest Linux   |
//   | run: exit record   |<------| GICv3, timers; vCPU    |<------| until a VM exit    |
//   +--------------------+ exits +-----------------------+ exit  +--------------------+
//
// Read main() for the nine boot steps. Helpers explain memory/loading, UART registers,
// the virtio output queue, eventfds, and the device tree in that order.
// Same print-and-reboot endpoint as macOS/x86: one CPU, 512 MiB, no input/disk/network.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <inttypes.h>
#include <libfdt.h>
#include <linux/kvm.h>
#include <linux/serial_reg.h>
#include <linux/virtio_config.h> // status and feature bits, as in the Linux x86 example
#include <linux/virtio_ids.h>
#include <linux/virtio_mmio.h>   // VIRTIO_MMIO_* register offsets
#include <linux/virtio_ring.h>   // descriptor flags: NEXT, WRITE, INDIRECT
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Build on little-endian AArch64 Linux"
#endif

// Guest physical addresses: RAM is backed by mmap; device windows are not RAM.
// KVM handles GIC accesses internally. UART and virtio control accesses return to mmio().
#define RAM_BASE UINT64_C(0x40000000)     // 1 GiB, aligned to the Image's 2 MiB requirement
#define RAM_SIZE (512UL << 20)           // RAM ends at 0x60000000, exclusive
#define DTB_BASE UINT64_C(0x48000000)     // x0 points here at kernel entry
#define DTB_SIZE (2UL << 20)             // libfdt workspace, not the packed blob size
#define INITRD_BASE UINT64_C(0x50000000)  // cpio archive with the shared /init
#define UART_BASE UINT64_C(0x09000000)    // eight byte-wide 8250 registers
#define GICD_BASE UINT64_C(0x08000000)    // distributor: shared interrupts, 64 KiB
#define GICR_BASE UINT64_C(0x10000000)    // one redistributor: private interrupts, 128 KiB
#define UART_IRQ 32                     // architectural INTID 32 = DT SPI 0
#define CONSOLE_BASE UINT64_C(0x0a000000) // 4 KiB virtio-MMIO window
#define CONSOLE_GSI 1 // ARM irqfd route pin 1 means GIC INTID 33.
#define QUEUE_MAX 256
#define CORE(name) (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(name))

static unsigned char *ram; // host pointer; guest physical addresses need guest() translation
static int vm;            // VM fd; com1() uses it to drive the UART's interrupt line
static void require(int ok, const char *why) {
  if (!ok) { fprintf(stderr, "vmmlite: %s\n", why); exit(1); }
}
static int checked_ioctl(int fd, unsigned long request, uintptr_t arg, const char *name) {
  int result = ioctl(fd, request, arg);
  if (result < 0) { perror(name); exit(1); }
  return result;
}
#define IOCTL(fd, request, arg) checked_ioctl(fd, request, (uintptr_t)(arg), #request)
#define FDT(call) do { int r = (call); if (r < 0) { \
  fprintf(stderr, "%s: %s\n", #call, fdt_strerror(r)); exit(1); } } while (0)

// Memory and loading ----------------------------------------------------------------
// Guest physical address -> offset in RAM -> host pointer:
//   0x40001000           -> 0x1000        -> ram + 0x1000
// Subtract before comparing: address + size could wrap near UINT64_MAX.
static void *guest(uint64_t address, size_t size) {
  require(address >= RAM_BASE && size <= RAM_SIZE &&
          address - RAM_BASE <= RAM_SIZE - size, "guest address outside RAM");
  return ram + (address - RAM_BASE);
}

// Load a whole file into its assigned region; capacity keeps it out of the next one.
// Fatal errors end this one-shot process; there is no reusable VM lifecycle API.
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

// Raw ARM64 Image header: the input is already uncompressed, unlike x86's bzImage.
//
//   offset  field        meaning (header fields are little endian)
//    0      code0/code1  executable instructions; PC starts at the Image's first byte
//    8      text_offset  distance from a 2 MiB-aligned RAM base
//   16      image_size   reserved footprint, including bytes not stored in the file
//   24      flags        bit 0 selects big endian, unsupported here
//   56      magic        41 52 4d 64: "ARM" followed by byte 0x64
//
// file -> ram (temporary) -> memmove to ram + text_offset (final)
// Reserve the whole image_size before the DTB. The pinned Image uses text_offset=0;
// do not assume the often-seen 0x80000 offset. Linux creates its own page tables.
static uint64_t kernel_entry(size_t file_size) {
  uint64_t offset, image_size, flags;
  require(file_size >= 64 && !memcmp(ram + 56, "ARM\x64", 4), "expected an uncompressed ARM64 Image");
  memcpy(&offset, ram + 8, 8);
  memcpy(&image_size, ram + 16, 8);
  memcpy(&flags, ram + 24, 8);
  require(image_size && !(flags & 1), "need a modern little-endian Image");
  require(offset < DTB_BASE - RAM_BASE && image_size >= file_size &&
          image_size <= DTB_BASE - RAM_BASE - offset, "kernel overlaps device tree");
  memmove(ram + offset, ram, file_size);
  return RAM_BASE + offset;
}

// UART ------------------------------------------------------------------------------
// The 8250 subset used by earlycon and Linux's OF serial driver (ttyS0).
//
//   offset  register    guest reads                         guest writes
//   0       RX / TX     zero: no input                      print byte; mark TX empty
//   1       IER         saved interrupt-enable bits         save IER; THRI enables TX IRQ
//   2       IIR / FCR   pending THRI, else NO_INT            ignored: no FIFO
//   3       LCR         saved line control                  save LCR, including DLAB
//   5       LSR         TEMT | THRE: transmitter empty     ignored
//
// DLAB=1 selects baud-divisor bytes at offsets 0/1; those writes must not print.
// TX-empty event + IER.THRI -> INTID 32 high -> read IIR -> clear event and lower line.
// KVM_IRQ_LINE takes the ARM SPI encoding plus INTID 32, unlike irqfd's routing pin.
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
  struct kvm_irq_level irq = {.irq = (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT) | UART_IRQ,
                              .level = !!(ier & UART_IER_THRI && thr_empty)};
  IOCTL(vm, KVM_IRQ_LINE, &irq);
  return reply;
}

static int kick_fd, irq_fd, stop_fd;
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
static void event_signal(int fd) {
  int result;
  do { result = eventfd_write(fd, 1); } while (result < 0 && errno == EINTR);
  require(!result, "eventfd write failed");
}

// Virtio console --------------------------------------------------------------------
// Modern virtio-mmio console: queue 0 (input) is advertised but deliberately unused.
// Queue 1 (output) owns three guest-memory areas: descriptors, available ring, used ring.
// Control writes and the worker share console_lock. The mutex does not stop the guest:
// acquire/release ordering separately implements the virtqueue publication protocol.
// Each address below is a guest physical address, not a host pointer.
struct queue {
  uint32_t size, ready;
  uint64_t desc, avail, used;
  uint16_t next; // next available entry to consume; also our used-ring producer index
};
static struct {
  struct queue transmit;
  uint32_t selected, feature_page, driver_page, status, interrupt;
  uint64_t features;
} console;
// One 16-byte descriptor, little endian like this host and guest. Only NEXT is accepted:
// WRITE would let the device write guest memory; INDIRECT would require another table.
struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };

//   area   bytes      alignment  contents
//   desc   16*N       16         {address, length, flags, next} for each buffer
//   avail  6+2*N       2         flags, idx, N head IDs, optional used_event
//   used   6+8*N       4         flags, idx, N {id,length} entries, optional avail_event
// VERSION_1 is the only feature; the trailing EVENT_IDX fields are not used.
static void queue_check(struct queue *queue) {
  require(queue->size && queue->size <= QUEUE_MAX && !(queue->size & (queue->size - 1)), "invalid queue size");
  require(!(queue->desc & 15) && !(queue->avail & 1) && !(queue->used & 3), "unaligned queue");
  guest(queue->desc, 16 * queue->size);
  guest(queue->avail, 6 + 2 * queue->size);
  guest(queue->used, 6 + 8 * queue->size);
}

// Linux: text -> desc[head] -> avail.ring[slot] -> avail.idx -> QueueNotify=1
// Worker: acquire avail.idx -> copy/validate chain -> fwrite(text) -> used.ring[slot]
//         -> release used.idx -> signal irqfd -> GIC INTID 33
// Completion length is zero: output reads guest bytes, writes none into those buffers.
// queue->next is our own 16-bit counter; the guest cannot choose our used-ring write slot.
// The eventfd counter counts notifications, not descriptors. One kick may cover many.
static void console_kick(uint32_t queue_number) {
  if (queue_number != 1) return;
  struct queue *queue = &console.transmit;
  // A delayed eventfd kick can follow reset; do not revisit the old queue addresses.
  if (!queue->ready || !(console.status & VIRTIO_CONFIG_S_DRIVER_OK)) return;
  queue_check(queue);
  uint16_t *avail = guest(queue->avail, 6 + 2 * queue->size);
  uint16_t *used = guest(queue->used, 6 + 8 * queue->size);
  uint16_t available_end = __atomic_load_n(&avail[1], __ATOMIC_ACQUIRE);
  if (available_end == queue->next) return;
  require((uint16_t)(available_end - queue->next) <= queue->size, "available ring overrun");
  while (queue->next != available_end) {
    uint16_t head = avail[2 + queue->next % queue->size], index = head;
    for (unsigned walked = 0;; walked++) {
      require(index < queue->size && walked < queue->size, "invalid or cyclic descriptor chain");
      struct descriptor descriptor;
      memcpy(&descriptor, guest(queue->desc + 16 * index, sizeof descriptor), sizeof descriptor);
      require(!(descriptor.flags & ~(unsigned)VRING_DESC_F_NEXT), "transmit descriptor must be readable and direct");
      require(fwrite(guest(descriptor.address, descriptor.length), 1, descriptor.length, stdout) == descriptor.length, "console write failed");
      if (!(descriptor.flags & VRING_DESC_F_NEXT)) break;
      index = descriptor.next;
    }
    uint32_t completion[] = {head, 0};
    memcpy((char *)used + 4 + 8 * (queue->next % queue->size), completion, sizeof completion);
    queue->next++;
    __atomic_store_n(&used[1], queue->next, __ATOMIC_RELEASE);
  }
  require(!fflush(stdout), "console flush failed");
  console.interrupt |= VIRTIO_MMIO_INT_VRING;
  event_signal(irq_fd);
}

// Register access, serialized with the worker by mmio()'s console_lock.
// Offsets/names come from linux/virtio_mmio.h (Virtio 1.2 section 4.2.2):
//
//   offset      register suffix       role
//   000/004/008 MAGIC/VERSION/ID      identify virtio-MMIO v2, console device 3
//   010/014     DEVICE_FEATURES/SEL   offer VERSION_1, bit 0 of feature page 1
//   020/024     DRIVER_FEATURES/SEL   save the features accepted by Linux
//   030/034/038 QUEUE_SEL/MAX/NUM     choose queue; maximum and configured length
//   044         QUEUE_READY          validate geometry before enabling output
//   050         QUEUE_NOTIFY         queue number; KVM intercepts value 1 via ioeventfd
//   060/064     INTERRUPT_STATUS/ACK report/clear used-buffer bit; separate from GIC EOI
//   070         STATUS               feature negotiation, DRIVER_OK, or reset to zero
//   080/090/0a0 QUEUE_*_LOW          64-bit addresses split into low/high words (+4)
//
// Unknown reads return zero. Queue 0 stays advertised but unserved, as in the siblings.
// FEATURES_OK is accepted only when VERSION_1 (feature bit 32) was negotiated.
// Queue addresses stay fixed while ready. STATUS=0 resets all device/queue state.
static uint32_t console_register_read(unsigned reg) {
  struct queue *queue = console.selected == 1 ? &console.transmit : NULL;
  switch (reg) {
  case VIRTIO_MMIO_MAGIC_VALUE: return 0x74726976;
  case VIRTIO_MMIO_VERSION: return 2;
  case VIRTIO_MMIO_DEVICE_ID: return VIRTIO_ID_CONSOLE;
  case VIRTIO_MMIO_DEVICE_FEATURES: return console.feature_page == 1 ? 1 : 0;
  case VIRTIO_MMIO_QUEUE_NUM_MAX: return console.selected < 2 ? QUEUE_MAX : 0;
  case VIRTIO_MMIO_QUEUE_READY: return queue ? queue->ready : 0;
  case VIRTIO_MMIO_INTERRUPT_STATUS: return console.interrupt;
  case VIRTIO_MMIO_STATUS: return console.status;
  default: return 0;
  }
}

static void console_register_write(unsigned reg, uint32_t value) {
  struct queue *queue = console.selected == 1 ? &console.transmit : NULL;
  switch (reg) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: console.feature_page = value; break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL: console.driver_page = value; break;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    if (console.driver_page < 2) {
      unsigned shift = 32 * console.driver_page;
      console.features = (console.features & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)value << shift);
    }
    break;
  case VIRTIO_MMIO_QUEUE_SEL: console.selected = value; break;
  case VIRTIO_MMIO_QUEUE_NOTIFY: console_kick(value); break;
  case VIRTIO_MMIO_INTERRUPT_ACK:
    console.interrupt &= ~value;
    break;
  case VIRTIO_MMIO_STATUS:
    if (!value) memset(&console, 0, sizeof console);
    else {
      if ((value & VIRTIO_CONFIG_S_FEATURES_OK) &&
          console.features != (UINT64_C(1) << VIRTIO_F_VERSION_1))
        value &= ~(unsigned)VIRTIO_CONFIG_S_FEATURES_OK;
      require(!(value & VIRTIO_CONFIG_S_DRIVER_OK) || (value & VIRTIO_CONFIG_S_FEATURES_OK),
              "DRIVER_OK without accepted features");
      console.status = value;
    }
    break;
  case VIRTIO_MMIO_QUEUE_NUM: if (queue && !queue->ready) queue->size = value; break;
  case VIRTIO_MMIO_QUEUE_READY:
    if (queue) {
      require(value <= 1, "invalid QueueReady");
      if (value) queue_check(queue);
      queue->ready = value;
    }
    break;
  default:
    if (queue && !queue->ready && reg >= VIRTIO_MMIO_QUEUE_DESC_LOW &&
        reg <= VIRTIO_MMIO_QUEUE_USED_LOW + 4 && !(reg & 3)) {
      uint64_t *address = reg < VIRTIO_MMIO_QUEUE_AVAIL_LOW ? &queue->desc :
                          reg < VIRTIO_MMIO_QUEUE_USED_LOW ? &queue->avail : &queue->used;
      if ((reg & 15) == 0) *address = (*address & UINT64_C(0xffffffff00000000)) | value;
      if ((reg & 15) == 4) *address = (*address & UINT64_C(0xffffffff)) | ((uint64_t)value << 32);
    }
  }
}


// Host notification path -------------------------------------------------------------
// KVM consumes matched doorbell writes without returning a KVM_EXIT_MMIO to main.
// This worker drains queue 1 while the guest may run on the vCPU thread.
// stop_fd lets normal shutdown join the worker before RAM or descriptors disappear.
static void *console_thread(void *unused) {
  (void)unused;
  struct pollfd events[] = {{kick_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
  for (;;) {
    int result = poll(events, 2, -1);
    if (result < 0 && errno == EINTR) continue;
    require(result > 0 && !((events[0].revents | events[1].revents) & (POLLERR | POLLHUP | POLLNVAL)), "console poll failed");
    if (events[1].revents & POLLIN) return NULL;
    eventfd_t count;
    do { result = eventfd_read(kick_fd, &count); } while (result < 0 && errno == EINTR);
    require(!result, "eventfd read failed");
    require(!pthread_mutex_lock(&console_lock), "console lock failed");
    console_kick(1);
    require(!pthread_mutex_unlock(&console_lock), "console unlock failed");
  }
}

// Guest write 1 at 0x0a000050 -> ioeventfd -> worker -> used ring -> irqfd -> GIC.
// ARM irqchip routing pin N targets INTID N+32. This is SPI 1 / INTID 33.
//   machine.dts: <0 1 1>   SPI, relative number 1, edge rising
//   route: GSI 1 -> pin 1 -> INTID 33; Linux assigns its own software IRQ number
// machine.dts uses edge rising: ordinary irqfd pulses, with no resampling protocol.
// The guest separately acknowledges console.interrupt and completes GIC handling.
static void connect_console(void) {
  kick_fd = eventfd(0, EFD_CLOEXEC);
  irq_fd = eventfd(0, EFD_CLOEXEC);
  stop_fd = eventfd(0, EFD_CLOEXEC);
  require(kick_fd >= 0 && irq_fd >= 0 && stop_fd >= 0, "creating eventfds failed");
  struct kvm_irq_routing *routes = calloc(1, sizeof *routes + sizeof routes->entries[0]);
  require(routes != NULL, "allocating IRQ route failed");
  routes->nr = 1;
  routes->entries[0] = (struct kvm_irq_routing_entry){.gsi = CONSOLE_GSI, .type = KVM_IRQ_ROUTING_IRQCHIP,
      .u.irqchip = {.irqchip = 0, .pin = CONSOLE_GSI}};
  IOCTL(vm, KVM_SET_GSI_ROUTING, routes);
  free(routes);
  struct kvm_irqfd irq = {.fd = irq_fd, .gsi = CONSOLE_GSI};
  IOCTL(vm, KVM_IRQFD, &irq);
  struct kvm_ioeventfd kick = {.datamatch = 1, .addr = CONSOLE_BASE + VIRTIO_MMIO_QUEUE_NOTIFY, .len = 4,
                              .fd = kick_fd, .flags = KVM_IOEVENTFD_FLAG_DATAMATCH};
  IOCTL(vm, KVM_IOEVENTFD, &kick);
}

static void attribute(int fd, unsigned long operation, uint32_t group, uint64_t attr, void *value) {
  struct kvm_device_attr a = {.group = group, .attr = attr, .addr = (uintptr_t)value};
  IOCTL(fd, operation, &a);
}

// Machine description ----------------------------------------------------------------
// dtc supplies structure; libfdt writes big-endian cells into that compiled template.
// x0 names the DTB. /chosen carries bootargs and exclusive initramfs bounds.
// Query CPU affinity and timer PPIs instead of assuming host allocation/CPU topology.
//
//   /chosen       initrd-start / initrd-end     start included, end excluded
//   /cpus/cpu@0   reg                          MPIDR affinity bits, excluding flags
//   /timer        interrupts                   <1, INTID-16, 4>: PPI, relative ID, level
//
// Timer tuples stay in binding order: secure physical, nonsecure physical, virtual,
// hypervisor. Query the two EL1 timers used by this guest; keep the other template IDs.
static void prepare_dtb(void *dtb, size_t dtb_size, size_t initrd_size, int cpu) {
  FDT(fdt_check_full(dtb, dtb_size));
  FDT(fdt_open_into(dtb, dtb, DTB_SIZE));
  int chosen = fdt_path_offset(dtb, "/chosen");
  FDT(fdt_setprop_u64(dtb, chosen, "linux,initrd-start", INITRD_BASE));
  FDT(fdt_setprop_u64(dtb, chosen, "linux,initrd-end", INITRD_BASE + initrd_size));
  uint64_t mpidr;
  struct kvm_one_reg r = {.id = ARM64_SYS_REG(3, 0, 0, 0, 5), .addr = (uintptr_t)&mpidr};
  IOCTL(cpu, KVM_GET_ONE_REG, &r);
  FDT(fdt_setprop_u64(dtb, fdt_path_offset(dtb, "/cpus/cpu@0"), "reg", mpidr & UINT64_C(0xff00ffffff)));
  uint32_t physical, virtual;
  attribute(cpu, KVM_GET_DEVICE_ATTR, KVM_ARM_VCPU_TIMER_CTRL, KVM_ARM_VCPU_TIMER_IRQ_PTIMER, &physical);
  attribute(cpu, KVM_GET_DEVICE_ATTR, KVM_ARM_VCPU_TIMER_CTRL, KVM_ARM_VCPU_TIMER_IRQ_VTIMER, &virtual);
  require(physical >= 16 && physical < 32 && virtual >= 16 && virtual < 32, "timer IRQ is not a PPI");
  fdt32_t timers[] = {1, 13, 4, 1, physical - 16, 4, 1, virtual - 16, 4, 1, 10, 4};
  for (unsigned i = 0; i < sizeof timers / sizeof timers[0]; i++) timers[i] = cpu_to_fdt32(timers[i]);
  FDT(fdt_setprop(dtb, fdt_path_offset(dtb, "/timer"), "interrupts", timers, sizeof timers));
  FDT(fdt_pack(dtb));
}

// KVM has decoded the trapped instruction into address, width, direction and data.
// For reads, supply data before the next KVM_RUN. KVM completes the instruction;
// unlike the macOS exception path, userspace does not decode ESR or advance PC here.
static void mmio(struct kvm_run *run) {
  uint64_t address = run->mmio.phys_addr;
  if (address >= UART_BASE && address < UART_BASE + 8) {
    require(run->mmio.len == 1, "UART needs byte accesses");
    run->mmio.data[0] = com1(address - UART_BASE, run->mmio.is_write, run->mmio.data[0]);
    return;
  }
  require(address >= CONSOLE_BASE && address < CONSOLE_BASE + 0x1000 &&
          run->mmio.len == 4 && !(address & 3), "unhandled MMIO address/width");
  uint32_t value;
  memcpy(&value, run->mmio.data, sizeof value);
  require(!pthread_mutex_lock(&console_lock), "console lock failed");
  if (run->mmio.is_write) console_register_write(address - CONSOLE_BASE, value);
  else {
    value = console_register_read(address - CONSOLE_BASE);
    memcpy(run->mmio.data, &value, sizeof value);
  }
  require(!pthread_mutex_unlock(&console_lock), "console unlock failed");
}

int main(int argc, char **argv) {
  require(argc == 4, "usage: vmmlite Image initramfs.cpio machine.dtb (AArch64 Linux with /dev/kvm)");
  setbuf(stdout, NULL);

  // --------------------------------------------------------------------------------
  // 1. Open KVM, check the ABI/capabilities, create the VM.
  // kvm is the system fd; vm owns this machine's memory slots, vCPU and GIC device.
  // PSCI supplies firmware calls; ioeventfd/irqfd supply the console notification path.
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) { perror("open /dev/kvm (need AArch64 KVM and read/write permission)"); return 1; }
  require(IOCTL(kvm, KVM_GET_API_VERSION, 0) == KVM_API_VERSION, "unsupported KVM API");
  require(IOCTL(kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2), "KVM lacks PSCI 0.2");
  require(IOCTL(kvm, KVM_CHECK_EXTENSION, KVM_CAP_IRQFD) &&
          IOCTL(kvm, KVM_CHECK_EXTENSION, KVM_CAP_IOEVENTFD), "KVM lacks irqfd/ioeventfd");
  vm = IOCTL(kvm, KVM_CREATE_VM, 0);

  // --------------------------------------------------------------------------------
  // 2. Back 512 MiB of guest physical memory with this process's pages.
  // mmap creates host memory. The memory slot tells KVM where it appears to Linux:
  //   guest [0x40000000, 0x60000000) -> host [ram, ram + RAM_SIZE)
  // Device addresses are outside this slot; they do not accidentally become RAM.
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(ram != MAP_FAILED, "mapping guest RAM failed");
  struct kvm_userspace_memory_region memory = {
      .slot = 0, .guest_phys_addr = RAM_BASE, .memory_size = RAM_SIZE, .userspace_addr = (uintptr_t)ram};
  IOCTL(vm, KVM_SET_USER_MEMORY_REGION, &memory);

  // --------------------------------------------------------------------------------
  // 3. Load the raw Image, root archive and device-tree template into separate regions.
  // Image: RAM_BASE + header.text_offset, reserve header.image_size before the DTB.
  // DTB: 0x48000000, at most 2 MiB. Initramfs: 0x50000000, up to RAM's exclusive end.
  uint64_t entry = kernel_entry(load(argv[1], ram, DTB_BASE - RAM_BASE));
  size_t initrd_size = load(argv[2], guest(INITRD_BASE, 0), RAM_BASE + RAM_SIZE - INITRD_BASE);
  void *dtb = guest(DTB_BASE, DTB_SIZE);
  size_t dtb_size = load(argv[3], dtb, DTB_SIZE);

  // --------------------------------------------------------------------------------
  // 4. Create one native-target vCPU and ask KVM to implement PSCI firmware calls.
  // The preferred target comes from KVM, not an invented CPU model. VCPU_INIT installs
  // the reset state, including the disabled guest MMU; step 6 sets our Linux entry state.
  int cpu = IOCTL(vm, KVM_CREATE_VCPU, 0);
  struct kvm_vcpu_init init = {0};
  IOCTL(vm, KVM_ARM_PREFERRED_TARGET, &init);
  init.features[0] |= 1u << KVM_ARM_VCPU_PSCI_0_2;
  IOCTL(cpu, KVM_ARM_VCPU_INIT, &init);

  // --------------------------------------------------------------------------------
  // 5. Create GICv3. Initialize only after vCPU creation; these addresses match the DTS.
  // 64 INTIDs = 16 SGIs + 16 PPIs + 32 SPIs. The distributor routes shared device IRQs;
  // one redistributor holds this CPU's private state. KVM supplies the CPU interface.
  // KVM's order differs from macOS, where framework GIC creation precedes the vCPU.
  struct kvm_create_device gic = {.type = KVM_DEV_TYPE_ARM_VGIC_V3};
  IOCTL(vm, KVM_CREATE_DEVICE, &gic);
  uint64_t distributor = GICD_BASE, redistributor = GICR_BASE;
  uint32_t nr_irqs = 64;
  attribute(gic.fd, KVM_SET_DEVICE_ATTR, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_DIST, &distributor);
  attribute(gic.fd, KVM_SET_DEVICE_ATTR, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_REDIST, &redistributor);
  attribute(gic.fd, KVM_SET_DEVICE_ATTR, KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0, &nr_irqs);
  attribute(gic.fd, KVM_SET_DEVICE_ATTR, KVM_DEV_ARM_VGIC_GRP_CTRL, KVM_DEV_ARM_VGIC_CTRL_INIT, NULL);

  // --------------------------------------------------------------------------------
  // 6. Finish the machine description and install the ARM64 boot register contract.
  //
  //   register     value               meaning
  //   PC           Image entry         first instruction, calculated in step 3
  //   x0           0x48000000          physical address of the packed DTB
  //   x1-x3        zero                reserved by Linux's ARM64 boot protocol
  //   PSTATE       0x3c5               DAIF masks set, EL1h (kernel uses SP_EL1)
  //
  // Linux creates page tables and exception vectors before enabling its MMU/IRQs.
  // KVM_SET_ONE_REG takes a register ID and a host pointer to the value, not the value.
  prepare_dtb(dtb, dtb_size, initrd_size, cpu);
  uint64_t ids[] = {CORE(regs.pc), CORE(regs.pstate), CORE(regs.regs[0]),
                    CORE(regs.regs[1]), CORE(regs.regs[2]), CORE(regs.regs[3])};
  uint64_t values[] = {entry, 0x3c5, DTB_BASE, 0, 0, 0}; // EL1h; D, A, I, F masked; MMU off after INIT.
  for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
    struct kvm_one_reg r = {.id = ids[i], .addr = (uintptr_t)&values[i]};
    IOCTL(cpu, KVM_SET_ONE_REG, &r);
  }

  // --------------------------------------------------------------------------------
  // 7. Map the shared exit record; connect eventfds and start the console worker.
  // The vCPU fd's mapping contains struct kvm_run, not guest RAM. KVM writes exits
  // here; userspace supplies MMIO read data before reentering. Ask KVM for its size.
  // Register the route/irqfd/doorbell before running Linux, so no kick is lost at boot.
  size_t run_size = IOCTL(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu, 0);
  require(run != MAP_FAILED, "mapping kvm_run failed");
  connect_console();
  pthread_t worker;
  require(!pthread_create(&worker, NULL, console_thread, NULL), "starting console worker failed");

  // --------------------------------------------------------------------------------
  // 8. Enter Linux. KVM handles timers/GIC/PSCI; userspace handles device MMIO.
  //
  //   exit                     action
  //   KVM_EXIT_MMIO            UART or virtio control register: mmio(), then reenter
  //   KVM_EXIT_SYSTEM_EVENT    PSCI SHUTDOWN/RESET: finish this one-shot VM
  //   other                    report an unsupported exit and terminate
  //
  // A queue-1 doorbell normally wakes the worker without returning here. KVM finishes
  // trapped load/store instructions; unlike the macOS loop, do not advance PC by hand.
  for (;;) {
    if (ioctl(cpu, KVM_RUN, 0) < 0) {
      if (errno == EINTR) continue;
      perror("KVM_RUN");
      return 1;
    }
    if (run->exit_reason == KVM_EXIT_MMIO) { mmio(run); continue; }
    if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT &&
        (run->system_event.type == KVM_SYSTEM_EVENT_SHUTDOWN || run->system_event.type == KVM_SYSTEM_EVENT_RESET)) break;
    fprintf(stderr, "unexpected KVM exit %u\n", run->exit_reason);
    return 1;
  }

  // --------------------------------------------------------------------------------
  // 9. PSCI reset/off ends this one-shot VM; stop the worker before releasing memory.
  // stop_fd wakes a worker blocked in poll(). join is the lifetime boundary: afterward
  // no worker can follow a queue pointer into RAM or signal an eventfd we are closing.
  event_signal(stop_fd);
  require(!pthread_join(worker, NULL), "joining console worker failed");
  fprintf(stderr, "guest requested PSCI shutdown/reset\n");
  munmap(run, run_size);
  close(cpu);
  close(gic.fd);
  close(vm);
  close(kvm);
  munmap(ram, RAM_SIZE);
  close(kick_fd);
  close(irq_fd);
  close(stop_fd);
  return 0;
}
