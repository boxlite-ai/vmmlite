// Include the same device and loader code used by the Windows program; omit only its host API.
#define VMMLITE_TEST
#include "../vmmlite.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The same preparation calls as main() sections 3-6, with a portable RAM allocation.
static const char *prepare_guest(struct memory ram, struct image kernel, struct image initrd) {
  struct boot_layout layout;
  const char *error = load_kernel(ram, kernel, initrd, &layout);
  if (error) return error;
  setup_zero_page(ram, kernel, initrd, layout);
  setup_mptable(ram);
  setup_page_tables(ram);
  return NULL;
}

struct observer {
  uint8_t output[256];
  size_t bytes;
  unsigned interrupts, vector, destination, delivery;
  bool logical, level, discard;
};
static bool output(void *context, const uint8_t *bytes, size_t size) {
  struct observer *observer = context;
  if (!observer->discard) {
    assert(size <= sizeof observer->output - observer->bytes);
    memcpy(observer->output + observer->bytes, bytes, size);
  }
  observer->bytes += size;
  return true;
}
static bool interrupt(void *context, unsigned vector, unsigned destination,
                      unsigned delivery, bool logical, bool level) {
  struct observer *observer = context;
  observer->interrupts++;
  observer->vector = vector; observer->destination = destination;
  observer->delivery = delivery; observer->logical = logical; observer->level = level;
  return true;
}
static void out(struct pc *pc, uint16_t port, uint8_t value) { pc_port(pc, port, 1, true, value); }
static uint8_t in(struct pc *pc, uint16_t port) { return (uint8_t)pc_port(pc, port, 1, false, 0); }
static void route(struct pc *pc, unsigned pin, uint32_t low, uint32_t high) {
  uint32_t select = 0x11 + pin * 2;
  assert(pc_mmio(pc, 0, true, &select)); assert(pc_mmio(pc, 0x10, true, &high));
  select--;
  assert(pc_mmio(pc, 0, true, &select)); assert(pc_mmio(pc, 0x10, true, &low));
}
static uint32_t ioapic_read(struct pc *pc, uint32_t select) {
  uint32_t value = 0;
  assert(pc_mmio(pc, 0, true, &select)); assert(pc_mmio(pc, 0x10, false, &value));
  return value;
}
static void pic_init(struct pc *pc) {
  out(pc, 0x20, 0x11); out(pc, 0x21, 0x20); out(pc, 0x21, 4); out(pc, 0x21, 1);
  out(pc, 0xa0, 0x11); out(pc, 0xa1, 0x28); out(pc, 0xa1, 2); out(pc, 0xa1, 1);
  out(pc, 0x21, 255); out(pc, 0xa1, 255);
}

