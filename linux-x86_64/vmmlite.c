// vmmlite.c: the smallest VMM that boots Linux (x86_64, KVM). For learning only.
// From the repo root: make build:linux-x86_64    Run: make run:linux-x86_64 KERNEL=/path/to/bzImage
//
//   vmmlite (this process)       KVM (host kernel)               CPU (VT-x / AMD-V)
//   +--------------------+ ioctl +-----------------------+ entry +--------------------+
//   | ram: guest RAM     |------>| VM: memslot, PIC,     |------>| runs guest code    |
//   | run: kvm_run page  |<------| IOAPIC, PIT; vCPU     |<------| until a VM exit    |
//   +--------------------+ exits +-----------------------+ exit  +--------------------+

#include <asm/bootparam.h>                               // struct boot_params: the zero page
#include <asm/processor-flags.h>                         // CR0, CR4 and RFLAGS bits
#include <fcntl.h>
#include <linux/kvm.h>                                   // KVM's ioctl numbers and structs
#include <linux/serial_reg.h>                            // the 16550 UART's registers and bits
#include <linux/virtio_config.h>                         // virtio feature bits
#include <linux/virtio_ids.h>                            // virtio device IDs
#include <linux/virtio_mmio.h>                           // virtio-mmio register offsets
#include <linux/virtio_ring.h>                           // the split virtqueue's descriptors, rings
#include <pthread.h>
#include <stddef.h>                                      // offsetof()
#include <stdint.h>                                      // fixed-width types, for the MP table
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>                                 // eventfd(): the console's kick, its IRQ
#include <sys/ioctl.h>                                   // ioctl(): how every KVM call is made
#include <sys/mman.h>                                    // mmap(): files, guest RAM, run page
#include <sys/stat.h>
#include <unistd.h>

#define RAM_SIZE             (512UL << 20)               // guest RAM: 512 MiB at address 0
#define PAGE_SIZE            0x1000UL                    // 4 KiB
// Where each thing goes, as guest physical addresses:
#define GDT_START            0x500                       // the boot GDT
#define ZERO_PAGE_START      0x7000                      // struct boot_params, the zero page
#define PML4_START           0x9000                      // the page tables, one page each:
#define PDPT_START           (PML4_START + PAGE_SIZE)    //   PML4, PDPT, PD
#define PD_START             (PDPT_START + PAGE_SIZE)
#define CMDLINE_START        0x20000                     // the kernel command line
#define MPTABLE_START        0x9fc00                     // the MP table
#define KERNEL_START         0x100000                    // 1 MiB: the kernel
#define VIRTIO_CONSOLE_START 0xd0000000                  // the virtio console's registers
#define VIRTIO_CONSOLE_SIZE  0x1000                      //   4 KiB of them
#define IO_APIC_DEFAULT_PHYS_BASE 0xfec00000             // the IOAPIC's registers
#define APIC_DEFAULT_PHYS_BASE 0xfee00000                // each CPU's LAPIC registers
#define TSS_START            0xfffbd000                  // 3 pages Intel VT-x needs
// The devices section 8 emulates, and the IRQs (here IRQ n = GSI n = IOAPIC pin n):
#define COM1_PORT            0x3f8                       // COM1, a 16550 UART: 8 I/O ports
#define COM1_SIZE            8
#define COM1_IRQ             4
#define VIRTIO_CONSOLE_IRQ   5
#define I8042_COMMAND_REG    0x64                        // the keyboard controller's command port
#define KBD_CCMD_RESET       0xfe                        //   and its command: pulse CPU reset

static unsigned char *ram;                               // guest RAM, mapped in section 2

static void die(const char *message) { fputs(message, stderr); exit(1); }

static long check(long result, const char *what) {       // stop with a message if a call failed;
  if (result < 0) { perror(what); exit(1); }             //   every call here fails with -1
  return result;
}

// One KVM call: an ioctl on a KVM file descriptor, checked; a failure prints the request name.
#define IOCTL(fd, request, arg) check(ioctl(fd, request, arg), #request)

static void *map(int fd, size_t size,                    // mmap(), checked: size bytes of fd,
                 int prot, int flags) {                  //   or fresh memory when fd is -1
  void *mapped = mmap(0, size, prot, flags, fd, 0);
  return (void *)check((long)mapped, "mmap");            // MAP_FAILED is -1, so check() sees it
}

static unsigned char *map_file(const char *path,         // a whole file, mapped read-only,
                               size_t *size) {           //   and its size
  int fd = check(open(path, O_RDONLY), path);
  struct stat info;
  check(fstat(fd, &info), path);
  *size = info.st_size;
  return map(fd, *size, PROT_READ, MAP_PRIVATE);
}

// The low byte of the sum of count bytes; a correct MP checksum makes it 0.
static unsigned char byte_sum(unsigned char *bytes, int count) {
  unsigned char total = 0;
  while (count--) total += *bytes++;
  return total;
}

