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

#define RAM_SIZE  (512UL << 20)                          // guest RAM: 512 MiB at address 0
#define GDT       0x500                                  // where the boot GDT goes
#define ZERO_PAGE 0x7000                                 // where struct boot_params goes
#define PML4      0x9000                                 // where the 3 pages of page tables go
#define CMDLINE   0x20000                                // where the command line goes
#define MPTABLE   0x9fc00                                // where the MP table goes
#define KERNEL    0x100000                               // 1 MiB: where the kernel goes
#define VIRTIO    0xd0000000                             // the virtio console's 4 KiB of registers

static unsigned char *ram;                               // guest RAM, mapped in section 2

static void die(const char *msg) { fputs(msg, stderr); exit(1); }

static long ck(long ret, const char *what) {             // stop with a message if a call failed;
  if (ret < 0) { perror(what); exit(1); }                //   every call here fails with -1
  return ret;
}

// One KVM call: an ioctl on a KVM file descriptor, checked; a failure prints the request name.
#define IOCTL(fd, request, arg) ck(ioctl(fd, request, arg), #request)

static void *map(int fd, size_t size,                    // mmap(), checked: size bytes of fd,
                 int prot, int flags) {                  //   or fresh memory when fd is -1
  void *p = mmap(0, size, prot, flags, fd, 0);
  return (void *)ck((long)p, "mmap");                    // MAP_FAILED is -1, so ck() sees it
}

static unsigned char *load(const char *path,             // a whole file, mapped read-only,
                           size_t *size) {               //   and its size
  int fd = ck(open(path, O_RDONLY), path);
  struct stat st;
  ck(fstat(fd, &st), path);
  *size = st.st_size;
  return map(fd, *size, PROT_READ, MAP_PRIVATE);
}