static void loader(void) {
  struct memory ram = {calloc(1, 32u << 20), 32u << 20};
  assert(ram.bytes);
  uint8_t kernel[4096] = {0}, initrd[] = {1, 2, 3, 4, 5};
  kernel[0x201] = 0x62;
  store16(kernel + 0x1fe, 0xaa55); store32(kernel + 0x202, 0x53726448);
  store16(kernel + 0x206, 0x20c); kernel[0x211] = 1; kernel[0x234] = 1;
  store16(kernel + 0x236, 1); store32(kernel + 0x230, 2u << 20);
  store32(kernel + 0x22c, 0x37ffffff); store32(kernel + 0x238, 2048);
  store64(kernel + 0x258, 16u << 20); store32(kernel + 0x260, 8u << 20);
  kernel[2560] = 0xa5;
  struct image image = {kernel, sizeof kernel}, archive = {initrd, sizeof initrd};
  assert(prepare_guest(ram, image, archive) == NULL);
  uint8_t *zero = ram.bytes + ZERO_PAGE_START;
  assert(ram.bytes[KERNEL_START] == 0xa5);
  assert(zero[0x210] == 255 && zero[0x1e8] == 2);
  assert(le32(zero + 0x214) == KERNEL_START);
  assert(le32(zero + 0x228) == CMDLINE_START);
  assert(strstr((char *)ram.bytes + CMDLINE_START, "console=hvc0"));
  uint32_t location = le32(zero + 0x218);
  assert(location == ram.size - 4096 && le32(zero + 0x21c) == sizeof initrd);
  assert(!memcmp(ram.bytes + location, initrd, sizeof initrd));
  assert(le64(zero + 0x2d8) == MPTABLE_START && le64(zero + 0x2ec) == ram.size - KERNEL_START);
  assert(le64(ram.bytes + PML4_START) == 0xa003);
  assert(le64(ram.bytes + 0xb000 + 511 * 8) == (UINT64_C(511) << 21 | 0x83));
  assert(le64(ram.bytes + GDT_START + 16) == UINT64_C(0x00af9b000000ffff));
  uint8_t sum = 0;
  for (unsigned i = 0; i < 16; i++) sum = (uint8_t)(sum + ram.bytes[MPTABLE_START + i]);
  assert(!sum);
  for (unsigned i = 16; i < 240; i++) sum = (uint8_t)(sum + ram.bytes[MPTABLE_START + i]);
  assert(!sum && le16(ram.bytes + MPTABLE_START + 16 + 34) == 21);
  assert(ram.bytes[MPTABLE_START + 96 + 5 * 8 + 7] == VIRTIO_CONSOLE_IRQ);

  assert(prepare_guest(ram, (struct image){kernel, 100}, archive));
  kernel[0x1fe] = 0; assert(prepare_guest(ram, image, archive)); kernel[0x1fe] = 0x55;
  kernel[0x1f1] = 255; assert(prepare_guest(ram, image, archive)); kernel[0x1f1] = 0;
  store32(kernel + 0x230, 3); assert(prepare_guest(ram, image, archive)); store32(kernel + 0x230, 2u << 20);
  // Checking load_address + init_size alone would miss this preferred-address overlap.
  store64(kernel + 0x258, 30u << 20); assert(prepare_guest(ram, image, archive));
  store64(kernel + 0x258, 16u << 20);
  store32(kernel + 0x22c, 1); assert(prepare_guest(ram, image, archive));
  store32(kernel + 0x22c, 0x37ffffff);
  store32(kernel + 0x238, 3); assert(prepare_guest(ram, image, archive));
  assert(ram.bytes[KERNEL_START] == 0xa5); // Rejected images did not overwrite the loaded payload.
  free(ram.bytes);
}

static void controllers(void) {
  struct observer observer = {0};
  struct pc *pc = pc_create(output, interrupt, &observer);
  assert(pc && ioapic_read(pc, 1) == ((23u << 16) | 0x11));
  route(pc, 5, 0x45 | (1u << 15) | (1u << 11), 0x01000000);
  pc_irq(pc, 5, true); pc_irq(pc, 5, true);
  assert(observer.interrupts == 1 && observer.vector == 0x45);
  assert(observer.destination == 1 && observer.logical && observer.level);
  assert(ioapic_read(pc, 0x1a) & (1u << 14));
  pc_eoi(pc, 0x45); assert(observer.interrupts == 2);
  pc_irq(pc, 5, false); pc_eoi(pc, 0x45);
  assert(observer.interrupts == 2 && !(ioapic_read(pc, 0x1a) & (1u << 14)));
  route(pc, 0, 0x30 | (1u << 16), 0);
  pc_irq(pc, 0, true); route(pc, 0, 0x30, 0);
  assert(observer.interrupts == 2); // An edge while masked is lost, not replayed on unmask.
  pc_irq(pc, 0, false); pc_irq(pc, 0, true);
  assert(observer.interrupts == 3 && observer.vector == 0x30 && !observer.level);
  pc_destroy(pc);

  pc = pc_create(output, interrupt, &observer); assert(pc); pic_init(pc);
  pc_irq(pc, 4, true); assert(pc_pic_pending(pc) == -1);
  out(pc, 0x21, 0xce); // IRQ 0, 4, 5 enabled.
  assert(pc_pic_pending(pc) == 0x24); pc_pic_ack(pc);
  out(pc, 0x20, 0x0b); assert(in(pc, 0x20) == 0x10);
  pc_irq(pc, 5, true); assert(pc_pic_pending(pc) == -1);
  pc_irq(pc, 0, true); assert(pc_pic_pending(pc) == 0x20); pc_pic_ack(pc);
  out(pc, 0x20, 0x20); assert(pc_pic_pending(pc) == -1);
  out(pc, 0x20, 0x20); assert(pc_pic_pending(pc) == 0x25); pc_pic_ack(pc);
  out(pc, 0x20, 0x20); assert(pc_pic_pending(pc) == -1);
  out(pc, 0x21, 0xfb); out(pc, 0xa1, 0xfd);
  pc_irq(pc, 9, true); assert(pc_pic_pending(pc) == 0x29); pc_pic_ack(pc);
  out(pc, 0xa0, 0x0b); assert(in(pc, 0xa0) == 2);
  out(pc, 0x20, 0x0b); assert(in(pc, 0x20) == 4);
  out(pc, 0xa0, 0x20); out(pc, 0x20, 0x20);
  assert(pc_pic_pending(pc) == -1 && pc_error(pc) == NULL);
  pc_destroy(pc);
}

