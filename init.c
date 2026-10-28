// init.c: PID 1 inside the VM. Print one line, then reboot, which ends the VM.
// Build: gcc -static -o init init.c && echo init | cpio -o -H newc > initramfs.cpio
//
//   guest root filesystem, all in RAM        where it comes from
//   /                                        the initramfs (cpio) vmmlite loaded
//   +-- init                                 this program; the kernel runs it as PID 1
//   +-- root/                                the kernel's built-in initramfs
//   +-- dev/                                 the kernel's built-in initramfs
//       +-- console                          fds 0, 1, 2: the console, hvc0 (virtio)
//
//   write(1) -> tty -> hvc0 -> virtqueue 1 -> vmmlite's console handler -> fwrite()

#include <sys/reboot.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
  const char line[] = "Hello from PID 1!\n";
  // stdout, fd 1: the console, hvc0
  if (write(STDOUT_FILENO, line, sizeof line - 1) < 0) return 1;
  tcdrain(STDOUT_FILENO);                                // wait until the console has sent it all
  reboot(RB_AUTOBOOT);                                   // i8042 reset on x86, PSCI on ARM64;
                                                         //   either ends vmmlite
}