// COM1: a 16550 UART at ports 0x3f8-0x3ff, as much of it as Linux's 8250 driver needs to send.
// The register names and bits are Linux's, from linux/serial_reg.h.
//
//   reg      guest reads (IN)                   guest writes (OUT)
//   RX, TX   0, no input                        putchar(), and the TX register is empty again
//   IER      IER                                IER: turning THRI on raises "TX empty"
//   IIR,FCR  IIR_THRI if "TX empty" is pending, FCR: no FIFO, ignored
//            else IIR_NO_INT
//   LCR      LCR                                LCR; DLAB turns RX/TX and IER into DLL and DLM,
//                                               the baud divisor
//   LSR      TEMT | THRE: transmitter empty     -
//
//   IRQ 4 is up while THRI is on and a "TX empty" event is pending; reading IIR acks the event.
//
static unsigned char com1(int vm, int reg, int is_out, unsigned char byte) {
  static unsigned char ier, lcr, thr_empty;              // IER, LCR, and a "TX empty" not acked
  unsigned char reply = 0;                               // what an IN reads
  if (lcr & UART_LCR_DLAB && reg <= UART_DLM) return 0;  // the baud divisor; no need to
  if (is_out) {                                          //   emulate it
    if (reg == UART_TX) { putchar(byte); thr_empty = 1; }
    if (reg == UART_IER) { thr_empty |= byte & ~ier & UART_IER_THRI; ier = byte; }
    if (reg == UART_LCR) lcr = byte;
  } else {
    if (reg == UART_IER) reply = ier;
    if (reg == UART_IIR) reply = ier & UART_IER_THRI && thr_empty ? UART_IIR_THRI : UART_IIR_NO_INT;
    if (reg == UART_IIR && reply == UART_IIR_THRI) thr_empty = 0;
    if (reg == UART_LCR) reply = lcr;
    if (reg == UART_LSR) reply = UART_LSR_TEMT | UART_LSR_THRE;
  }
  struct kvm_irq_level irq_line = {.irq = COM1_IRQ, .level = ier & UART_IER_THRI && thr_empty};
  IOCTL(vm, KVM_IRQ_LINE, &irq_line);                    // IRQ 4, up or down
  return reply;
}

// The virtio console: the console Linux calls hvc0, at 0xd0000000 with IRQ 5 (section 4 names both
// on the command line). All output after the first boot lines goes through it.
//
//   function                  called from          what it does
//   console_init()            section 1, once      gives KVM two eventfds, starts console_thread()
//   console_thread()          a thread of its own  prints what the guest puts on queue 1
//   console_register_read()   section 8            answers the guest's read of a register
//   console_register_write()  section 8            applies the guest's write to a register
//
//   guest                       KVM                          vmmlite
//   setup registers   ------->  KVM_EXIT_MMIO          --->  console_register_read(),
//                                                            console_register_write()
//   kick queue 1      ------->  ioeventfd: wakes console_thread(); the vCPU runs on
//   IRQ 5             <-------  irqfd                  <---  console_thread() writes irq_eventfd
//
//   IRQ 5 is set in three places, which must agree:
//   where vmmlite sets it                      who uses it
//   section 4, the command line:          -->  virtio-mmio driver makes a device at 0xd0000000
//     virtio_mmio.device=4K@0xd0000000:5       with IRQ 5, then calls request_irq(5, vm_interrupt)
//                                              when the console sets up its queues
//   section 5, the MP table:              -->  Linux learns IRQ 5 is IOAPIC pin 5, and puts
//     ISA IRQ 5 -> IOAPIC pin 5                IRQ 5's vector in pin 5's entry
//   console_init(), the irqfd:            -->  KVM raises GSI 5, which is IOAPIC pin 5, each
//     .gsi = 5                                 time console_thread() writes irq_eventfd
//
//   Firecracker, Cloud Hypervisor and libkrun wire devices on KVM the same way: the kick never
//   reaches section 8, and the data never crosses MMIO: console_thread() reads guest RAM itself.
//
#define VIRT_MAGIC           0x74726976                  // "virt" (QEMU's names: the magic
#define VIRT_VERSION         2                           //   value and version 2, virtio 1.0+)
#define CONSOLE_FEATURES     (1ULL << VIRTIO_F_VERSION_1)
#define TRANSMIT_QUEUE       1                           // queue 1: the console's output
#define QUEUE_SIZE_MAX       256                         // the largest queue it accepts
static int kick_eventfd, irq_eventfd;                    // guest -> console_thread() -> guest
// The registers the driver writes and reads back: STATUS, DEVICE_FEATURES_SEL, QUEUE_SEL.
static unsigned device_status, features_select, queue_select;
static _Atomic unsigned interrupt_status;                // console_thread() sets INT_VRING;
                                                         //   the guest's IRQ 5 handler acks it

// Queue 1, the transmit queue, as the driver set it up: three areas in guest RAM, and how far
// console_thread() has read.
static unsigned queue_size;                              // entries in each area, from QUEUE_NUM
static struct vring_desc *desc_table;                    // the buffers: {guest address, length,
                                                         //   flags, next}
static struct vring_avail *avail_ring;                   // driver -> device: idx counts the chains
                                                         //   added; ring[] holds their heads
static struct vring_used *used_ring;                     // device -> driver: heads handed back,
                                                         //   counted by idx
static unsigned short next_avail;                        // how many avail_ring entries were taken

// console_thread(): the device, on its own thread. read() sleeps until the guest kicks queue 1.
// Then, for each head the driver has added since, it writes the chain's buffers to stdout and
// puts the head in the used ring, which hands the buffers back. Last, it sets bit 0 of
// interrupt_status and writes irq_eventfd, so KVM raises IRQ 5. Linux's console driver spins on
// the used ring until its buffer comes back, so every line the guest prints waits for this thread.
static void *console_thread(void *arg) {
  // read() returns the number of kicks since the last read, at least 1
  for (unsigned long kicks; read(kick_eventfd, &kicks, sizeof kicks) == sizeof kicks;) {
    // acquire: after reading avail_ring->idx, see the entries the driver wrote before bumping it
    while (next_avail != __atomic_load_n(&avail_ring->idx, __ATOMIC_ACQUIRE)) {
      unsigned short head = avail_ring->ring[next_avail++ % queue_size];
      // walk the descriptor chain; a buffer at guest address addr is at ram + addr here
      for (unsigned short desc = head;; desc = desc_table[desc].next) {
        fwrite(ram + desc_table[desc].addr, 1, desc_table[desc].len, stdout);
        if (!(desc_table[desc].flags & VRING_DESC_F_NEXT)) break;
      }
      // hand the chain back; len stays 0, since nothing was written into the buffers
      used_ring->ring[used_ring->idx % queue_size].id = head;
      // release: the entry above becomes visible to the driver before the new idx does
      __atomic_store_n(&used_ring->idx, used_ring->idx + 1, __ATOMIC_RELEASE);
    }
    interrupt_status |= VIRTIO_MMIO_INT_VRING;           // "used buffers"
    // KVM injects IRQ 5
    if (write(irq_eventfd, &kicks, sizeof kicks) != sizeof kicks) break;
  }
  return arg;
}