static void timer_and_uart(void) {
  struct observer observer = {0};
  struct pc *pc = pc_create(output, interrupt, &observer); assert(pc);
  route(pc, 0, 0x30, 0);
  pc_tick(pc, 0);
  out(pc, 0x43, 0x34); out(pc, 0x40, 100); out(pc, 0x40, 0);
  pc_tick(pc, 90000); assert(observer.interrupts == 1);
  pc_tick(pc, 90000); assert(observer.interrupts == 1);
  pc_tick(pc, 10000000); assert(observer.interrupts == 2); // Missed periods coalesced.
  out(pc, 0x61, 0); out(pc, 0x43, 0xb0); out(pc, 0x42, 0x34); out(pc, 0x42, 0x12);
  out(pc, 0x61, 1); assert(!(in(pc, 0x61) & 0x20));
  out(pc, 0x43, 0x80); // Latch channel 2's count.
  uint8_t low = in(pc, 0x42);
  pc_tick(pc, 20000000);
  uint8_t high = in(pc, 0x42);
  assert(((unsigned)high << 8 | low) == 0x1234);
  assert(in(pc, 0x61) & 0x20);
  out(pc, 0x43, 0xe8); // Latch channel 2's status, not its count.
  assert((in(pc, 0x42) & 0xb0) == 0xb0);

  route(pc, 4, 0x44, 0);
  out(pc, 0x3fb, 0x80); out(pc, 0x3f8, 12); out(pc, 0x3f9, 0);
  assert(in(pc, 0x3f8) == 12 && observer.bytes == 0);
  out(pc, 0x3fb, 3); out(pc, 0x3f9, 2);
  assert(in(pc, 0x3fa) == 2 && in(pc, 0x3fa) == 1);
  out(pc, 0x3f8, 'W'); assert(observer.bytes == 1 && observer.output[0] == 'W');
  assert(in(pc, 0x3fd) == 0x60);
  assert(pc_port(pc, 0xcfc, 4, false, 0) == UINT32_MAX);
  out(pc, 0x64, 0xfe); assert(pc_reset_requested(pc));
  assert(pc_error(pc) == NULL);
  pc_destroy(pc);
}

static void reg_write(struct console *console, unsigned offset, uint32_t value) {
  assert(console_register_write(console, offset, value));
}
static uint32_t reg_read(struct console *console, unsigned offset) {
  uint32_t value = 0;
  assert(console_register_read(console, offset, &value));
  return value;
}
static void negotiate(struct console *console) {
  reg_write(console, 0x70, 0); reg_write(console, 0x70, 3);
  reg_write(console, 0x24, 1); reg_write(console, 0x20, 1);
  reg_write(console, 0x70, 11); assert(reg_read(console, 0x70) == 11);
}
static void tx_queue(struct console *console) {
  reg_write(console, 0x30, 1); reg_write(console, 0x38, 8);
  reg_write(console, 0x80, 0x1000); reg_write(console, 0x90, 0x2000);
  reg_write(console, 0xa0, 0x3000); reg_write(console, 0x44, 1);
  reg_write(console, 0x70, 15);
}
static void descriptor(uint8_t *ram, unsigned index, uint64_t address, uint32_t size, uint16_t flags, uint16_t next) {
  uint8_t *entry = ram + 0x1000 + index * 16;
  store64(entry, address); store32(entry + 8, size);
  store16(entry + 12, flags); store16(entry + 14, next);
}