static unsigned char sum(unsigned char *p, int n) {      // byte sum; an MP checksum makes it 0
  unsigned char s = 0;
  while (n--) s += *p++;
  return s;
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
static unsigned char com1(int vm, int reg, int out, unsigned char v) {
  static unsigned char ier, lcr, thre;                   // thre: a "THR empty" event not yet acked
  unsigned char r = 0;
  if (lcr & 0x80 && reg < 2) return 0;                   // DLAB: the baud divisor; no need
  if (out) {                                             //   to emulate it
    if (reg == 0) { putchar(v); thre = 1; }
    if (reg == 1) { thre |= v & ~ier & 2; ier = v; }
    if (reg == 3) lcr = v;
  } else {
    if (reg == 1) r = ier;
    if (reg == 2) r = ier & 2 && thre ? 2 : 1;
    if (reg == 2 && r == 2) thre = 0;
    if (reg == 3) r = lcr;
    if (reg == 5) r = 0x60;
  }
  struct kvm_irq_level irq = {.irq = 4, .level = ier & 2 && thre};
  IOCTL(vm, KVM_IRQ_LINE, &irq);                         // IRQ 4, up or down
  return r;
}

// The virtio console: the console Linux calls hvc0, at 0xd0000000 with IRQ 5 (section 4 names both
// on the command line). All output after the first boot lines goes through it.
//
//   function         called by                    what it does
//   virtio_start()   section 1, once              gives KVM two eventfds, starts console()
//   console()        a thread of its own          prints what the guest puts on queue 1
//   virtio_read()    section 8, per MMIO read     returns a register's value
//   virtio_write()   section 8, per MMIO write    stores a register, such as a queue's address
//
//   guest                       KVM                          vmmlite
//   setup registers   ------->  KVM_EXIT_MMIO          --->  virtio_read(), virtio_write()
//   kick queue 1      ------->  ioeventfd: wakes console(); the vCPU runs on
//   IRQ 5             <-------  irqfd                  <---  console() writes call
//
//   Firecracker, Cloud Hypervisor and libkrun wire devices on KVM the same way: the kick never
//   reaches section 8, and the data never crosses MMIO, since console() reads guest RAM itself.
//
static int kick, call;                                   // eventfds: guest -> console() -> guest
static unsigned status, fsel, sel;                       // STATUS, DEVICE_FEATURES_SEL, QUEUE_SEL
static _Atomic unsigned isr;                             // INTERRUPT_STATUS: console() sets bit 0,
                                                         //   the guest's IRQ 5 handler acks it
// Queue 1, the transmit queue, as the driver set it up: three areas in guest RAM.
//   desc   the descriptor table: num buffers, each {guest address, length, flags, next}
//   avail  driver -> device: idx counts the chains the driver has added, and ring[idx % num]
//          holds each chain's first descriptor, its head
//   used   device -> driver: idx counts the chains handed back, and ring[] holds their heads
//   next   how many heads console() has taken from avail; it chases avail->idx
static struct {
  unsigned num; unsigned short next;
  struct vring_desc *desc; struct vring_avail *avail; struct vring_used *used;
} q;

// console(): the device, on its own thread. read() sleeps until the guest kicks queue 1. Then, for
// each head the driver has added since, it writes the chain's buffers to stdout and puts the head
// in the used ring, which hands the buffers back. Last, it sets bit 0 of INTERRUPT_STATUS and
// writes call, so KVM raises IRQ 5. Linux's console driver spins on the used ring until its buffer
// comes back, so every line the guest prints waits for this thread.
static void *console(void *arg) {
  for (unsigned long n; read(kick, &n, 8) == 8;) {       // sleep until the guest kicks queue 1
    // acquire: after reading avail->idx, see the ring entries the driver wrote before bumping it
    while (q.next != __atomic_load_n(&q.avail->idx, __ATOMIC_ACQUIRE)) {
      unsigned short head = q.avail->ring[q.next++ % q.num];
      // walk the descriptor chain; a buffer at guest address addr is at ram + addr here
      for (unsigned short i = head;; i = q.desc[i].next) {
        fwrite(ram + q.desc[i].addr, 1, q.desc[i].len, stdout);
        if (!(q.desc[i].flags & VRING_DESC_F_NEXT)) break;
      }
      q.used->ring[q.used->idx % q.num].id = head;       // hand it back; len stays 0: nothing
                                                         //   was written into the buffers
      // release: the entry above becomes visible to the driver before the new used->idx does
      __atomic_store_n(&q.used->idx, q.used->idx + 1, __ATOMIC_RELEASE);
    }
    isr |= 1;                                            // "used buffers"
    if (write(call, &n, 8) != 8) break;                  // KVM injects IRQ 5
  }
  return arg;
}

// virtio_start(): connects the console to KVM, then starts console(). An eventfd is a counter in
// the kernel: a write adds to it, and a read waits until it is nonzero, returns it and resets it.
//   kick   KVM_IOEVENTFD: when the guest writes 1, queue 1's index, to QUEUE_NOTIFY, KVM adds 1
//          to kick and resumes the guest at once, with no exit to section 8. Other values still
//          exit, so queue 0's kick reaches virtio_write().
//   call   KVM_IRQFD: when console() writes to call, KVM raises IRQ 5 in the guest.
static void virtio_start(int vm) {
  kick = eventfd(0, 0), call = eventfd(0, 0);            // both counters start at 0
  struct kvm_ioeventfd io = {.addr = VIRTIO + VIRTIO_MMIO_QUEUE_NOTIFY, .len = 4,
      .fd = kick, .datamatch = 1, .flags = KVM_IOEVENTFD_FLAG_DATAMATCH};
  IOCTL(vm, KVM_IOEVENTFD, &io);                         // a kick of queue 1 wakes console()
  struct kvm_irqfd irq = {.fd = call, .gsi = 5};
  IOCTL(vm, KVM_IRQFD, &irq);                            // a write to call raises IRQ 5
  pthread_t thread;
  pthread_create(&thread, 0, console, 0);                // it sleeps until the first kick
}

// virtio_read(), virtio_write(): the registers. Section 8 calls them on the vCPU thread, once for
// each MMIO exit into the console's 4 KiB, with reg the offset in it:
//
//   reg    register             guest reads                guest writes
//   0x000  MAGIC_VALUE          "virt"                     -
//   0x004  VERSION              2: virtio 1.0 or later     -
//   0x008  DEVICE_ID            3: a console               -
//   0x010  DEVICE_FEATURES      fsel 0: 0, no features     -
//                               fsel 1: 1, bit 32, which
//                               is VIRTIO_F_VERSION_1
//   0x014  DEVICE_FEATURES_SEL  -                          fsel: which 32 feature bits to read
//   0x030  QUEUE_SEL            -                          sel: the queue the writes below set up
//   0x034  QUEUE_NUM_MAX        256: the largest queue     -
//   0x038  QUEUE_NUM            -                          q.num, if sel is 1
//   0x050  QUEUE_NOTIFY         -                          a kick; only queue 0's comes here
//   0x060  INTERRUPT_STATUS     isr                        -
//   0x064  INTERRUPT_ACK        -                          clears those bits of isr
//   0x070  STATUS               status                     status; 0 resets the device
//   0x080  QUEUE_DESC_LOW       -                          q.desc = ram + value, if sel is 1
//   0x090  QUEUE_AVAIL_LOW      -                          q.avail = ram + value, if sel is 1
//   0x0a0  QUEUE_USED_LOW       -                          q.used = ram + value, if sel is 1
//   other                       0                          ignored, such as the addresses' high
//                                                            halves: guest RAM is below 4 GiB
//
// Linux's virtio-mmio driver at boot, one MMIO exit per register access:
//   1. reads MAGIC_VALUE, VERSION and DEVICE_ID, then writes STATUS 0: a reset
//   2. adds the STATUS bits ACKNOWLEDGE and DRIVER, reading STATUS and writing it back each time
//   3. reads DEVICE_FEATURES with fsel 1 and 0, accepts VIRTIO_F_VERSION_1, adds FEATURES_OK,
//      and reads STATUS again to check that the device kept it
//   4. sets up queue 0 (input, which vmmlite ignores) and queue 1: QUEUE_SEL, QUEUE_NUM_MAX,
//      QUEUE_NUM, the three ring addresses, QUEUE_READY
//   5. adds DRIVER_OK: the device is live
// After that, only its IRQ 5 handler comes here: it reads INTERRUPT_STATUS and writes the value
// back to INTERRUPT_ACK.
static unsigned virtio_read(unsigned long reg) {
  switch (reg) {
  case VIRTIO_MMIO_MAGIC_VALUE:      return 0x74726976;  // "virt"
  case VIRTIO_MMIO_VERSION:          return 2;
  case VIRTIO_MMIO_DEVICE_ID:        return 3;           // a console
  case VIRTIO_MMIO_DEVICE_FEATURES:  return fsel == 1;   // bit 32: VIRTIO_F_VERSION_1
  case VIRTIO_MMIO_QUEUE_NUM_MAX:    return 256;
  case VIRTIO_MMIO_INTERRUPT_STATUS: return isr;
  case VIRTIO_MMIO_STATUS:           return status;
  }
  return 0;
}

static void virtio_write(unsigned long reg, unsigned v) {
  switch (reg) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL: fsel = v; break;
  case VIRTIO_MMIO_QUEUE_SEL:        sel = v; break;
  case VIRTIO_MMIO_INTERRUPT_ACK:    isr &= ~v; break;
  case VIRTIO_MMIO_STATUS:           status = v; break;
  }
  if (sel == 1) switch (reg) {                           // queue 1's size and ring addresses
  case VIRTIO_MMIO_QUEUE_NUM:        q.num = v; break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW:   q.desc = (void *)(ram + v); break;
  case VIRTIO_MMIO_QUEUE_AVAIL_LOW:  q.avail = (void *)(ram + v); break;
  case VIRTIO_MMIO_QUEUE_USED_LOW:   q.used = (void *)(ram + v); break;
  }
}