// console_init(): connects the console to KVM, then starts console_thread(). An eventfd is a
// counter in the kernel: a write adds to it, and a read waits until it is nonzero, returns it and
// resets it.
//   kick_eventfd  KVM_IOEVENTFD: when the guest writes 1, queue 1's index, to QUEUE_NOTIFY, KVM
//                 adds 1 to it and resumes the guest at once, with no exit to section 8. Other
//                 values still exit, so queue 0's kick reaches console_register_write().
//   irq_eventfd   KVM_IRQFD: when console_thread() writes to it, KVM raises IRQ 5 in the guest.
static void console_init(int vm) {
  // two counters, both 0
  kick_eventfd = eventfd(0, 0), irq_eventfd = eventfd(0, 0);
  struct kvm_ioeventfd ioeventfd = {.addr = VIRTIO_CONSOLE_START + VIRTIO_MMIO_QUEUE_NOTIFY,
      .len = sizeof(uint32_t), .fd = kick_eventfd, .datamatch = TRANSMIT_QUEUE,
      .flags = KVM_IOEVENTFD_FLAG_DATAMATCH};
  IOCTL(vm, KVM_IOEVENTFD, &ioeventfd);                  // queue 1's kick wakes console_thread()
  struct kvm_irqfd irqfd = {.fd = irq_eventfd, .gsi = VIRTIO_CONSOLE_IRQ};
  IOCTL(vm, KVM_IRQFD, &irqfd);                          // a write to irq_eventfd raises IRQ 5
  pthread_t thread;
  pthread_create(&thread, 0, console_thread, 0);         // it sleeps until the first kick
}

// console_register_read(), console_register_write(): the console's registers, its control path;
// the data itself goes through queue 1. Section 8 calls them on the vCPU thread, once for each
// MMIO exit into the console's 4 KiB, with reg the offset in it:
//
//   reg    register             guest reads                guest writes
//   0x000  MAGIC_VALUE          VIRT_MAGIC, "virt"         -
//   0x004  VERSION              VIRT_VERSION, 2            -
//   0x008  DEVICE_ID            VIRTIO_ID_CONSOLE, 3       -
//   0x010  DEVICE_FEATURES      CONSOLE_FEATURES, 32 bits  -
//                               at a time
//   0x014  DEVICE_FEATURES_SEL  -                          features_select
//   0x030  QUEUE_SEL            -                          queue_select
//   0x034  QUEUE_NUM_MAX        QUEUE_SIZE_MAX, 256        -
//   0x038  QUEUE_NUM            -                          queue_size *
//   0x050  QUEUE_NOTIFY         -                          a kick; only queue 0's comes here
//   0x060  INTERRUPT_STATUS     interrupt_status           -
//   0x064  INTERRUPT_ACK        -                          clears those interrupt_status bits
//   0x070  STATUS               device_status              device_status; 0 resets the device
//   0x080  QUEUE_DESC_LOW       -                          desc_table = ram + value *
//   0x090  QUEUE_AVAIL_LOW      -                          avail_ring = ram + value *
//   0x0a0  QUEUE_USED_LOW       -                          used_ring = ram + value *
//   other                       0                          ignored
//   * only while queue_select is TRANSMIT_QUEUE: vmmlite serves queue 1 alone
//
// Linux's virtio-mmio driver at boot, one MMIO exit per register access:
//   1. reads MAGIC_VALUE, VERSION, DEVICE_ID and VENDOR_ID, then writes STATUS 0: a reset
//   2. adds the STATUS bits ACKNOWLEDGE and DRIVER, reading STATUS and writing it back each time
//   3. reads DEVICE_FEATURES with features_select 1 and 0, accepts VIRTIO_F_VERSION_1, adds
//      FEATURES_OK, and reads STATUS again to check that the device kept it
//   4. sets up queue 0 (input, which vmmlite ignores) and queue 1: QUEUE_SEL, QUEUE_NUM_MAX,
//      QUEUE_NUM, the three ring addresses, QUEUE_READY
//   5. adds DRIVER_OK: the device is live
//   6. gives queue 0 its 256 empty input buffers, kicking after each: 256 writes of 0 to
//      QUEUE_NOTIFY, which the ioeventfd lets through to here
//   7. reads STATUS once more, to check that DRIVER_OK is set
// After that, only its IRQ 5 handler comes here: it reads INTERRUPT_STATUS and writes the value
// back to INTERRUPT_ACK.
static unsigned console_register_read(unsigned long reg) {
  switch (reg) {
  case VIRTIO_MMIO_MAGIC_VALUE:      return VIRT_MAGIC;
  case VIRTIO_MMIO_VERSION:          return VIRT_VERSION;
  case VIRTIO_MMIO_DEVICE_ID:        return VIRTIO_ID_CONSOLE;
  // the 64 feature bits, 32 at a time: features_select 0 picks bits 0-31, 1 picks bits 32-63
  case VIRTIO_MMIO_DEVICE_FEATURES:
    return features_select < 2 ? CONSOLE_FEATURES >> 32 * features_select : 0;
  case VIRTIO_MMIO_QUEUE_NUM_MAX:    return QUEUE_SIZE_MAX;
  case VIRTIO_MMIO_INTERRUPT_STATUS: return interrupt_status;
  case VIRTIO_MMIO_STATUS:           return device_status;
  }
  return 0;
}

