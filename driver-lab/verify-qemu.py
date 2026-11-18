#!/usr/bin/env python3
"""Boot the built image and verify an ARM64 external module in a disposable VM."""
from pathlib import Path
import pexpect

lab = Path(__file__).resolve().parent
logs = lab / 'logs'
logs.mkdir(exist_ok=True)
with (logs / 'qemu-envcheck.log').open('w') as log:
    child = pexpect.spawn('/bin/bash', [str(lab / 'start-qemu.sh')],
                          encoding='utf-8', codec_errors='replace', timeout=180)
    child.logfile_read = log
    try:
        child.expect('login:')
        child.sendline('root')
        child.expect(r'# ')
        child.sendline("export PS1='ENVTEST> '")
        child.expect(r'\r\nENVTEST> ')

        def check(command, marker):
            child.sendline(command + " && printf '\\n" + marker + "\\n'")
            # BusyBox's console may add one or more blank lines before output.
            # Match the marker itself instead of assuming an exact line layout.
            child.expect(marker)
            child.expect('ENVTEST> ')

        check('uname -a; test "$(uname -m)" = aarch64', 'ARCH_OK')
        check('insmod /root/envcheck.ko', 'INSMOD_OK')
        check("grep '^envcheck ' /proc/modules", 'MODULE_PRESENT')
        check("dmesg | grep 'envcheck: ARM64 driver environment loaded successfully'", 'LOAD_LOG_OK')
        check('rmmod envcheck', 'RMMOD_OK')
        check("! grep -q '^envcheck ' /proc/modules", 'MODULE_ABSENT')
        check("dmesg | grep 'envcheck: module unloaded successfully'", 'UNLOAD_LOG_OK')
        child.sendline('poweroff')
        child.expect(pexpect.EOF, timeout=60)
        child.close()
        if child.exitstatus != 0:
            raise RuntimeError(f'QEMU exit status: {child.exitstatus}, signal: {child.signalstatus}')
        print('PASS: ARM64 boot, module load/unload, and clean shutdown')
        print(logs / 'qemu-envcheck.log')
    finally:
        if child.isalive():
            child.terminate(force=True)