static void virtio(void) {
  struct memory ram = {calloc(1, 65536), 65536}; assert(ram.bytes);
  struct observer observer = {0};
  struct pc *pc = pc_create(output, interrupt, &observer); assert(pc);
  struct console *console = console_create(ram, pc, output, &observer); assert(console);
  assert(reg_read(console, 0) == 0x74726976 && reg_read(console, 8) == 3);
  reg_write(console, 0x14, 1); assert(reg_read(console, 0x10) == 1);
  reg_write(console, 0x70, 11); assert(!(reg_read(console, 0x70) & 8));
  negotiate(console); tx_queue(console);
  route(pc, 5, 0x45 | (1u << 15), 0);
  memcpy(ram.bytes + 0x7000, "Hello ", 6); memcpy(ram.bytes + 0x7100, "WHP!\n", 5);
  descriptor(ram.bytes, 0, 0x7000, 6, 1, 1); descriptor(ram.bytes, 1, 0x7100, 5, 0, 0);
  store16(ram.bytes + 0x2002, 1); store16(ram.bytes + 0x2004, 0);
  reg_write(console, 0x50, 1);
  assert(observer.bytes == 11 && !memcmp(observer.output, "Hello WHP!\n", 11));
  assert(le16(ram.bytes + 0x3002) == 1 && le32(ram.bytes + 0x3004) == 0);
  assert(le32(ram.bytes + 0x3008) == 0 && console_bytes(console) == 11);
  assert(reg_read(console, 0x60) == 1 && observer.interrupts == 1);
  reg_write(console, 0x64, 1); pc_eoi(pc, 0x45);
  assert(reg_read(console, 0x60) == 0 && observer.interrupts == 1);
  reg_write(console, 0x70, 0);
  assert(reg_read(console, 0x44) == 0 && reg_read(console, 0x60) == 0);

  // Fail before output/used-ring publication for each malformed descriptor boundary.
  const struct { uint64_t address; uint16_t flags, next, head, available; } bad[] = {
    {65536, 0, 0, 0, 1}, {0x7000, 2, 0, 0, 1}, {0x7000, 4, 0, 0, 1},
    {0x7000, 1, 0, 0, 1}, {0x7000, 0, 0, 8, 1}, {0x7000, 0, 0, 0, 9}
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    negotiate(console); tx_queue(console);
    descriptor(ram.bytes, 0, bad[i].address, 1, bad[i].flags, bad[i].next);
    store16(ram.bytes + 0x2002, bad[i].available); store16(ram.bytes + 0x2004, bad[i].head);
    store16(ram.bytes + 0x3002, 0);
    uint32_t kick = 1;
    assert(!console_register_write(console, 0x50, kick) && console_error(console));
    assert(observer.bytes == 11 && le16(ram.bytes + 0x3002) == 0);
  }
  negotiate(console);
  reg_write(console, 0x30, 1); reg_write(console, 0x38, 3);
  uint32_t ready = 1;
  assert(!console_register_write(console, 0x44, ready));
  negotiate(console); tx_queue(console);
  uint32_t replacement = 0x4000;
  assert(!console_register_write(console, 0x80, replacement));

  // Exercise 16-bit index wrap in the production queue, with interrupt suppression.
  negotiate(console); tx_queue(console); observer.discard = true;
  descriptor(ram.bytes, 0, 0x7000, 1, 0, 0);
  store16(ram.bytes + 0x2000, 1);
  for (unsigned i = 0; i < 65537; i++) {
    store16(ram.bytes + 0x2004 + (i % 8) * 2, 0);
    store16(ram.bytes + 0x2002, (uint16_t)(i + 1));
    reg_write(console, 0x50, 1);
  }
  assert(le16(ram.bytes + 0x3002) == 1 && observer.bytes == 65548);
  assert(reg_read(console, 0x60) == 0 && observer.interrupts == 1);
  console_destroy(console); pc_destroy(pc); free(ram.bytes);
}

static struct image read_image(const char *path) {
  FILE *file = fopen(path, "rb"); assert(file);
  assert(fseek(file, 0, SEEK_END) == 0);
  long size = ftell(file); assert(size > 0 && (unsigned long)size <= RAM_SIZE);
  assert(fseek(file, 0, SEEK_SET) == 0);
  uint8_t *bytes = malloc((size_t)size); assert(bytes);
  assert(fread(bytes, 1, (size_t)size, file) == (size_t)size);
  assert(fclose(file) == 0);
  return (struct image){bytes, (size_t)size};
}

int main(int argc, char **argv) {
  if (argc == 3) {
    struct image kernel = read_image(argv[1]), initrd = read_image(argv[2]);
    struct memory ram = {calloc(1, RAM_SIZE), RAM_SIZE}; assert(ram.bytes);
    const char *error = prepare_guest(ram, kernel, initrd);
    if (error) fprintf(stderr, "%s\n", error);
    else printf("guest image loaded: entry=0x%x initrd=0x%x bytes=%u\n", KERNEL_ENTRY,
        le32(ram.bytes + ZERO_PAGE_START + 0x218), le32(ram.bytes + ZERO_PAGE_START + 0x21c));
    free(ram.bytes); free((void *)kernel.bytes); free((void *)initrd.bytes);
    return error != NULL;
  }
  assert(argc == 1);
  loader(); controllers(); timer_and_uart(); virtio();
  puts("loader, PIC/IOAPIC, PIT/UART and virtio contracts exercised");
  return 0;
}
