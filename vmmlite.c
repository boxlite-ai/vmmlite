// vmmlite.c: the smallest VMM that boots Linux (x86_64, KVM). For learning only.
// Build: gcc -O2 -o vmmlite vmmlite.c      Run: ./vmmlite bzImage initramfs.cpio
//
//   vmmlite (this process)       KVM (host kernel)               CPU (VT-x / AMD-V)
//   +--------------------+ ioctl +-----------------------+ entry +--------------------+
//   | ram: guest RAM     |------>| VM: memslot, PIC,     |------>| runs guest code    |
//   | run: kvm_run page  |<------| IOAPIC, PIT; vCPU     |<------| until a VM exit    |
//   +--------------------+ exits +-----------------------+ exit  +--------------------+

#include <asm/bootparam.h>                               // struct boot_params: the zero page
#include <fcntl.h>
#include <linux/kvm.h>                                   // KVM's ioctl numbers and structs
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>                                   // ioctl(): how every KVM call is made
#include <sys/mman.h>                                    // mmap(): files, guest RAM, run page
#include <sys/stat.h>

#define RAM_SIZE  (512UL << 20)                          // guest RAM: 512 MiB at address 0
#define GDT       0x500                                  // where the boot GDT goes
#define ZERO_PAGE 0x7000                                 // where struct boot_params goes
#define PML4      0x9000                                 // where the 3 pages of page tables go
#define CMDLINE   0x20000                                // where the command line goes
#define MPTABLE   0x9fc00                                // where the MP table goes
#define KERNEL    0x100000                               // 1 MiB: where the kernel goes

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
  //   The kernel handles guest accesses to these; they never reach the loop in section 8.
  //
  int kvm = ck(open("/dev/kvm", O_RDWR), "/dev/kvm");
  int vm = IOCTL(kvm, KVM_CREATE_VM, 0);
  IOCTL(vm, KVM_SET_TSS_ADDR, 0xfffbd000);               // 3 pages Intel VT-x needs
  IOCTL(vm, KVM_CREATE_IRQCHIP, 0);                      // PIC, IOAPIC, LAPICs in the kernel
  struct kvm_pit_config pit = {
      .flags = KVM_PIT_SPEAKER_DUMMY};                   // also emulate port 0x61
  IOCTL(vm, KVM_CREATE_PIT2, &pit);                      // the 8254 timer, in the kernel

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
  unsigned char *ram = map(-1, RAM_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS   // not backed by a file,
                           | MAP_NORESERVE);             //   no swap reserved up front
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
  //   0x00020000 | "console=ttyS0 panic=-1\0"        |  vmmlite, here
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
  strcpy((char *)ram + CMDLINE,                          // log to COM1,
         "console=ttyS0 panic=-1");                      //   and reboot on a panic
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
  //   other        any other port or MMIO     IN / OUT   reads 0, writes ignored
  //
  for (;;) {
    IOCTL(cpu, KVM_RUN, 0);                              // run the guest until it exits
    unsigned char *io = (unsigned char *)run             // port I/O data bytes,
                        + run->io.data_offset;           //   inside the run pages
    int port = run->io.port;                             // the port, for an I/O exit
    if (run->exit_reason == KVM_EXIT_MMIO) {             // memory that is not RAM:
      if (!run->mmio.is_write)
        memset(run->mmio.data, 0, 8);                    //   reads see 0, writes are dropped
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
