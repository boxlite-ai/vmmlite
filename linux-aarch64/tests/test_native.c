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
  assert(request == KVM_IRQ_LINE);
  va_list args;
  va_start(args, request);
  struct kvm_irq_level *irq = (void *)va_arg(args, uintptr_t);
  va_end(args);
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


static void bad_guest(void) { guest(UINT64_MAX, 16); }
static void underflow(void) { guest(RAM_BASE - 1, 1); }
static void overrun(void) { guest(RAM_BASE + RAM_SIZE - 1, 2); }
static void bad_mmio(void) {
  struct kvm_run run = {0};
  run.mmio.phys_addr = UART_BASE; run.mmio.len = 4;
  mmio(&run);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  setbuf(stdout, NULL);
  ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(ram != MAP_FAILED);
  size_t size = load(argv[1], ram, DTB_BASE - RAM_BASE);
  uint64_t offset;
  memcpy(&offset, ram + 8, 8);
  assert(kernel_entry(size) == RAM_BASE + offset);
  must_fail(bad_guest, "outside RAM"); must_fail(underflow, "outside RAM"); must_fail(overrun, "outside RAM");
  must_fail(bad_mmio, "address/width");
  assert(guest(RAM_BASE + RAM_SIZE, 0) == ram + RAM_SIZE);
  uart_test();
  munmap(ram, RAM_SIZE);
  puts("PASS: Image placement, guest bounds, UART IRQ/DLAB and MMIO width");
}
