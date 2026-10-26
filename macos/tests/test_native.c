// Exercise production parsing and virtqueue processing; stub only the host IRQ boundary.
#define hv_gic_set_spi test_set_spi
#define main vmmlite_main
#include "../vmmlite.c"
#undef main
#undef hv_gic_set_spi
#include <assert.h>
#include <sys/wait.h>

static unsigned raised, lowered;
hv_return_t test_set_spi(uint32_t interrupt, bool level) {
  assert(interrupt == CONSOLE_IRQ);
  if (level) raised++; else lowered++;
  return HV_SUCCESS;
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
  console_write(reg, address);
  console_write(reg + 4, address >> 32);
}

static void setup_queue(void) {
  console_write(0x070, 0);
  console_write(0x024, 1);
  console_write(0x020, 1);
  console_write(0x070, 15);
  assert(console_read(0x070) == 15);
  console_write(0x030, 1);
  console_write(0x038, 4);
  set_address(0x080, RAM_BASE + 0x1000);
  set_address(0x090, RAM_BASE + 0x2000);
  set_address(0x0a0, RAM_BASE + 0x3000);
  memset(guest(RAM_BASE + 0x1000, 0x3000), 0, 0x3000);
  console_write(0x044, 1);
  assert(console_read(0x044) == 1);
}

static void bad_guest(void) { guest(UINT64_MAX, 16); }
static void underflow(void) { guest(RAM_BASE - 1, 1); }
static void overrun(void) { guest(RAM_BASE + RAM_SIZE - 1, 2); }
static void bad_size(void) { console.queues[1].size = 3; queue_check(&console.queues[1]); }
static void zero_size(void) { console.queues[1].size = 0; queue_check(&console.queues[1]); }
static void huge_size(void) { console.queues[1].size = 512; queue_check(&console.queues[1]); }
static void unaligned(void) { console.queues[1].desc++; queue_check(&console.queues[1]); }
static void high_address(void) { console.queues[1].desc = UINT64_C(0x100000000); queue_check(&console.queues[1]); }
static void kick(void) { console_kick(1); }

static void transmit_test(void) {
  setup_queue();
  struct descriptor *d = guest(console.queues[1].desc, 64);
  uint16_t *available = guest(console.queues[1].avail, 14);
  uint16_t *used = guest(console.queues[1].used, 38);
  memcpy(guest(RAM_BASE + 0x5000, 12), "hello queue\n", 12);
  d[0] = (struct descriptor){RAM_BASE + 0x5000, 6, 1, 1};
  d[1] = (struct descriptor){RAM_BASE + 0x5006, 6, 0, 0};
  available[1] = 1;
  unsigned before = raised;
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
  assert(used[1] == 1 && console.queues[1].next == 1);
  uint32_t completion[2];
  memcpy(completion, (char *)used + 4, sizeof completion);
  assert(completion[0] == 0 && completion[1] == 0 && raised == before + 1);
  assert(console_read(0x060) == 1);
  unsigned clears = lowered;
  console_write(0x064, 1);
  assert(console_read(0x060) == 0 && lowered == clears + 1);

  // Both 16-bit counters wrap, and the used-ring slot wraps independently.
  console.queues[1].next = UINT16_MAX;
  available[1] = 0; available[5] = 1; used[1] = UINT16_MAX;
  d[1].length = 0;
  console_kick(1);
  assert(console.queues[1].next == 0 && used[1] == 0);
  memcpy(completion, (char *)used + 4 + 8 * 3, sizeof completion);
  assert(completion[0] == 1);
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
  assert(guest(RAM_BASE + RAM_SIZE, 0) == ram + RAM_SIZE);
  assert(console_read(0x000) == 0x74726976 && console_read(0x008) == 3);
  console_write(0x070, 11); // VERSION_1 has not been accepted.
  assert(!(console_read(0x070) & 8));
  console_write(0x030, 2); assert(!console_read(0x034));
  setup_queue();
  must_fail(bad_size, "queue size"); must_fail(zero_size, "queue size"); must_fail(huge_size, "queue size");
  must_fail(unaligned, "unaligned"); must_fail(high_address, "outside RAM");
  transmit_test();
  setup_queue();
  uint16_t *available = guest(console.queues[1].avail, 14);
  struct descriptor *d = guest(console.queues[1].desc, 64);
  available[1] = 5; must_fail(kick, "ring overrun");
  available[1] = 1; available[2] = 4; must_fail(kick, "descriptor chain");
  available[2] = 0; d[0] = (struct descriptor){RAM_BASE, 0, 1, 0};
  must_fail(kick, "cyclic descriptor chain");
  d[0].flags = 2; must_fail(kick, "readable and direct");
  d[0].flags = 4; must_fail(kick, "readable and direct");
  d[0].flags = 0; d[0].address = UINT64_MAX; d[0].length = 1; must_fail(kick, "outside RAM");
  console_write(0x070, 0);
  assert(!console.status && !console.queues[1].ready && !console.queues[1].desc);
  munmap(ram, RAM_SIZE);
  puts("PASS: Image placement, guest bounds, virtqueue completion/wrap/reset and malformed descriptors");
}
