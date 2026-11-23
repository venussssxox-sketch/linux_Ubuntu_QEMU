#!/usr/bin/env python3
"""Bounded regression in a disposable ARM64 VM; preserve evidence on failure."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid
import pexpect

lab = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rounds', type=int,
                    help='number of complete regression rounds (default: 10)')
parser.add_argument('--duration-hours', type=float,
                    help='run complete rounds until this test duration is reached')
parser.add_argument('--timeout', type=int, default=120, help='seconds per round')
parser.add_argument('--expected-tests', type=int, default=13,
                    help='expected PASS results in each complete round')
parser.add_argument('--images-dir', type=Path,
                    default=lab / 'buildroot-2025.02.19/output/images')
parser.add_argument('--deployment-dir', type=Path,
                    default=lab / 'rootfs-overlay/root')
parser.add_argument('--label', default='regression')
args = parser.parse_args()
if args.rounds is not None and args.duration_hours is not None:
    parser.error('--rounds and --duration-hours are mutually exclusive')
if args.rounds is None and args.duration_hours is None:
    args.rounds = 10
if ((args.rounds is not None and args.rounds < 1) or
        (args.duration_hours is not None and args.duration_hours <= 0) or
        args.timeout < 1 or args.expected_tests < 1 or
        not re.fullmatch(r'[A-Za-z0-9_-]+', args.label)):
    parser.error('positive duration/rounds/timeout/test count and a simple label required')
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out = lab / 'logs' / (stamp + '-' + args.label)
out.mkdir(exist_ok=False)
images_dir = args.images_dir.resolve()
deployment_dir = args.deployment_dir.resolve()
for required in (images_dir / 'Image', images_dir / 'rootfs.ext4',
                 deployment_dir / 'kqueue_lab.ko',
                 deployment_dir / 'run-kqueue-regression.sh'):
    if not required.exists():
        parser.error(f'missing required artifact: {required}')
files = sorted(deployment_dir.glob('kqueue_*'))
files.append(deployment_dir / 'run-kqueue-regression.sh')
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
report = {'started_utc': stamp, 'requested_rounds': args.rounds,
          'requested_duration_hours': args.duration_hours,
          'timeout_seconds': args.timeout, 'expected_tests': args.expected_tests,
          'rounds': [], 'deployed_sha256': hashes,
          'images_dir': str(images_dir),
          'deployment_dir': str(deployment_dir),
          'git_head': subprocess.check_output(['git','rev-parse','HEAD'], cwd=lab, text=True).strip(),
          'source_sha256': {str(p.relative_to(lab)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in [lab/'drivers/kqueue_lab/kqueue_lab.c', *sorted((lab/'userspace').glob('*.c'))]},
          'success': False}
start = time.monotonic()
child = None

def write_json(path):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)

try:
    with (out/'serial.log').open('w') as serial:
        guest_env = os.environ.copy()
        guest_env['KQUEUE_IMAGES_DIR'] = str(images_dir)
        child = pexpect.spawn('/bin/bash', [str(lab/'start-qemu.sh')],
                              env=guest_env, encoding='utf-8',
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
        test_start = time.monotonic()
        deadline = (test_start + args.duration_hours * 3600
                    if args.duration_hours is not None else None)
        number = 0
        while True:
            if args.rounds is not None and number >= args.rounds:
                break
            if deadline is not None and number > 0 and time.monotonic() >= deadline:
                break
            number += 1
            tick = time.monotonic()
            output, code = command('/root/run-kqueue-regression.sh', args.timeout)
            (out/f'round-{number:03}.log').write_text(output)
            passed = re.findall(r'^passed: (\d+)\r?$', output, re.M)
            failed = re.findall(r'^failed: (\d+)\r?$', output, re.M)
            expected = str(args.expected_tests)
            ok = (code == 0 and passed == [expected] and failed == ['0'] and
                  output.count('RESULT: PASS - ') == args.expected_tests)
            report['rounds'].append({'round': number, 'exit_code': code, 'passed': passed,
                                     'failed': failed, 'success': ok,
                                     'elapsed_seconds': round(time.monotonic()-tick, 3)})
            report['test_elapsed_seconds'] = round(time.monotonic() - test_start, 3)
            write_json(out / 'checkpoint.json')
            target = str(args.rounds) if args.rounds is not None else f'{args.duration_hours}h'
            print(f'round {number}/{target}: {"PASS" if ok else "FAIL"}', flush=True)
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
    write_json(out / 'summary.json')
    print(out, flush=True)
raise SystemExit(0 if report['success'] else 1)