int main(int argc, char **argv) {
  if (argc != 3) die("usage: vmmlite bzImage initramfs\n");
  size_t ksize, isize;
  unsigned char *k = load(argv[1], &ksize);              // the kernel image (bzImage)
  unsigned char *initrd = load(argv[2], &isize);         // the initramfs archive (cpio)

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
  int kvm = ck(open("/dev/kvm", O_RDWR), "/dev/kvm");
  int vm = IOCTL(kvm, KVM_CREATE_VM, 0);
  IOCTL(vm, KVM_SET_TSS_ADDR, 0xfffbd000);               // 3 pages Intel VT-x needs
  IOCTL(vm, KVM_CREATE_IRQCHIP, 0);                      // PIC, IOAPIC, LAPICs in the kernel
  struct kvm_pit_config pit = {
      .flags = KVM_PIT_SPEAKER_DUMMY};                   // also emulate port 0x61
  IOCTL(vm, KVM_CREATE_PIT2, &pit);                      // the 8254 timer, in the kernel
  virtio_start(vm);                                      // the virtio console and its thread

  // ------------------------------------------------------------------------------------------
  // 2. Guest RAM: plain host memory that KVM maps at guest physical address 0.
  //
  //   vmmlite's virtual memory               guest physical memory
  //   ram ------------> +---------+          +---------+ <- 0x00000000
  //                     | 512 MiB | memslot 0| 512 MiB |
  //                     |         |<========>|         |    the same bytes: ram[X] is
  //                     |         |          |         |    guest physical address X
  //   ram + RAM_SIZE -> +---------+          +---------+ <- 0x20000000
  //
  ram = map(-1, RAM_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS                  // not backed by a file,
            | MAP_NORESERVE);                            //   no swap reserved up front
  struct kvm_userspace_memory_region mem = {
      .slot = 0, .guest_phys_addr = 0,
      .memory_size = RAM_SIZE,
      .userspace_addr = (unsigned long)ram};
  IOCTL(vm, KVM_SET_USER_MEMORY_REGION, &mem);           // now guest address X is ram[X]

  // ------------------------------------------------------------------------------------------
  // 3. The kernel. A bzImage is a boot sector, the 16-bit setup code, then the real kernel.
  //
  //   bzImage (k[], file offsets)                      guest RAM (ram[])
  //   0x000      +------------------------------+
  //              | boot sector (512 B)          |      skipped: real-mode code
  //   0x1f1      |   setup header begins -------|----> zero page, section 4
  //   0x200      +------------------------------+
  //              | setup code, setup_sects*512 B|      skipped: 16-bit code
  //   setup      +------------------------------+
  //              | decompressor, with its       |----> 0x100000 (KERNEL), copied here;
  //              | 64-bit entry at +0x200       |      the vCPU starts at 0x100200
  //              | + compressed vmlinux         |
  //   ksize      +------------------------------+
  //
  if (memcmp(k + 0x202, "HdrS", 4) || !(k[0x236] & 1))   // the header's magic; XLF_KERNEL_64
    die("vmmlite: need a bzImage with a 64-bit entry\n");
  size_t setup = ((k[0x1f1] ? k[0x1f1] : 4) + 1) * 512;  // setup_sects (0 means 4) + boot sector
  memcpy(ram + KERNEL, k + setup, ksize - setup);

  // ------------------------------------------------------------------------------------------
  // 4. The zero page: everything the kernel learns about this machine. Guest RAM afterwards:
  //
  //   guest physical  contents                              written by
  //   0x00000000 +-----------------------------------+
  //   0x00000500 | GDT                               |  vmmlite, section 6
  //   0x00007000 | zero page: struct boot_params     |  vmmlite, here; the kernel reads it
  //   0x00009000 | page tables: PML4, PDPT, PD       |  vmmlite, section 6
  //   0x00020000 | command line                      |  vmmlite, here
  //   0x0009fc00 +-----------------------------------+
  //              | MP table, then the legacy BIOS    |  vmmlite, section 5; not in the E820
  //              | hole                              |
  //   0x00100000 +-----------------------------------+
  //              | decompressor + compressed vmlinux |  vmmlite, section 3
  //              +-----------------------------------+
  //              | free: the guest unpacks vmlinux   |  the guest, during boot
  //              | here, then uses it as its memory  |
  //   initrd_at  +-----------------------------------+
  //              | initramfs (cpio archive)          |  vmmlite, here
  //   0x20000000 +-----------------------------------+  end of RAM
  //
  //   Zero page fields set here, as offsets from 0x7000; every other byte stays 0:
  //   +0x1e8  e820_entries    2                  +0x218  ramdisk_image  initrd_at
  //   +0x1f1  hdr             copied from file   +0x21c  ramdisk_size   isize
  //   +0x210  type_of_loader  0xff               +0x228  cmd_line_ptr   0x20000
  //   +0x2d0  e820_table      {0, 0x9fc00, RAM}, {1 MiB, RAM_SIZE - 1 MiB, RAM}
  //
  struct boot_params *bp = (void *)(ram + ZERO_PAGE);    // fresh RAM, so already all 0
  memcpy(&bp->hdr, k + 0x1f1,                            // start from the kernel's own
         0x202 + k[0x201] - 0x1f1);                      //   header; byte 0x201 sets its end
  bp->hdr.type_of_loader = 0xff;                         // "other"; 0 = Linux skips the initrd
  bp->hdr.cmd_line_ptr = CMDLINE;
  strcpy((char *)ram + CMDLINE,                          // log to COM1 until hvc0, the virtio
         "earlycon=uart8250,io,0x3f8 console=hvc0 "      //   console, takes over; reboot on a
         "panic=-1 virtio_mmio.device=4K@0xd0000000:5"); //   panic; the console's 4 KiB, IRQ 5
  unsigned long initrd_at = (RAM_SIZE - isize)           // the initramfs goes at the top
                            & ~0xfffUL;                  //   of RAM, 4 KiB aligned
  memcpy(ram + initrd_at, initrd, isize);
  bp->hdr.ramdisk_image = initrd_at;
  bp->hdr.ramdisk_size = isize;
  bp->e820_table[0] = (struct boot_e820_entry){
      .addr = 0, .size = 0x9fc00, .type = 1};            // usable RAM below the MP table
  bp->e820_table[1] = (struct boot_e820_entry){
      .addr = KERNEL, .size = RAM_SIZE - KERNEL,         // usable RAM from 1 MiB to the end
      .type = 1};                                        // (type 1 means usable RAM)
  bp->e820_entries = 2;

  // ------------------------------------------------------------------------------------------
  // 5. The MP table: how Linux finds the CPU, the IOAPIC, and the IOAPIC pin of each ISA IRQ.
  //    Linux searches the last KB below 640 KiB for "_MP_"; the E820 keeps it out of RAM.
  //
  //   mp[]  bytes    entry                        contents
  //   0     16       floating pointer "_MP_"      ---> the header, at 0x9fc10
  //   16    44       header "PCMP"                length, entry count, LAPIC at 0xfee00000
  //   60    20       CPU                          LAPIC 0, version 0x14, enabled, boot CPU
  //   80    8        bus                          bus 0: ISA
  //   88    8        IOAPIC                       IOAPIC 1, version 0x11, at 0xfec00000
  //   96    16 x 8   IRQ routes                   ISA IRQ n -> IOAPIC 1, pin n
  //   224   2 x 8    local IRQ routes             PIC (ExtINT) -> LINT0, NMI -> LINT1
  //
#define IRQ(n) 3, 0, 0, 0, 0, n, 1, n
  static unsigned char mp[] = {
      '_', 'M', 'P', '_', 0x10, 0xfc, 0x09, 0, 1, 4,
      [16] = 'P', 'C', 'M', 'P', [22] = 4, [50] = 21, 0, 0, 0, 0xe0, 0xfe,
      [60] = 0, 0, 0x14, 3,
      [80] = 1, 0, 'I', 'S', 'A', ' ', ' ', ' ',
      2, 1, 0x11, 1, 0, 0, 0xc0, 0xfe,
      IRQ(0), IRQ(1), IRQ(2), IRQ(3), IRQ(4), IRQ(5), IRQ(6), IRQ(7),
      IRQ(8), IRQ(9), IRQ(10), IRQ(11), IRQ(12), IRQ(13), IRQ(14), IRQ(15),
      4, 3, 0, 0, 0, 0, 0xff, 0,
      4, 1, 0, 0, 0, 0, 0xff, 1};
  mp[20] = sizeof mp - 16;                               // the header's table length,
  mp[23] = -sum(mp + 16, sizeof mp - 16);                //   and checksum
  mp[10] = -sum(mp, 16);                                 // the floating pointer's checksum
  memcpy(ram + MPTABLE, mp, sizeof mp);

  // ------------------------------------------------------------------------------------------
  // 6. Long mode needs a GDT and page tables in guest RAM, where the CPU reads them.
  //
  //   guest physical  table  entries
  //   0x0500          GDT    [0] null  [1] unused  [2] 0x10: 64-bit code  [3] 0x18: data
  //   0x9000          PML4   [0] ---> 0xa000
  //   0xa000          PDPT   [0] ---> 0xb000
  //   0xb000          PD     [n] ---> n * 2 MiB, for n < 512: virtual X = physical X below 1 GiB
  //
  unsigned long gdt[] = {0, 0, 0x00af9b000000ffff, 0x00cf93000000ffff};
  memcpy(ram + GDT, gdt, sizeof gdt);
  unsigned long *pt = (void *)(ram + PML4);              // PML4, PDPT, PD: 512 entries each
  pt[0] = (PML4 + 0x1000) | 3;                           // PML4[0] ---> PDPT; present, writable
  pt[512] = (PML4 + 0x2000) | 3;                         // PDPT[0] ---> PD
  for (unsigned long n = 0; n < 512; n++)
    pt[1024 + n] = n << 21 | 0x83;                       // PD[n] ---> a 2 MiB page (bit 7)

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
  int cpu = IOCTL(vm, KVM_CREATE_VCPU, 0);
  long run_size = IOCTL(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = map(cpu, run_size,               // map the vCPU fd itself,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED);                 //   shared with KVM
  struct kvm_cpuid2 *cpuid = calloc(1, sizeof *cpuid
      + 256 * sizeof cpuid->entries[0]);
  cpuid->nent = 256;                                     // KVM's maximum number of entries
  IOCTL(kvm, KVM_GET_SUPPORTED_CPUID, cpuid);            // fill it with what host + KVM offer
  for (unsigned i = 0; i < cpuid->nent; i++)             // in leaf 1, the feature bits:
    if (cpuid->entries[i].function == 1) {
      cpuid->entries[i].ebx &= 0x00ffffff;               //   APIC ID 0, not the host CPU's
      cpuid->entries[i].ecx |= 1u << 31 | 1u << 24;      //   set "in a VM" and "TSC deadline"
    }
  IOCTL(cpu, KVM_SET_CPUID2, cpuid);                     // the guest's CPUID answers come from here
  struct kvm_sregs s;                                    // segment and control registers
  IOCTL(cpu, KVM_GET_SREGS, &s);                         // start from the reset state
  struct kvm_segment seg = {                             // a flat segment:
      .base = 0, .limit = ~0u,                           //   all 4 GiB,
      .present = 1, .s = 1, .db = 1, .g = 1};            //   valid, code/data, 32-bit, 4 KiB units
  seg.type = 3;                                          // read/write data,
  seg.selector = 0x18;                                   //   selector __BOOT_DS,
  s.ds = s.es = s.fs = s.gs = s.ss = seg;                //   for every data segment
  seg.type = 11;                                         // execute/read code,
  seg.selector = 0x10;                                   //   selector __BOOT_CS,
  seg.db = 0, seg.l = 1;                                 //   64-bit,
  s.cs = seg;                                            //   for the code segment
  s.gdt.base = GDT, s.gdt.limit = sizeof gdt - 1;
  s.cr3 = PML4;                                          // the page tables
  s.cr4 |= 1 << 5;                                       // CR4.PAE
  s.cr0 |= 1u << 31 | 1;                                 // CR0.PG, CR0.PE: paging on
  s.efer |= 1 << 10 | 1 << 8;                            // EFER.LMA, EFER.LME: long mode
  IOCTL(cpu, KVM_SET_SREGS, &s);                         // apply
  struct kvm_regs r = {
      .rip = KERNEL + 0x200,                             // the 64-bit entry point
      .rsi = ZERO_PAGE,                                  // boot protocol: RSI = the zero page
      .rflags = 2};                                      // interrupts off (bit 1 is always set)
  IOCTL(cpu, KVM_SET_REGS, &r);

  // ------------------------------------------------------------------------------------------
  // 8. Run. KVM returns to us only when the guest touches a device we must emulate:
  //
  //   port         device                     guest      vmmlite
  //   0x3f8-0x3ff  COM1, a 16550 UART         IN / OUT   com1(): registers, IRQ 4
  //   0x64         i8042 keyboard controller  OUT 0xfe   guest reset: return 0
  //   0xd0000000   virtio console registers   MMIO       virtio_read(), virtio_write()
  //   other        any other port or MMIO     IN / OUT   reads 0, writes ignored
  //   A kick of the console's queue 1 never comes here: KVM wakes its thread instead.
  //
  for (;;) {
    IOCTL(cpu, KVM_RUN, 0);                              // run the guest until it exits
    unsigned char *io = (unsigned char *)run             // port I/O data bytes,
                        + run->io.data_offset;           //   inside the run pages
    int port = run->io.port;                             // the port, for an I/O exit
    if (run->exit_reason == KVM_EXIT_MMIO) {             // memory that is not RAM
      unsigned long reg = run->mmio.phys_addr - VIRTIO;  // an offset into the console's 4 KiB
      unsigned v = 0;
      if (run->mmio.is_write) {
        memcpy(&v, run->mmio.data, 4);
        if (reg < 0x1000) virtio_write(reg, v);
      } else {
        if (reg < 0x1000) v = virtio_read(reg);
        memset(run->mmio.data, 0, 8);                    // other addresses read 0
        memcpy(run->mmio.data, &v, 4);
      }
    } else if (run->exit_reason != KVM_EXIT_IO) {        // anything else, e.g. a triple fault
      fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
      return 1;
    } else if (port >= 0x3f8 && port <= 0x3ff) {         // COM1; an IN reads what com1()
      *io = com1(vm, port & 7, run->io.direction, *io);  //   returns, an OUT ignores it
    } else if (run->io.direction == KVM_EXIT_IO_IN) {    // the guest reads another port:
      memset(io, 0, run->io.size);                       //   it reads 0
    } else if (port == 0x64 && *io == 0xfe) {            // keyboard controller reset:
      return 0;                                          //   the guest rebooted; done
    }                                                    // any other port write: ignored
  }
}
