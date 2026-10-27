"""Run the actual KVM VMM and require Linux, hvc0, PID 1, and PSCI completion."""
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path('.build')
kernel = Path(sys.argv[1])
command = [str(build / 'vmmlite'), str(kernel), str(build / 'initramfs.cpio'), str(build / 'machine.dtb')]


def boot(initramfs, log_name, greeting):
    args = command.copy()
    args[2] = str(initramfs)
    try:
        result = subprocess.run(args, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        (build / log_name).write_bytes((error.stdout or b'') + (error.stderr or b''))
        raise AssertionError(f'Linux boot timed out; see .build/{log_name}') from error
    output = (result.stdout + result.stderr).decode(errors='replace')
    (build / log_name).write_text(output)
    assert result.returncode == 0, output[-5000:]
    for marker in ('Linux version', 'ttyS0 at MMIO 0x9000000', 'console [hvc0] enabled', 'Run /init as init process',
                   greeting, 'guest requested PSCI shutdown/reset'):
        assert marker in output, f'Missing {marker!r}:\n{output[-5000:]}'
    assert 'Kernel panic' not in output, output[-5000:]
    assert output.count(greeting) == 1, 'Guest greeting duplicated'
    assert output.index('bootconsole [uart8250] enabled') < output.index('console [hvc0] enabled')
    assert output.index('bootconsole [uart8250] disabled') < output.index(greeting)
    return output


for iteration in range(3):
    boot(build / 'initramfs.cpio', f'boot-{iteration}.log', 'Hello from PID 1!')
output = boot(build / 'timer.cpio', 'timer.log', 'Timer wakeup passed')
lines = [line for line in output.splitlines() if line.startswith('console iteration ')]
assert lines == [f'console iteration {iteration:03d}' for iteration in range(300)]
(build / 'boot.log').write_bytes((build / 'boot-0.log').read_bytes())

# Malformed real kernel headers are rejected by the executable before entering Linux.
header = bytearray(kernel.read_bytes()[:64])
mutations = [('magic', 56, b'nope', 'ARM64 Image'),
             ('zero-size', 16, bytes(8), 'little-endian Image'),
             ('endianness', 24, (1).to_bytes(8, 'little'), 'little-endian Image'),
             ('overlap', 8, (0x08000000).to_bytes(8, 'little'), 'overlaps device tree')]
with tempfile.TemporaryDirectory() as directory:
    for name, offset, replacement, message in mutations:
        data = header.copy()
        data[offset:offset + len(replacement)] = replacement
        path = Path(directory) / name
        path.write_bytes(data)
        result = subprocess.run([command[0], str(path), *command[2:]], capture_output=True, timeout=5)
        assert result.returncode == 1 and message in result.stderr.decode(), result
    for name, size in [('empty', 0), ('oversized', 0x08000001)]:
        path = Path(directory) / name
        with path.open('wb') as file:
            file.truncate(size)
        result = subprocess.run([command[0], str(path), *command[2:]], capture_output=True, timeout=5)
        assert result.returncode == 1 and b'input empty, too large' in result.stderr, result
    bad_dtb = Path(directory) / 'bad.dtb'
    bad_dtb.write_bytes(b'invalid device tree')
    result = subprocess.run([*command[:3], str(bad_dtb)], capture_output=True, timeout=5)
    assert result.returncode == 1 and b'FDT_ERR_' in result.stderr, result
print('PASS: repeated Linux/hvc0/PID 1 boots, timer wakeup, 300 console writes, malformed inputs')
