// vmmlite.c: the smallest VMM that boots Linux (x86_64, KVM). For learning only.
// Build: gcc -O2 -pthread -o vmmlite vmmlite.c    Run: ./vmmlite bzImage initramfs.cpio
//
//   vmmlite (this process)       KVM (host kernel)               CPU (VT-x / AMD-V)
//   +--------------------+ ioctl +-----------------------+ entry +--------------------+
//   | ram: guest RAM     |------>| VM: memslot, PIC,     |------>| runs guest code    |
//   | run: kvm_run page  |<------| IOAPIC, PIT; vCPU     |<------| until a VM exit    |
//   +--------------------+ exits +-----------------------+ exit  +--------------------+

#include <asm/bootparam.h>                               // struct boot_params: the zero page
#include <fcntl.h>
#include <linux/kvm.h>                                   // KVM's ioctl numbers and structs
#include <linux/virtio_mmio.h>                           // virtio-mmio register offsets
#include <linux/virtio_ring.h>                           // the split virtqueue's descriptors, rings
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>                                 // eventfd(): the console's kick, its IRQ
#include <sys/ioctl.h>                                   // ioctl(): how every KVM call is made
#include <sys/mman.h>                                    // mmap(): files, guest RAM, run page
#include <sys/stat.h>
#include <unistd.h>

#define RAM_SIZE             (512UL << 20)               // guest RAM: 512 MiB at address 0
// Where each thing goes, as guest physical addresses:
#define GDT_START            0x500                       // the boot GDT
#define ZERO_PAGE_START      0x7000                      // struct boot_params, the zero page
#define PML4_START           0x9000                      // 3 pages of page tables, PML4 first
#define CMDLINE_START        0x20000                     // the kernel command line
#define MPTABLE_START        0x9fc00                     // the MP table
#define KERNEL_START         0x100000                    // 1 MiB: the kernel
#define VIRTIO_CONSOLE_START 0xd0000000                  // the virtio console's 4 KiB of registers

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
//
//   reg  guest reads (IN)                 guest writes (OUT)
//   0    RBR: 0, no input                 THR: putchar(), and the THR is empty again
//   1    IER                              IER: turning THRI (bit 1) on raises "THR empty"
//   2    IIR: 2 = THR empty, 1 = nothing  FCR: no FIFO, ignored
//   3    LCR                              LCR; bit 7 (DLAB) turns regs 0-1 into the baud divisor
//   5    LSR: 0x60, transmitter empty     -
//
//   IRQ 4 is up while THRI is on and a "THR empty" event is pending; reading IIR acks the event.
//
static unsigned char com1(int vm, int reg, int is_out, unsigned char byte) {
  static unsigned char ier, lcr, thr_empty;              // IER, LCR, and a "THR empty" not acked
  unsigned char reply = 0;                               // what an IN reads
  if (lcr & 0x80 && reg < 2) return 0;                   // DLAB: the baud divisor; no need
  if (is_out) {                                          //   to emulate it
    if (reg == 0) { putchar(byte); thr_empty = 1; }
    if (reg == 1) { thr_empty |= byte & ~ier & 2; ier = byte; }
    if (reg == 3) lcr = byte;
  } else {
    if (reg == 1) reply = ier;
    if (reg == 2) reply = ier & 2 && thr_empty ? 2 : 1;
    if (reg == 2 && reply == 2) thr_empty = 0;
    if (reg == 3) reply = lcr;
    if (reg == 5) reply = 0x60;
  }
  struct kvm_irq_level irq_line = {.irq = 4, .level = ier & 2 && thr_empty};
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
static int kick_eventfd, irq_eventfd;                    // guest -> console_thread() -> guest
// The registers the driver writes and reads back: STATUS, DEVICE_FEATURES_SEL, QUEUE_SEL.
static unsigned device_status, features_select, queue_select;
static _Atomic unsigned interrupt_status;                // console_thread() sets bit 0; the
                                                         //   guest's IRQ 5 handler acks it

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
  for (unsigned long kicks; read(kick_eventfd, &kicks, 8) == 8;) {
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
    interrupt_status |= 1;                               // bit 0: "used buffers"
    if (write(irq_eventfd, &kicks, 8) != 8) break;       // KVM injects IRQ 5
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
      .len = 4, .fd = kick_eventfd, .datamatch = 1, .flags = KVM_IOEVENTFD_FLAG_DATAMATCH};
  IOCTL(vm, KVM_IOEVENTFD, &ioeventfd);                  // queue 1's kick wakes console_thread()
  struct kvm_irqfd irqfd = {.fd = irq_eventfd, .gsi = 5};
  IOCTL(vm, KVM_IRQFD, &irqfd);                          // a write to irq_eventfd raises IRQ 5
  pthread_t thread;
  pthread_create(&thread, 0, console_thread, 0);         // it sleeps until the first kick
}

