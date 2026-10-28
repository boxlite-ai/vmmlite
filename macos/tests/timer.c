// A real guest: the host test requires elapsed guest time and repeated console output.
#include <errno.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  struct timespec before, after, remaining = {.tv_nsec = 250000000};
  if (clock_gettime(CLOCK_MONOTONIC, &before)) return 1;
  while (nanosleep(&remaining, &remaining)) if (errno != EINTR) return 2;
  if (clock_gettime(CLOCK_MONOTONIC, &after)) return 3;
  double elapsed = after.tv_sec - before.tv_sec + (after.tv_nsec - before.tv_nsec) / 1e9;
  if (elapsed < 0.20 || elapsed > 5) return 4;
  for (unsigned i = 0; i < 300; i++) {
    char line[40];
    int size = snprintf(line, sizeof line, "console iteration %03u\n", i);
    if (write(1, line, size) != size) return 5;
  }
  if (write(1, "Timer wakeup passed\n", 20) != 20) return 6;
  return reboot(RB_AUTOBOOT) ? 7 : 0;
}
