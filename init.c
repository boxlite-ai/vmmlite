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
//   write(1) -> tty -> hvc0 -> virtqueue 1 -> vmmlite's console thread -> fwrite()

#include <sys/reboot.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
  if (write(1, "Hello from PID 1!\n", 18) < 0) return 1; // fd 1: the console, hvc0
  tcdrain(1);                                            // wait until the console has sent it all
  reboot(RB_AUTOBOOT);                                   // restart: Linux pulses the i8042
                                                         //   reset line, and vmmlite exits
}