// console_register_read(), console_register_write(): the console's registers, its control path;
// the data itself goes through queue 1. Section 8 calls them on the vCPU thread, once for each
// MMIO exit into the console's 4 KiB, with reg the offset in it:
//
//   reg    register             guest reads                guest writes
//   0x000  MAGIC_VALUE          "virt"                     -
//   0x004  VERSION              2: virtio 1.0 or later     -
//   0x008  DEVICE_ID            3: a console               -
//   0x010  DEVICE_FEATURES      features_select 0: 0       -
//                               features_select 1: 1, bit
//                               32, VIRTIO_F_VERSION_1
//   0x014  DEVICE_FEATURES_SEL  -                          features_select
//   0x030  QUEUE_SEL            -                          queue_select
//   0x034  QUEUE_NUM_MAX        256: the largest queue     -
//   0x038  QUEUE_NUM            -                          queue_size *
//   0x050  QUEUE_NOTIFY         -                          a kick; only queue 0's comes here
//   0x060  INTERRUPT_STATUS     interrupt_status           -
//   0x064  INTERRUPT_ACK        -                          clears those interrupt_status bits
//   0x070  STATUS               device_status              device_status; 0 resets the device
//   0x080  QUEUE_DESC_LOW       -                          desc_table = ram + value *
//   0x090  QUEUE_AVAIL_LOW      -                          avail_ring = ram + value *
//   0x0a0  QUEUE_USED_LOW       -                          used_ring = ram + value *
//   other                       0                          ignored
//   * only while queue_select is 1: vmmlite serves queue 1 alone
//
// Linux's virtio-mmio driver at boot, one MMIO exit per register access:
//   1. reads MAGIC_VALUE, VERSION and DEVICE_ID, then writes STATUS 0: a reset
//   2. adds the STATUS bits ACKNOWLEDGE and DRIVER, reading STATUS and writing it back each time
//   3. reads DEVICE_FEATURES with features_select 1 and 0, accepts VIRTIO_F_VERSION_1, adds
//      FEATURES_OK, and reads STATUS again to check that the device kept it
//   4. sets up queue 0 (input, which vmmlite ignores) and queue 1: QUEUE_SEL, QUEUE_NUM_MAX,
//      QUEUE_NUM, the three ring addresses, QUEUE_READY
//   5. adds DRIVER_OK: the device is live
// After that, only its IRQ 5 handler comes here: it reads INTERRUPT_STATUS and writes the value
// back to INTERRUPT_ACK.
static unsigned console_register_read(unsigned long reg) {
  switch (reg) {
  case VIRTIO_MMIO_MAGIC_VALUE:      return 0x74726976;  // "virt"
  case VIRTIO_MMIO_VERSION:          return 2;
  case VIRTIO_MMIO_DEVICE_ID:        return 3;           // a console
  // bit 32, VIRTIO_F_VERSION_1, is bit 0 of the upper 32 feature bits
  case VIRTIO_MMIO_DEVICE_FEATURES:  return features_select == 1;
  case VIRTIO_MMIO_QUEUE_NUM_MAX:    return 256;
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
  if (queue_select == 1) switch (reg) {                  // queue 1's size and ring addresses
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
  IOCTL(vm, KVM_SET_TSS_ADDR, 0xfffbd000);               // 3 pages Intel VT-x needs
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
  // the header's magic, and XLF_KERNEL_64: the bzImage has a 64-bit entry
  if (memcmp(bzimage + 0x202, "HdrS", 4) || !(bzimage[0x236] & 1))
    die("vmmlite: need a bzImage with a 64-bit entry\n");
  // setup_sects (0 means 4) sectors of setup code, plus the boot sector
  size_t setup_size = ((bzimage[0x1f1] ? bzimage[0x1f1] : 4) + 1) * 512;
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
  //   +0x210  type_of_loader  0xff               +0x228  cmd_line_ptr   0x20000
  //   +0x2d0  e820_table      {0, 0x9fc00, RAM}, {1 MiB, RAM_SIZE - 1 MiB, RAM}
  //
  // fresh RAM, so already all 0
  struct boot_params *zero_page = (void *)(ram + ZERO_PAGE_START);
  memcpy(&zero_page->hdr, bzimage + 0x1f1,               // start from the kernel's own
         0x202 + bzimage[0x201] - 0x1f1);                //   header; byte 0x201 sets its end
  zero_page->hdr.type_of_loader = 0xff;                  // "other"; 0 = Linux skips the initrd
  zero_page->hdr.cmd_line_ptr = CMDLINE_START;
  strcpy((char *)ram + CMDLINE_START,                    // log to COM1 until hvc0, the virtio
         "earlycon=uart8250,io,0x3f8 console=hvc0 "      //   console, takes over; reboot on a
         "panic=-1 virtio_mmio.device=4K@0xd0000000:5"); //   panic; the console's 4 KiB, IRQ 5
  // the initramfs goes at the top of RAM, 4 KiB aligned
  unsigned long initramfs_start = (RAM_SIZE - initramfs_size) & ~0xfffUL;
  memcpy(ram + initramfs_start, initramfs, initramfs_size);
  zero_page->hdr.ramdisk_image = initramfs_start;
  zero_page->hdr.ramdisk_size = initramfs_size;
  zero_page->e820_table[0] = (struct boot_e820_entry){
      .addr = 0, .size = 0x9fc00, .type = 1};            // usable RAM below the MP table
  zero_page->e820_table[1] = (struct boot_e820_entry){   // usable RAM from 1 MiB to the end
      .addr = KERNEL_START, .size = RAM_SIZE - KERNEL_START,
      .type = 1};                                        // (type 1 means usable RAM)
  zero_page->e820_entries = 2;

  // ------------------------------------------------------------------------------------------
  // 5. The MP table: how Linux finds the CPU, the IOAPIC, and the IOAPIC pin of each ISA IRQ.
  //    Linux searches the last KB below 640 KiB for "_MP_"; the E820 keeps it out of RAM.
  //
  //   mptable[]  bytes    entry                    what it tells Linux
  //   0          16       floating pointer "_MP_"  where the header is
  //   16         44       header "PCMP"            how many entries follow; where the LAPIC is
  //   60         20       CPU                      one CPU: the boot CPU
  //   80         8        bus                      bus 0 is an ISA bus
  //   88         8        IOAPIC                   one IOAPIC, and where it is
  //   96         16 x 8   IRQ routes               the IOAPIC pin of each ISA IRQ
  //   224        2 x 8    local IRQ routes         what each CPU's LINT0 and LINT1 pins receive
  //
  //   Numbers wider than a byte are stored low byte first: 0x10, 0xfc, 0x09, 0 is 0x0009fc10.
  //
  // ROUTE(irq): one IRQ route, 8 bytes. ROUTE(5), byte by byte:
  //
  //   3, 0, 0, 0, 0, 5, 1, 5
  //   |  |  '--'  |  |  |  '--- pin 5 of that IOAPIC
  //   |  |   |    |  |  '------ IOAPIC ID 1, as in the IOAPIC entry
  //   |  |   |    |  '--------- ISA IRQ 5
  //   |  |   |    '------------ bus 0, the ISA bus
  //   |  |   '----------------- flags 0: as the bus defines, so edge-triggered, active high
  //   |  '--------------------- 0: an ordinary interrupt; the IOAPIC entry supplies its vector
  //   '------------------------ type 3: an IRQ route
  //
#define ROUTE(irq) 3, 0, 0, 0, 0, irq, 1, irq            // ISA IRQ irq -> IOAPIC 1, pin irq
  static unsigned char mptable[] = {
      // floating pointer: "_MP_", the header's address 0x0009fc10, its own length in 16-byte
      // units (1), the spec version (4: 1.4); byte 10, its checksum, is set below
      '_', 'M', 'P', '_', 0x10, 0xfc, 0x09, 0, 1, 4,
      // header: "PCMP"; [22] the spec version (4: 1.4); [50] 21 entries; [52] the LAPIC's
      // address 0xfee00000. [20], its length, and [23], its checksum, are set below.
      [16] = 'P', 'C', 'M', 'P', [22] = 4, [50] = 21, 0, 0, 0, 0xe0, 0xfe,
      // CPU: type 0, LAPIC ID 0, LAPIC version 0x14, flags 3: enabled (1) + boot CPU (2)
      [60] = 0, 0, 0x14, 3,
      // bus: type 1, bus ID 0, bus type "ISA" padded with spaces to 6 bytes
      [80] = 1, 0, 'I', 'S', 'A', ' ', ' ', ' ',
      // IOAPIC: type 2, IOAPIC ID 1, version 0x11, flags 1: enabled, address 0xfec00000
      2, 1, 0x11, 1, 0, 0, 0xc0, 0xfe,
      ROUTE(0), ROUTE(1), ROUTE(2), ROUTE(3), ROUTE(4), ROUTE(5), ROUTE(6), ROUTE(7),
      ROUTE(8), ROUTE(9), ROUTE(10), ROUTE(11), ROUTE(12), ROUTE(13), ROUTE(14), ROUTE(15),
      // local IRQ routes, laid out like ROUTE: type 4; ExtINT (3), the PIC's output, to pin
      // LINT0 (0), and NMI (1) to LINT1 (1); flags, bus and IRQ 0; LAPIC ID 0xff: every CPU
      4, 3, 0, 0, 0, 0, 0xff, 0,
      4, 1, 0, 0, 0, 0, 0xff, 1};
  // the header's table length and checksum, then the floating pointer's checksum
  mptable[20] = sizeof mptable - 16;
  mptable[23] = -byte_sum(mptable + 16, sizeof mptable - 16);
  mptable[10] = -byte_sum(mptable, 16);
  memcpy(ram + MPTABLE_START, mptable, sizeof mptable);

  // ------------------------------------------------------------------------------------------
  // 6. Long mode needs a GDT and page tables in guest RAM, where the CPU reads them.
  //
  //   guest physical  table  entries
  //   0x0500          GDT    [0] null  [1] unused  [2] 0x10: 64-bit code  [3] 0x18: data
  //   0x9000          PML4   [0] ---> 0xa000
  //   0xa000          PDPT   [0] ---> 0xb000
  //   0xb000          PD     [n] ---> n * 2 MiB, for n < 512: virtual X = physical X below 1 GiB
  //
  //   A GDT entry packs a segment's base, limit and kind into 8 bytes. GDT[2], digit by digit:
  //
  //   0x 00 a f 9b 000000 ffff
  //      |  | |  |    |     '--- limit bits 0-15: 0xffff
  //      |  | |  |    '--------- base bits 0-23: 0
  //      |  | |  '-------------- 0x9b: present, ring 0, code: execute/read, accessed
  //      |  | '----------------- limit bits 16-19: 0xf, so the limit is 0xfffff
  //      |  '------------------- flags 0xa: the limit counts 4 KiB units (G), 64-bit code (L)
  //      '---------------------- base bits 24-31: 0
  //
  //   GDT[3], 0x00cf93000000ffff, is the same flat 4 GiB as data: 0x93 is present, ring 0,
  //   data: read/write, accessed; flags 0xc are 4 KiB units (G) and 32-bit (D).
  //
  unsigned long gdt[] = {0, 0, 0x00af9b000000ffff, 0x00cf93000000ffff};
  memcpy(ram + GDT_START, gdt, sizeof gdt);
  unsigned long *pml4 = (void *)(ram + PML4_START);      // 3 tables of 512 entries, one page
  unsigned long *pdpt = pml4 + 512, *pd = pdpt + 512;    //   each, one after another
  pml4[0] = (PML4_START + 0x1000) | 3;                   // PML4[0] ---> PDPT; 3: present, writable
  pdpt[0] = (PML4_START + 0x2000) | 3;                   // PDPT[0] ---> PD
  for (unsigned long page = 0; page < 512; page++)
    pd[page] = page << 21 | 0x83;                        // PD[page] ---> 2 MiB page: 3 + bit 7

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
  int vcpu = IOCTL(vm, KVM_CREATE_VCPU, 0);
  long run_size = IOCTL(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = map(vcpu, run_size,              // map the vCPU fd itself,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED);                 //   shared with KVM
  struct kvm_cpuid2 *cpuid = calloc(1, sizeof *cpuid
      + 256 * sizeof cpuid->entries[0]);
  cpuid->nent = 256;                                     // KVM's maximum number of entries
  IOCTL(kvm, KVM_GET_SUPPORTED_CPUID, cpuid);            // fill it with what host + KVM offer
  for (unsigned entry = 0; entry < cpuid->nent; entry++) // in leaf 1, the feature bits:
    if (cpuid->entries[entry].function == 1) {
      cpuid->entries[entry].ebx &= 0x00ffffff;           //   APIC ID, bits 31-24: 0, not the host's
      cpuid->entries[entry].ecx |= 1u << 31 | 1u << 24;  //   set "in a VM" and "TSC deadline"
    }
  IOCTL(vcpu, KVM_SET_CPUID2, cpuid);                    // the guest's CPUID answers come from here
  struct kvm_sregs sregs;                                // segment and control registers
  IOCTL(vcpu, KVM_GET_SREGS, &sregs);                    // start from the reset state
  struct kvm_segment segment = {                         // a flat segment:
      .base = 0, .limit = ~0u,                           //   all 4 GiB,
      .present = 1, .s = 1, .db = 1, .g = 1};            //   valid, code/data, 32-bit, 4 KiB units
  segment.type = 3;                                      // read/write data, selector
  segment.selector = 0x18;                               //   __BOOT_DS, for every data segment:
  sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = segment;
  segment.type = 11;                                     // execute/read code,
  segment.selector = 0x10;                               //   selector __BOOT_CS,
  segment.db = 0, segment.l = 1;                         //   64-bit,
  sregs.cs = segment;                                    //   for the code segment
  sregs.gdt.base = GDT_START, sregs.gdt.limit = sizeof gdt - 1;
  sregs.cr3 = PML4_START;                                // the page tables
  sregs.cr4 |= 1 << 5;                                   // CR4.PAE
  sregs.cr0 |= 1u << 31 | 1;                             // CR0.PG, CR0.PE: paging on
  sregs.efer |= 1 << 10 | 1 << 8;                        // EFER.LMA, EFER.LME: long mode
  IOCTL(vcpu, KVM_SET_SREGS, &sregs);                    // apply
  struct kvm_regs regs = {
      .rip = KERNEL_START + 0x200,                       // the 64-bit entry point
      .rsi = ZERO_PAGE_START,                            // boot protocol: RSI = the zero page
      .rflags = 2};                                      // interrupts off (bit 1 is always set)
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
        memcpy(&value, run->mmio.data, 4);
        if (reg < 0x1000) console_register_write(reg, value);
      } else {
        if (reg < 0x1000) value = console_register_read(reg);
        memset(run->mmio.data, 0, 8);                    // other addresses read 0
        memcpy(run->mmio.data, &value, 4);
      }
    } else if (run->exit_reason != KVM_EXIT_IO) {        // anything else, e.g. a triple fault
      fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
      return 1;
    } else if (port >= 0x3f8 && port <= 0x3ff) {         // COM1; an IN reads com1()'s reply
      *io_data = com1(vm, port & 7, run->io.direction, *io_data);
    } else if (run->io.direction == KVM_EXIT_IO_IN) {    // the guest reads another port:
      memset(io_data, 0, run->io.size);                  //   it reads 0
    } else if (port == 0x64 && *io_data == 0xfe) {       // keyboard controller reset:
      return 0;                                          //   the guest rebooted; done
    }                                                    // any other port write: ignored
  }
}
