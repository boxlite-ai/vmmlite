// Exercise production parsing/MMIO; stub only the kernel IRQ boundary.
#define ioctl test_ioctl
#define main vmmlite_main
#include "../vmmlite.c"
#undef main
#undef ioctl
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/wait.h>

static bool uart_level;
int test_ioctl(int fd, unsigned long request, ...) {
  (void)fd;
  va_list args;
  va_start(args, request);
  void *argument = (void *)va_arg(args, uintptr_t);
  va_end(args);
  if (request == KVM_SET_GSI_ROUTING) {
    struct kvm_irq_routing *r = argument;
    assert(r->nr == 1 && r->entries[0].gsi == 1 && r->entries[0].u.irqchip.pin == 1);
    assert(r->entries[0].type == KVM_IRQ_ROUTING_IRQCHIP);
    return 0;
  }
  if (request == KVM_IRQFD) {
    struct kvm_irqfd *r = argument;
    assert(r->gsi == 1 && !r->flags && r->fd == (unsigned)irq_fd);
    return 0;
  }
  if (request == KVM_IOEVENTFD) {
    struct kvm_ioeventfd *r = argument;
    assert(r->addr == CONSOLE_BASE + VIRTIO_MMIO_QUEUE_NOTIFY && r->len == 4 && r->datamatch == 1);
    assert(r->flags == KVM_IOEVENTFD_FLAG_DATAMATCH && r->fd == kick_fd);
    return 0;
  }
  assert(request == KVM_IRQ_LINE);
  struct kvm_irq_level *irq = argument;
  assert(irq->irq == (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT | UART_IRQ));
  uart_level = irq->level;
  return 0;
}

static void must_fail(void (*action)(void), const char *message) {
  int pipefd[2], status;
  assert(!pipe(pipefd));
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    close(pipefd[0]);
    assert(dup2(pipefd[1], STDERR_FILENO) >= 0);
    close(pipefd[1]);
    action();
    _exit(99);
  }
  close(pipefd[1]);
  char error[1024] = {0};
  assert(read(pipefd[0], error, sizeof error - 1) > 0);
  close(pipefd[0]);
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
  assert(strstr(error, message));
}

static void set_address(unsigned reg, uint64_t address) {
  console_register_write(reg, address);
  console_register_write(reg + 4, address >> 32);
}

static void setup_queue(void) {
  console_register_write(0x070, 0);
  console_register_write(0x024, 1);
  console_register_write(0x020, 1);
  console_register_write(0x070, 15);
  assert(console_register_read(0x070) == 15);
  console_register_write(0x030, 1);
  console_register_write(0x038, 4);
  set_address(0x080, RAM_BASE + 0x1000);
  set_address(0x090, RAM_BASE + 0x2000);
  set_address(0x0a0, RAM_BASE + 0x3000);
  memset(guest(RAM_BASE + 0x1000, 0x3000), 0, 0x3000);
  console_register_write(0x044, 1);
  assert(console_register_read(0x044) == 1);
}

static void bad_guest(void) { guest(UINT64_MAX, 16); }
static void underflow(void) { guest(RAM_BASE - 1, 1); }
static void overrun(void) { guest(RAM_BASE + RAM_SIZE - 1, 2); }
static void bad_size(void) { console.transmit.size = 3; queue_check(&console.transmit); }
static void zero_size(void) { console.transmit.size = 0; queue_check(&console.transmit); }
static void huge_size(void) { console.transmit.size = 512; queue_check(&console.transmit); }
static void unaligned(void) { console.transmit.desc++; queue_check(&console.transmit); }
static void high_address(void) { console.transmit.desc = UINT64_C(0x100000000); queue_check(&console.transmit); }
static void kick(void) { console_kick(1); }
static void bad_uart_width(void) {
  struct kvm_run run = {0};
  run.mmio.phys_addr = UART_BASE; run.mmio.len = 4;
  mmio(&run);
}
static void bad_console_alignment(void) {
  struct kvm_run run = {0};
  run.mmio.phys_addr = CONSOLE_BASE + 1; run.mmio.len = 4;
  mmio(&run);
}
static void bad_console_width(void) {
  struct kvm_run run = {0};
  run.mmio.phys_addr = CONSOLE_BASE; run.mmio.len = 8;
  mmio(&run);
}

static void transmit_test(void) {
  setup_queue();
  struct descriptor *d = guest(console.transmit.desc, 64);
  uint16_t *available = guest(console.transmit.avail, 14);
  uint16_t *used = guest(console.transmit.used, 38);
  memcpy(guest(RAM_BASE + 0x5000, 12), "hello queue\n", 12);
  d[0] = (struct descriptor){RAM_BASE + 0x5000, 6, 1, 1};
  d[1] = (struct descriptor){RAM_BASE + 0x5006, 6, 0, 0};
  available[1] = 1;
  eventfd_t notification;
  FILE *capture = tmpfile();
  assert(capture);
  int saved = dup(STDOUT_FILENO);
  assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
  console_kick(1);
  assert(dup2(saved, STDOUT_FILENO) >= 0);
  close(saved);
  rewind(capture);
  char bytes[13] = {0};
  assert(fread(bytes, 1, 12, capture) == 12 && !memcmp(bytes, "hello queue\n", 12));
  fclose(capture);
  assert(used[1] == 1 && console.transmit.next == 1);
  uint32_t completion[2];
  memcpy(completion, (char *)used + 4, sizeof completion);
  assert(completion[0] == 0 && completion[1] == 0);
  assert(console_register_read(0x060) == 1);
  assert(!eventfd_read(irq_fd, &notification) && notification == 1);
  console_register_write(0x064, 1);
  assert(console_register_read(0x060) == 0);

  // Both 16-bit counters wrap, and the used-ring slot wraps independently.
  console.transmit.next = UINT16_MAX;
  available[1] = 0; available[5] = 1; used[1] = UINT16_MAX;
  d[1].length = 0;
  console_kick(1);
  assert(console.transmit.next == 0 && used[1] == 0);
  memcpy(completion, (char *)used + 4 + 8 * 3, sizeof completion);
  assert(completion[0] == 1);
}