// console_register_write(): the guest wrote value to the register at offset reg. It keeps what
// the console needs and ignores the rest: the features the driver accepts, QUEUE_READY, queue 0's
// setup and kick, and the high halves of addresses, since guest RAM is below 4 GiB.
static void console_register_write(unsigned long reg, unsigned value) {
  switch (reg) {
  // which 32 feature bits the next DEVICE_FEATURES read returns: 0 = bits 0-31, 1 = bits 32-63
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: features_select = value; break;
  // which queue the QUEUE_* writes that follow set up
  case VIRTIO_MMIO_QUEUE_SEL:        queue_select = value; break;
  // clear the interrupt_status bits that the IRQ 5 handler acks
  case VIRTIO_MMIO_INTERRUPT_ACK:    interrupt_status &= ~value; break;
  // the driver's progress bits, which it reads back; writing 0 is its reset
  case VIRTIO_MMIO_STATUS:           device_status = value; break;
  }
  // the QUEUE_* writes set up the queue that the last QUEUE_SEL write chose; vmmlite keeps only
  // queue 1, the transmit queue (queue 0 is input, which it ignores)
  if (queue_select == TRANSMIT_QUEUE) switch (reg) {
  case VIRTIO_MMIO_QUEUE_NUM:        queue_size = value; break;
  // each area's guest physical address X becomes a pointer: guest address X is ram + X here
  case VIRTIO_MMIO_QUEUE_DESC_LOW:   desc_table = (void *)(ram + value); break;
  case VIRTIO_MMIO_QUEUE_AVAIL_LOW:  avail_ring = (void *)(ram + value); break;
  case VIRTIO_MMIO_QUEUE_USED_LOW:   used_ring = (void *)(ram + value); break;
  }
}

int main(int argc, char **argv) {
  if (argc != 3) die("usage: vmmlite bzImage initramfs\n");
  // the kernel image (bzImage) and the initramfs archive (cpio), mapped from their files
  size_t bzimage_size, initramfs_size;
  unsigned char *bzimage = map_file(argv[1], &bzimage_size);
  unsigned char *initramfs = map_file(argv[2], &initramfs_size);

  // ------------------------------------------------------------------------------------------
  // 1. A VM: an empty address space, plus interrupt controllers and a timer.
  //
  //   kvm fd --KVM_CREATE_VM--> vm fd: one guest, all of its state inside the host kernel
  //                              +-- TSS: 3 pages at 0xfffbd000, used by Intel VT-x only
  //                              +-- irqchip: PIC (ports 0x20, 0xa0), IOAPIC, a LAPIC per vCPU
  //                              +-- PIT: ports 0x40-0x43, plus 0x61 (speaker dummy)
  //                              +-- ioeventfd, irqfd: the virtio console's kick, IRQ 5
  //   The kernel handles guest accesses to these; they never reach the loop in section 8.
  //
  int kvm = check(open("/dev/kvm", O_RDWR), "/dev/kvm");
  int vm = IOCTL(kvm, KVM_CREATE_VM, 0);
  IOCTL(vm, KVM_SET_TSS_ADDR, TSS_START);                // 3 pages Intel VT-x needs
  IOCTL(vm, KVM_CREATE_IRQCHIP, 0);                      // PIC, IOAPIC, LAPICs in the kernel
  struct kvm_pit_config pit_config = {
      .flags = KVM_PIT_SPEAKER_DUMMY};                   // also emulate port 0x61
  IOCTL(vm, KVM_CREATE_PIT2, &pit_config);               // the 8254 timer, in the kernel
  console_init(vm);                                      // the virtio console and its thread

  // ------------------------------------------------------------------------------------------
  // 2. Guest RAM: plain host memory that KVM maps at guest physical address 0.
  //
  //   vmmlite's virtual memory               guest physical memory
  //   ram + RAM_SIZE -> +---------+          +---------+ <- 0x20000000
  //                     | 512 MiB | memslot 0| 512 MiB |
  //                     |         |<========>|         |    the same bytes: ram[X] is
  //                     |         |          |         |    guest physical address X
  //   ram ------------> +---------+          +---------+ <- 0x00000000
  //
  ram = map(-1, RAM_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS                  // not backed by a file,
            | MAP_NORESERVE);                            //   no swap reserved up front
  struct kvm_userspace_memory_region memslot = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = RAM_SIZE,
      .userspace_addr = (unsigned long)ram};
  IOCTL(vm, KVM_SET_USER_MEMORY_REGION, &memslot);       // now guest address X is ram[X]

  // ------------------------------------------------------------------------------------------
  // 3. The kernel. A bzImage is a boot sector, the 16-bit setup code, then the real kernel.
  //
  //   bzImage (bzimage[], file offsets)                guest RAM (ram[])
  //   0x000        +------------------------------+
  //                | boot sector (512 B)          |      skipped: real-mode code
  //   0x1f1        |   setup header begins -------|----> zero page, section 4
  //   0x200        +------------------------------+
  //                | setup code, setup_sects*512 B|      skipped: 16-bit code
  //   setup_size   +------------------------------+
  //                | decompressor, with its       |----> KERNEL_START (0x100000), copied
  //                | 64-bit entry at +0x200       |      here; the vCPU starts at 0x100200
  //                | + compressed vmlinux         |
  //   bzimage_size +------------------------------+
  //
  // the boot protocol's numbers (Documentation/arch/x86/boot.rst), with Firecracker's names
