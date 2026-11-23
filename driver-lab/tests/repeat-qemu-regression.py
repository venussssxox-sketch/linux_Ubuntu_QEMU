#!/usr/bin/env python3
"""Bounded regression in a disposable ARM64 VM; preserve evidence on failure."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid
import pexpect

lab = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rounds', type=int, default=10)
parser.add_argument('--timeout', type=int, default=120, help='seconds per round')
parser.add_argument('--label', default='regression')
args = parser.parse_args()
if args.rounds < 1 or args.timeout < 1 or not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
    parser.error('positive rounds/timeout and a simple alphanumeric label required')
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out = lab / 'logs' / (stamp + '-' + args.label)
out.mkdir(exist_ok=False)
files = sorted((lab / 'rootfs-overlay/root').glob('kqueue_*'))
files.append(lab / 'rootfs-overlay/root/run-kqueue-regression.sh')
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
report = {'started_utc': stamp, 'requested_rounds': args.rounds,
          'timeout_seconds': args.timeout, 'rounds': [], 'deployed_sha256': hashes,
          'git_head': subprocess.check_output(['git','rev-parse','HEAD'], cwd=lab, text=True).strip(),
          'source_sha256': {str(p.relative_to(lab)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in [lab/'drivers/kqueue_lab/kqueue_lab.c', *sorted((lab/'userspace').glob('*.c'))]},
          'success': False}
start = time.monotonic()
child = None
try:
    with (out/'serial.log').open('w') as serial:
        child = pexpect.spawn('/bin/bash', [str(lab/'start-qemu.sh')], encoding='utf-8',
                              codec_errors='replace', timeout=180)
        child.logfile_read = serial
        child.expect('login:')
        child.sendline('root')
        child.expect(r'# ')
        child.sendline("stty -echo; export PS1='LABCHECK> '")
        child.expect(r'\r\nLABCHECK> ')

        def command(cmd, timeout=30):
            marker = 'DONE_' + uuid.uuid4().hex
            child.sendline(cmd + "; rc=$?; printf '\\n" + marker + ":%s\\n' \"$rc\"")
            child.expect(r'\r+\n' + marker + r':(\d+)\r+\n', timeout=timeout)
            output, code = child.before.replace('\r', ''), int(child.match.group(1))
            child.expect_exact('LABCHECK> ')
            return output, code

        env, code = command('uname -a; test "$(uname -m)" = aarch64')
        report['guest_environment'] = env.strip()
        if code: raise RuntimeError('guest is not ARM64')
        for name, digest in hashes.items():
            output, code = command('sha256sum ' + shlex.quote('/root/' + name))
            if code or digest not in output.split():
                raise RuntimeError('image/overlay mismatch: ' + name)
        # Keep noisy driver messages in dmesg instead of interleaving shell output.
        output, code = command('dmesg -n 4')
        if code: raise RuntimeError('could not set console log level')
        for number in range(1, args.rounds + 1):
            tick = time.monotonic()
            output, code = command('/root/run-kqueue-regression.sh', args.timeout)
            (out/f'round-{number:03}.log').write_text(output)
            passed = re.findall(r'^passed: (\d+)\r?$', output, re.M)
            failed = re.findall(r'^failed: (\d+)\r?$', output, re.M)
            ok = code == 0 and passed == ['11'] and failed == ['0'] and output.count('RESULT: PASS - ') == 11
            report['rounds'].append({'round': number, 'exit_code': code, 'passed': passed,
                                     'failed': failed, 'success': ok,
                                     'elapsed_seconds': round(time.monotonic()-tick, 3)})
            print(f'round {number}/{args.rounds}: {"PASS" if ok else "FAIL"}', flush=True)
            if not ok: raise RuntimeError(f'round {number} failed')
        output, code = command("if grep -q '^kqueue_lab ' /proc/modules; then exit_code=1; else exit_code=0; fi; test \"$exit_code\" = 0")
        if code: raise RuntimeError('module still loaded after regression')
        output, code = command('dmesg')
        (out/'dmesg.log').write_text(output)
        if code or re.search(r'BUG:|WARNING:|Oops:|Kernel panic|general protection fault', output):
            raise RuntimeError('kernel diagnostics require review')
        child.sendline('poweroff')
        child.expect(pexpect.EOF, timeout=60)
        child.close()
        if child.exitstatus != 0: raise RuntimeError('QEMU shutdown failed')
        report['success'] = True
except Exception as exc:
    report['error'] = f'{type(exc).__name__}: {exc}'
    print(report['error'], flush=True)
finally:
    if child is not None and child.isalive(): child.terminate(force=True)
    report['elapsed_seconds'] = round(time.monotonic()-start, 3)
    (out/'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out, flush=True)
raise SystemExit(0 if report['success'] else 1)