static void uart_test(void) {
  assert(com1(UART_LSR, 0, 0) == (UART_LSR_TEMT | UART_LSR_THRE));
  assert(com1(UART_TX, 0, 0) == 0); // no input
  com1(UART_IER, 1, UART_IER_THRI);
  assert(uart_level);
  assert(com1(UART_IIR, 0, 0) == UART_IIR_THRI && !uart_level);
  assert(com1(UART_IIR, 0, 0) == UART_IIR_NO_INT);
  FILE *capture = tmpfile();
  assert(capture);
  int saved = dup(STDOUT_FILENO);
  assert(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
  com1(UART_LCR, 1, UART_LCR_DLAB);
  assert(com1(UART_LCR, 0, 0) == UART_LCR_DLAB);
  com1(UART_TX, 1, 'X'); // divisor write: must not print
  com1(UART_IER, 1, 0); // divisor high: must not disable THRI
  com1(UART_LCR, 1, 0);
  assert(com1(UART_IER, 0, 0) == UART_IER_THRI);
  com1(UART_TX, 1, 'K');
  assert(uart_level);
  assert(dup2(saved, STDOUT_FILENO) >= 0);
  close(saved);
  rewind(capture);
  assert(fgetc(capture) == 'K' && fgetc(capture) == EOF);
  fclose(capture);
  com1(UART_IER, 1, 0);
  assert(!uart_level);
}

static void receive_ignored_test(void) {
  setup_queue();
  struct queue before = console.transmit;
  console_register_write(VIRTIO_MMIO_QUEUE_SEL, 0);
  assert(console_register_read(VIRTIO_MMIO_QUEUE_NUM_MAX) == QUEUE_MAX);
  console_register_write(VIRTIO_MMIO_QUEUE_NUM, 512);
  set_address(VIRTIO_MMIO_QUEUE_DESC_LOW, UINT64_MAX);
  console_register_write(VIRTIO_MMIO_QUEUE_READY, 1);
  console_kick(0);
  assert(!memcmp(&before, &console.transmit, sizeof before));
  console_register_write(VIRTIO_MMIO_QUEUE_SEL, 1);
  assert(console_register_read(VIRTIO_MMIO_QUEUE_READY) == 1);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  setbuf(stdout, NULL);
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  assert(ram != MAP_FAILED);
  size_t size = load(argv[1], ram, DTB_BASE - RAM_BASE);
  uint64_t offset;
  memcpy(&offset, ram + 8, 8);
  assert(kernel_entry(size) == RAM_BASE + offset);
  must_fail(bad_guest, "outside RAM"); must_fail(underflow, "outside RAM"); must_fail(overrun, "outside RAM");
  must_fail(bad_uart_width, "byte accesses");
  must_fail(bad_console_alignment, "address/width"); must_fail(bad_console_width, "address/width");
  assert(guest(RAM_BASE + RAM_SIZE, 0) == ram + RAM_SIZE);
  assert(console_register_read(0x000) == 0x74726976 && console_register_read(0x008) == 3);
  console_register_write(0x070, 11); // VERSION_1 has not been accepted.
  assert(!(console_register_read(0x070) & 8));
  console_register_write(0x030, 2); assert(!console_register_read(0x034));
  setup_queue();
  must_fail(bad_size, "queue size"); must_fail(zero_size, "queue size"); must_fail(huge_size, "queue size");
  must_fail(unaligned, "unaligned"); must_fail(high_address, "outside RAM");
  connect_console();
  uart_test();
  receive_ignored_test();
  transmit_test();
  setup_queue();
  uint16_t *available = guest(console.transmit.avail, 14);
  struct descriptor *d = guest(console.transmit.desc, 64);
  available[1] = 5; must_fail(kick, "ring overrun");
  available[1] = 1; available[2] = 4; must_fail(kick, "descriptor chain");
  available[2] = 0; d[0] = (struct descriptor){RAM_BASE, 0, 1, 0};
  must_fail(kick, "cyclic descriptor chain");
  d[0].flags = 2; must_fail(kick, "readable and direct");
  d[0].flags = 4; must_fail(kick, "readable and direct");
  d[0].flags = 0; d[0].address = UINT64_MAX; d[0].length = 1; must_fail(kick, "outside RAM");
  console_register_write(0x070, 0);
  assert(!console.status && !console.transmit.ready && !console.transmit.desc);
  console_kick(1); // A notification queued before reset must be harmless.
  struct kvm_run run = {0};
  run.mmio.phys_addr = CONSOLE_BASE; run.mmio.len = 4;
  mmio(&run);
  uint32_t value;
  memcpy(&value, run.mmio.data, sizeof value);
  assert(value == 0x74726976);
  pthread_t worker;
  assert(!pthread_create(&worker, NULL, console_thread, NULL));
  event_signal(kick_fd); event_signal(stop_fd);
  assert(!pthread_join(worker, NULL));
  close(kick_fd); close(irq_fd); close(stop_fd);
  munmap(ram, RAM_SIZE);
  puts("PASS: Image placement, guest bounds, UART IRQ/DLAB, transmit-only virtqueue, wrap/reset and malformed descriptors");
}