#define KERNEL_HDR_MAGIC     0x53726448                  // "HdrS"
#define BZIMAGE_64BIT_ENTRY_OFFSET 0x200                 // startup_64, from the kernel's start
#define DEFAULT_SETUP_SECTS  4                           // what setup_sects 0 means
#define SECTOR_SIZE          512
  // the file starts laid out like struct boot_params, so its setup header is at hdr
  struct setup_header *header = &((struct boot_params *)bzimage)->hdr;
  if (header->header != KERNEL_HDR_MAGIC || !(header->xloadflags & XLF_KERNEL_64))
    die("vmmlite: need a bzImage with a 64-bit entry\n");
  // setup_sects sectors of setup code, plus the boot sector
  unsigned setup_sects = header->setup_sects ? header->setup_sects : DEFAULT_SETUP_SECTS;
  size_t setup_size = (setup_sects + 1) * SECTOR_SIZE;
  memcpy(ram + KERNEL_START, bzimage + setup_size, bzimage_size - setup_size);

  // ------------------------------------------------------------------------------------------
  // 4. The zero page: everything the kernel learns about this machine. Guest RAM afterwards:
  //
  //   guest physical   contents, high addresses on top       written by
  //   0x20000000      +-----------------------------------+  end of RAM
  //                   | initramfs (cpio archive)          |  vmmlite, here
  //   initramfs_start +-----------------------------------+
  //                   | free: the guest unpacks vmlinux   |  the guest, during boot
  //                   | here, then uses it as its memory  |
  //                   +-----------------------------------+
  //                   | decompressor + compressed vmlinux |  vmmlite, section 3
  //   0x00100000      +-----------------------------------+
  //                   | MP table, then the legacy BIOS    |  vmmlite, section 5; not in the E820
  //                   | hole                              |
  //   0x0009fc00      +-----------------------------------+
  //   0x00020000      | command line                      |  vmmlite, here
  //   0x00009000      | page tables: PML4, PDPT, PD       |  vmmlite, section 6
  //   0x00007000      | zero page: struct boot_params     |  vmmlite, here; the kernel reads it
  //   0x00000500      | GDT                               |  vmmlite, section 6
  //   0x00000000      +-----------------------------------+
  //
  //   Zero page fields set here, as offsets from 0x7000; every other byte stays 0:
  //   +0x1e8  e820_entries    2                  +0x218  ramdisk_image  initramfs_start
  //   +0x1f1  hdr             copied from file   +0x21c  ramdisk_size   initramfs_size
  //   +0x210  type_of_loader  0xff, "other"      +0x228  cmd_line_ptr   0x20000
  //   +0x2d0  e820_table      {0, 0x9fc00, RAM}, {1 MiB, RAM_SIZE - 1 MiB, RAM}
  //
#define KERNEL_LOADER_OTHER  0xff                        // type_of_loader: no assigned ID
#define COMMAND_LINE_SIZE    2048                        // Linux's x86 limit
#define E820_TYPE_RAM        1                           // an E820 entry of usable RAM
  // fresh RAM, so already all 0
  struct boot_params *zero_page = (void *)(ram + ZERO_PAGE_START);
  // start from the kernel's own header; it ends at 0x202 plus the byte at 0x201, the offset in
  // the jump instruction at 0x200
  memcpy(&zero_page->hdr, header, offsetof(struct setup_header, header) + (header->jump >> 8));
  zero_page->hdr.type_of_loader = KERNEL_LOADER_OTHER;   // 0 = Linux skips the initrd
  zero_page->hdr.cmd_line_ptr = CMDLINE_START;
  // log to COM1 until hvc0, the virtio console, takes over; reboot on a panic; the console's
  // registers and IRQ, as <size>@<address>:<irq>. It prints, as Linux reads it:
  //   earlycon=uart8250,io,0x3f8 console=hvc0 panic=-1 virtio_mmio.device=4K@0xd0000000:5
  snprintf((char *)ram + CMDLINE_START, COMMAND_LINE_SIZE,
           "earlycon=uart8250,io,%#x console=hvc0 panic=-1 virtio_mmio.device=%dK@%#x:%d",
           COM1_PORT, VIRTIO_CONSOLE_SIZE / 1024, VIRTIO_CONSOLE_START, VIRTIO_CONSOLE_IRQ);
  // the initramfs goes at the top of RAM, page aligned
  unsigned long initramfs_start = (RAM_SIZE - initramfs_size) & ~(PAGE_SIZE - 1);
  memcpy(ram + initramfs_start, initramfs, initramfs_size);
  zero_page->hdr.ramdisk_image = initramfs_start;
  zero_page->hdr.ramdisk_size = initramfs_size;
  struct boot_e820_entry e820[] = {                      // usable RAM: below the MP table,
      {0, MPTABLE_START, E820_TYPE_RAM},                 //   and from 1 MiB to the end
      {KERNEL_START, RAM_SIZE - KERNEL_START, E820_TYPE_RAM}};
  memcpy(zero_page->e820_table, e820, sizeof e820);
  zero_page->e820_entries = sizeof e820 / sizeof e820[0];

  // ------------------------------------------------------------------------------------------
  // 5. The MP table: how Linux finds the CPU, the IOAPIC, and the IOAPIC pin of each ISA IRQ.
  //    Linux searches the last KB below 640 KiB for "_MP_"; the E820 keeps it out of RAM.
  //    Its structs, fields and constants are Linux's, from arch/x86/include/asm/mpspec_def.h.
  //
  //   offset  bytes    entry                    what it tells Linux
  //   0       16       floating pointer "_MP_"  where the header is
  //   16      44       header "PCMP"            how many entries follow; where the LAPIC is
  //   60      20       CPU                      one CPU: the boot CPU
  //   80      8        bus                      bus 0 is an ISA bus
  //   88      8        IOAPIC                   one IOAPIC, and where it is
  //   96      16 x 8   IRQ routes               the IOAPIC pin of each ISA IRQ
  //   224     2 x 8    local IRQ routes         what each CPU's LINT0 and LINT1 pins receive
  //
#define MP_PROCESSOR         0                           // entry types
#define MP_BUS               1
#define MP_IOAPIC            2
#define MP_INTSRC            3
#define MP_LINTSRC           4
#define MP_IRQPOL_DEFAULT    0                           // an IRQ's polarity and trigger: as its
#define MP_IRQTRIG_DEFAULT   0                           //   bus defines; ISA: active high, edge
#define CPU_ENABLED          1                           // a CPU's flags
#define CPU_BOOTPROCESSOR    2
#define MPC_APIC_USABLE      1                           // an IOAPIC's flags: enabled
#define MP_APIC_ALL          0xff                        // every LAPIC
#define NR_IRQS_LEGACY       16                          // the ISA IRQs, 0-15
#define MPC_SPEC             4                           // MP spec version 1.4
#define APIC_VERSION         0x14                        // the version KVM's LAPIC reports
#define IOAPIC_VERSION_ID    0x11                        // the version KVM's IOAPIC reports
#define BOOT_APIC_ID         0                           // vCPU 0's LAPIC
#define IOAPIC_ID            1                           // the IOAPIC's ID, after the LAPIC's
#define ISA_BUS              0                           // the bus entry's ID
  // interrupt types: vectored by the IOAPIC, NMI, SMI, or vectored by the 8259 PIC
  enum mp_irq_source_types {mp_INT, mp_NMI, mp_SMI, mp_ExtINT};
  struct mpf_intel {                                     // the floating pointer
    char signature[4];
    uint32_t physptr;                                    // the header's address
    uint8_t length, specification, checksum;             // length: in 16-byte units
    // feature1 0: a table follows, rather than a default configuration
    uint8_t feature1, feature2, feature3, feature4, feature5;
  };
  struct mpc_table {                                     // the header
    char signature[4];
    uint16_t length;                                     // the header and every entry
    uint8_t spec, checksum;
    char oem[8], productid[12];
    uint32_t oemptr;
    uint16_t oemsize, oemcount;                          // oemcount: the number of entries
    uint32_t lapic, reserved;                            // lapic: the LAPIC's address
  };
  struct mpc_cpu {uint8_t type, apicid, apicver, cpuflag;
                  uint32_t cpufeature, featureflag, reserved[2];};
  struct mpc_bus {uint8_t type, busid; char bustype[6];};
  struct mpc_ioapic {uint8_t type, apicid, apicver, flags; uint32_t apicaddr;};
  // an IRQ route, and a local IRQ route
  struct mpc_intsrc {uint8_t type, irqtype; uint16_t irqflag;
                     uint8_t srcbus, srcbusirq, dstapic, dstirq;};
  struct mpc_lintsrc {uint8_t type, irqtype; uint16_t irqflag;
                      uint8_t srcbusid, srcbusirq, destapic, destapiclint;};
  struct mptable {                                       // the table vmmlite writes
    struct mpf_intel floating_pointer;
    struct mpc_table header;
    struct mpc_cpu cpu;
    struct mpc_bus bus;
    struct mpc_ioapic ioapic;
    struct mpc_intsrc irq_routes[NR_IRQS_LEGACY];
    struct mpc_lintsrc local_routes[2];                  // LINT0, LINT1
  } *mp = (void *)(ram + MPTABLE_START);                 // fresh RAM, so already all 0
  mp->floating_pointer = (struct mpf_intel){.signature = "_MP_",
      .physptr = MPTABLE_START + offsetof(struct mptable, header),
      .length = sizeof mp->floating_pointer / 16, .specification = MPC_SPEC};
  // the entries: the CPU, the bus, the IOAPIC, then the IRQ routes and the local IRQ routes
  mp->header = (struct mpc_table){.signature = "PCMP", .spec = MPC_SPEC,
      .length = sizeof *mp - sizeof mp->floating_pointer,
      .oemcount = 3 + NR_IRQS_LEGACY + 2, .lapic = APIC_DEFAULT_PHYS_BASE};
  mp->cpu = (struct mpc_cpu){.type = MP_PROCESSOR, .apicid = BOOT_APIC_ID,
      .apicver = APIC_VERSION, .cpuflag = CPU_ENABLED | CPU_BOOTPROCESSOR};
  mp->bus = (struct mpc_bus){MP_BUS, ISA_BUS, "ISA   "};
  mp->ioapic = (struct mpc_ioapic){MP_IOAPIC, IOAPIC_ID, IOAPIC_VERSION_ID, MPC_APIC_USABLE,
      IO_APIC_DEFAULT_PHYS_BASE};
  for (int irq = 0; irq < NR_IRQS_LEGACY; irq++)         // ISA IRQ n -> IOAPIC pin n
    mp->irq_routes[irq] = (struct mpc_intsrc){MP_INTSRC, mp_INT,
        MP_IRQPOL_DEFAULT | MP_IRQTRIG_DEFAULT, ISA_BUS, irq, IOAPIC_ID, irq};
  // on every CPU, the PIC's output goes to LINT0, and NMI to LINT1
  mp->local_routes[0] = (struct mpc_lintsrc){MP_LINTSRC, mp_ExtINT,
      .destapic = MP_APIC_ALL, .destapiclint = 0};
  mp->local_routes[1] = (struct mpc_lintsrc){MP_LINTSRC, mp_NMI,
      .destapic = MP_APIC_ALL, .destapiclint = 1};
  // the checksums: each part's bytes must add up to 0
  mp->header.checksum = -byte_sum((unsigned char *)&mp->header, mp->header.length);
  mp->floating_pointer.checksum = -byte_sum((unsigned char *)&mp->floating_pointer,
                                            sizeof mp->floating_pointer);

  // ------------------------------------------------------------------------------------------
  // 6. Long mode needs a GDT and page tables in guest RAM, where the CPU reads them.
  //
  //   guest physical  table  entries
  //   0x0500          GDT    [0] null  [1] unused  [2] 0x10: 64-bit code  [3] 0x18: data
  //   0x9000          PML4   [0] ---> 0xa000
  //   0xa000          PDPT   [0] ---> 0xb000
  //   0xb000          PD     [n] ---> n * 2 MiB, for n < 512: virtual X = physical X below 1 GiB
  //
  //   A GDT entry packs a segment's base, limit and flags into 8 bytes; GDT_ENTRY() below does
  //   the packing. Its flags, with Linux's names (arch/x86/include/asm/desc_defs.h):
  //
  //   bits 15-12  G, D/B, L, AVL      bit 7  present      bits 6-5  ring      bit 4  S
  //   bits 3-0    the type: bit 0 accessed, bit 1 writable (data) or readable (code), bit 3 code
  //
  //   GDT[2] = GDT_ENTRY(DESC_CODE64, 0, FLAT_LIMIT) = 0x00af9b000000ffff
  //   GDT[3] = GDT_ENTRY(DESC_DATA64, 0, FLAT_LIMIT) = 0x00cf93000000ffff
  //
#define DESC_ACCESSED        0x0001
#define DESC_DATA_WRITABLE   0x0002
#define DESC_CODE_READABLE   0x0002
#define DESC_CODE_EXECUTABLE 0x0008
#define DESC_S               0x0010                      // code or data, not a system segment
#define DESC_PRESENT         0x0080
#define DESC_LONG_CODE       0x2000                      // L: 64-bit code
#define DESC_DB              0x4000                      // D/B: 32-bit
#define DESC_GRANULARITY_4K  0x8000                      // G: the limit counts 4 KiB units
#define DESC_CODE64          (DESC_S | DESC_PRESENT | DESC_ACCESSED | DESC_CODE_READABLE \
                              | DESC_CODE_EXECUTABLE | DESC_GRANULARITY_4K | DESC_LONG_CODE)
#define DESC_DATA64          (DESC_S | DESC_PRESENT | DESC_ACCESSED | DESC_DATA_WRITABLE \
                              | DESC_GRANULARITY_4K | DESC_DB)
#define GDT_ENTRY(flags, base, limit) /* Linux's macro of that name */ \
    (((base) & 0xff000000ULL) << 32 | ((flags) & 0xf0ffULL) << 40 \
     | ((limit) & 0xf0000ULL) << 32 | ((base) & 0xffffffULL) << 16 | ((limit) & 0xffffULL))
#define FLAT_LIMIT           0xfffff                     // 4 GiB in 4 KiB units
#define GDT_ENTRY_BOOT_CS    2                           // Linux's boot GDT slots, and their
#define GDT_ENTRY_BOOT_DS    3                           //   selectors: slot * 8, the entry size
#define BOOT_CS              (GDT_ENTRY_BOOT_CS * 8)
#define BOOT_DS              (GDT_ENTRY_BOOT_DS * 8)
  unsigned long gdt[] = {[GDT_ENTRY_BOOT_CS] = GDT_ENTRY(DESC_CODE64, 0, FLAT_LIMIT),
                         [GDT_ENTRY_BOOT_DS] = GDT_ENTRY(DESC_DATA64, 0, FLAT_LIMIT)};
  memcpy(ram + GDT_START, gdt, sizeof gdt);
#define PAGE_PRESENT         (1 << 0)                    // page-table entry bits (Linux's
#define PAGE_RW              (1 << 1)                    //   _PAGE_PRESENT, _PAGE_RW and
#define PAGE_PSE             (1 << 7)                    //   _PAGE_PSE: a 2 MiB page)
#define HUGE_PAGE_SIZE       (2UL << 20)                 // 2 MiB
  unsigned long *pml4 = (void *)(ram + PML4_START), *pdpt = (void *)(ram + PDPT_START);
  unsigned long *pd = (void *)(ram + PD_START);
  pml4[0] = PDPT_START | PAGE_PRESENT | PAGE_RW;         // PML4[0] ---> PDPT
  pdpt[0] = PD_START | PAGE_PRESENT | PAGE_RW;           // PDPT[0] ---> PD
  // PD[page] ---> a 2 MiB page, for each of the 512 entries in the PD's page
  for (unsigned long page = 0; page < PAGE_SIZE / sizeof *pd; page++)
    pd[page] = page * HUGE_PAGE_SIZE | PAGE_PRESENT | PAGE_RW | PAGE_PSE;

  // ------------------------------------------------------------------------------------------
  // 7. One vCPU: the host's CPU features, and the registers the 64-bit boot protocol wants.
  //
  //   register        value          meaning
  //   CS              0x10           GDT[2]: 64-bit code
  //   DS ES FS GS SS  0x18           GDT[3]: flat 4 GiB data
  //   GDTR, CR3       0x500, 0x9000  ---> GDT and PML4 (section 6)
  //   CR0 CR4 EFER    PE PG, PAE,    paging on, long mode active
  //                   LME LMA
  //   RIP             0x100200       ---> the 64-bit entry, startup_64 (section 3)
  //   RSI             0x7000         ---> zero page (section 4)
  //   RFLAGS          0x2            interrupts off
  //   CPUID           host+KVM       leaf 1: APIC ID 0, bit 31 (in a VM), bit 24 (TSC deadline)
  //
  //   run = mmap of the vCPU fd:  page 0  struct kvm_run: exit_reason, io, mmio, ...
  //                               page 1  port I/O data bytes, at run + run->io.data_offset
  //
#define KVM_MAX_CPUID_ENTRIES 256                        // KVM's limit (kvm_host.h)
#define CPUID_FEATURES       1                           // CPUID leaf 1: the feature bits
#define CPUID_EBX_APIC_ID    0xff000000u                 // leaf 1 EBX bits 31-24: the APIC ID
#define CPUID_EXT_TSC_DEADLINE_TIMER (1u << 24)          // leaf 1 ECX bits (QEMU's names)
#define CPUID_EXT_HYPERVISOR (1u << 31)
#define EFER_LME             (1 << 8)                    // long mode enable, and long mode
#define EFER_LMA             (1 << 10)                   //   active (Linux's names)
  int vcpu = IOCTL(vm, KVM_CREATE_VCPU, BOOT_APIC_ID);
  long run_size = IOCTL(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = map(vcpu, run_size,              // map the vCPU fd itself,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED);                 //   shared with KVM
  struct kvm_cpuid2 *cpuid = calloc(1, sizeof *cpuid
      + KVM_MAX_CPUID_ENTRIES * sizeof cpuid->entries[0]);
  cpuid->nent = KVM_MAX_CPUID_ENTRIES;
  IOCTL(kvm, KVM_GET_SUPPORTED_CPUID, cpuid);            // fill it with what host + KVM offer
  for (unsigned entry = 0; entry < cpuid->nent; entry++) // in leaf 1, the feature bits:
    if (cpuid->entries[entry].function == CPUID_FEATURES) {
      cpuid->entries[entry].ebx &= ~CPUID_EBX_APIC_ID;   //   APIC ID 0, not the host's
      // set "in a VM" and "TSC deadline"
      cpuid->entries[entry].ecx |= CPUID_EXT_HYPERVISOR | CPUID_EXT_TSC_DEADLINE_TIMER;
    }
  IOCTL(vcpu, KVM_SET_CPUID2, cpuid);                    // the guest's CPUID answers come from here
  struct kvm_sregs sregs;                                // segment and control registers
  IOCTL(vcpu, KVM_GET_SREGS, &sregs);                    // start from the reset state
  struct kvm_segment segment = {                         // a flat segment:
      .base = 0, .limit = ~0u,                           //   all 4 GiB,
      .present = 1, .s = 1, .db = 1, .g = 1};            //   valid, code/data, 32-bit, 4 KiB units
  segment.type = DESC_DATA_WRITABLE | DESC_ACCESSED;     // read/write data, selector
  segment.selector = BOOT_DS;                            //   __BOOT_DS, for every data segment:
  sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = segment;
  // execute/read code, selector __BOOT_CS, 64-bit, for the code segment
  segment.type = DESC_CODE_EXECUTABLE | DESC_CODE_READABLE | DESC_ACCESSED;
  segment.selector = BOOT_CS;
  segment.db = 0, segment.l = 1;
  sregs.cs = segment;
  sregs.gdt.base = GDT_START, sregs.gdt.limit = sizeof gdt - 1;
  sregs.cr3 = PML4_START;                                // the page tables
  sregs.cr4 |= X86_CR4_PAE;                              // physical address extension
  sregs.cr0 |= X86_CR0_PG | X86_CR0_PE;                  // paging, protected mode
  sregs.efer |= EFER_LMA | EFER_LME;                     // long mode
  IOCTL(vcpu, KVM_SET_SREGS, &sregs);                    // apply
  struct kvm_regs regs = {
      .rip = KERNEL_START + BZIMAGE_64BIT_ENTRY_OFFSET,  // the 64-bit entry point
      .rsi = ZERO_PAGE_START,                            // boot protocol: RSI = the zero page
      .rflags = X86_EFLAGS_FIXED};                       // interrupts off; bit 1 is always set
  IOCTL(vcpu, KVM_SET_REGS, &regs);

  // ------------------------------------------------------------------------------------------
  // 8. Run. KVM returns to us only when the guest touches a device we must emulate:
  //
  //   port         device                     guest      vmmlite
  //   0x3f8-0x3ff  COM1, a 16550 UART         IN / OUT   com1(): registers, IRQ 4
  //   0x64         i8042 keyboard controller  OUT 0xfe   guest reset: return 0
  //   0xd0000000   virtio console registers   MMIO       console_register_read(),
  //                                                      console_register_write()
  //   other        any other port or MMIO     IN / OUT   reads 0, writes ignored
  //   A kick of the console's queue 1 never comes here: KVM wakes console_thread() instead.
  //
  for (;;) {
    IOCTL(vcpu, KVM_RUN, 0);                             // run the guest until it exits
    unsigned char *io_data = (unsigned char *)run        // port I/O data bytes,
                             + run->io.data_offset;      //   inside the run pages
    int port = run->io.port;                             // the port, for an I/O exit
    if (run->exit_reason == KVM_EXIT_MMIO) {             // memory that is not RAM
      // an offset into the console's 4 KiB
      unsigned long reg = run->mmio.phys_addr - VIRTIO_CONSOLE_START;
      unsigned value = 0;
      if (run->mmio.is_write) {
        memcpy(&value, run->mmio.data, sizeof value);
        if (reg < VIRTIO_CONSOLE_SIZE) console_register_write(reg, value);
      } else {
        if (reg < VIRTIO_CONSOLE_SIZE) value = console_register_read(reg);
        // other addresses read 0
        memset(run->mmio.data, 0, sizeof run->mmio.data);
        memcpy(run->mmio.data, &value, sizeof value);
      }
    } else if (run->exit_reason != KVM_EXIT_IO) {        // anything else, e.g. a triple fault
      fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
      return 1;
    } else if (port >= COM1_PORT && port < COM1_PORT + COM1_SIZE) {
      // COM1; an IN reads com1()'s reply
      *io_data = com1(vm, port - COM1_PORT, run->io.direction, *io_data);
    } else if (run->io.direction == KVM_EXIT_IO_IN) {    // the guest reads another port:
      memset(io_data, 0, run->io.size);                  //   it reads 0
    } else if (port == I8042_COMMAND_REG && *io_data == KBD_CCMD_RESET) {
      return 0;                                          // the guest rebooted; done
    }                                                    // any other port write: ignored
  }
}
