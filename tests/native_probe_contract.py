#!/usr/bin/env python3
"""Exercise native-probe command/lifetime handling using a simulated public SDK.

This test neither links libextgpu nor accesses IOKit or physical hardware.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


class Probe:
    def __init__(self, binary: Path, fault: str = '', extra: tuple[str, ...] = ()):
        env = os.environ.copy()
        env['NATIVE_PROBE_FIXTURE_FAULT'] = fault
        self.process = subprocess.Popen(
            [str(binary), '--registry-id', '1', '--expect-bdf', '05:00.0', *extra],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
            bufsize=0,
        )
        self.pending = b''
        self.events: list[dict] = []
        self.until('ready')

    def until(self, event: str) -> list[dict]:
        found = []
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if b'\n' not in self.pending:
                assert self.process.stdout
                ready, _, _ = select.select([self.process.stdout], [], [], max(0, deadline - time.monotonic()))
                assert ready, f'timeout waiting for {event}; seen {found}'
                chunk = os.read(self.process.stdout.fileno(), 65536)
                assert chunk, f'probe exited waiting for {event}; seen {found}'
                self.pending += chunk
            while b'\n' in self.pending:
                line, self.pending = self.pending.split(b'\n', 1)
                record = json.loads(line)
                self.events.append(record)
                found.append(record)
                if record['event'] == event:
                    return found
        raise AssertionError(f'timeout waiting for {event}')

    def send(self, command: str, last: str | None = None) -> list[dict]:
        assert self.process.stdin
        self.process.stdin.write((command + '\n').encode())
        self.process.stdin.flush()
        return self.until(last or command.split()[0])

    def close(self):
        if self.process.poll() is None:
            self.process.kill()  # Simulated fixture only; never a hardware probe.
        self.process.wait(timeout=5)

    def finish(self):
        self.send('release')
        self.send('close', 'closed')
        assert self.process.wait(timeout=5) == 0


def last_status(events: list[dict]) -> dict:
    return [event for event in events if 'status' in event][-1]


def check(binary: Path):
    help_result = subprocess.run([str(binary), '--help'], capture_output=True, check=True, timeout=5)
    help_record = json.loads(help_result.stdout)
    assert help_record['event'] == 'help'
    assert not any(command.startswith('hvf-') for command in help_record['commands'])
    assert not help_result.stderr
    with tempfile.TemporaryDirectory(prefix='extgpu-probe-log-') as temporary:
        destination = Path(temporary) / 'probe.jsonl'
        logged = subprocess.run([str(binary), '--registry-id', '1', '--log-file', str(destination)],
                                input=b'close\n', capture_output=True, check=True, timeout=5)
        assert not logged.stdout
        assert logged.stderr.decode().strip() == 'native-probe JSONL: ' + str(destination)
        records = [json.loads(line) for line in destination.read_text().splitlines()]
        assert records[0]['event'] == 'log-open' and records[-1]['event'] == 'closed'
        assert destination.stat().st_mode & 0o777 == 0o600
        assert all(not record['vm_context_created'] and not record['hvf_mapping_retained']
                   and not record['vcpu_created'] and not record['vcpu_run']
                   for record in records if 'device_open' in record)
        original = destination.read_bytes()
        refused = subprocess.run([str(binary), '--registry-id', '1', '--log-file', str(destination)],
                                 capture_output=True, timeout=5)
        assert refused.returncode == 1 and not refused.stderr
        assert json.loads(refused.stdout)['event'] == 'log-open'
        assert destination.read_bytes() == original
        symbolic = Path(temporary) / 'symlink.jsonl'
        symbolic.symlink_to(destination)
        refused = subprocess.run([str(binary), '--registry-id', '1', '--log-file', str(symbolic)],
                                 capture_output=True, timeout=5)
        assert refused.returncode == 1 and destination.read_bytes() == original
        assert json.loads(refused.stdout)['event'] == 'log-open'
        refused = subprocess.run([str(binary), '--registry-id', '1', '--log-file', 'relative.jsonl'],
                                 capture_output=True, timeout=5)
        assert refused.returncode == 2 and json.loads(refused.stdout)['event'] == 'help'
    p = Probe(binary, extra=('--expect-host-root', 'different root'))
    try:
        assert p.events[-1]['mutation_authorized_by_identity'] is False
        assert not any(event['event'] == 'device-open' for event in p.events)
        p.send('close', 'closed')
    finally:
        p.close()
    # No implicit claim. Wrong identity cannot authorize any owner phase.
    p = Probe(binary, extra=('--expect-device', '0x0001'))
    try:
        assert p.events[-1]['mutation_authorized_by_identity'] is False
        assert last_status(p.send('claim'))['status'] != 0
        p.send('close', 'closed')
        assert p.process.wait(timeout=5) == 0
    finally:
        p.close()

    p = Probe(binary)
    try:
        assert not any(e.get('claimed') for e in p.events)
        assert last_status(p.send('hvf-map', 'unknown_command'))['status'] != 0
        assert last_status(p.send('claim'))['status'] == 0
        p.send('config-header', 'config_value')
        # Four config-value events; the first was consumed above.
        for _ in range(3):
            p.until('config_value')
        p.send('bar-info 0', 'bar_metadata')
        events = p.send('second-claim', 'second-close')
        assert next(e for e in events if e['event'] == 'second_claim_check')['exclusive_refusal_observed']
        assert last_status(p.send('release'))['status'] == 0
        assert last_status(p.send('claim'))['status'] == 0
        p.finish()
    finally:
        p.close()

    # Every failed retirement retains its handle, blocks new mutation and is retryable.
    for fault, create, retire, retained in (
        ('unmap-once', 'map-bar 0', 'unmap-bar', 'mapping_retained'),
        ('retire-once', 'dma-register', 'dma-retire', 'buffer_retained'),
        ('irq-close-once', 'irq-create', 'irq-close', 'irq_retained'),
    ):
        p = Probe(binary, fault)
        try:
            p.send('claim')
            if create.startswith('dma'):
                p.send('dma-bits 32')
            p.send(create, create.split()[0])
            result = last_status(p.send(retire))
            assert result['status'] != 0 and result[retained] and result['cleanup_required']
            assert p.process.poll() is None
            assert last_status(p.send('dma-bits 64'))['status'] != 0
            assert last_status(p.send('close', 'device-close'))['status'] != 0
            result = last_status(p.send(retire))
            assert result['status'] == 0 and not result[retained]
            if retained == 'buffer_retained':
                assert result['backing_retained'] is False
            p.finish()
        finally:
            p.close()

    # Failed initial setup may already own state, not only a failed retirement.
    for fault, create, retire, retained in (
        ('register-unsafe', 'dma-register', 'dma-retire', 'buffer_retained'),
        ('irq-unsafe', 'irq-create', 'irq-close', 'irq_retained'),
    ):
        p = Probe(binary, fault)
        try:
            p.send('claim')
            p.send('dma-bits 32')
            result = last_status(p.send(create))
            assert result['status'] != 0 and result[retained] and result['cleanup_required']
            if retained == 'buffer_retained':
                assert result['backing_retained']
            assert last_status(p.send('irq-create'))['status'] != 0
            p.send(retire)
            p.finish()
        finally:
            p.close()

    p = Probe(binary, 'release-once')
    try:
        p.send('claim')
        assert last_status(p.send('release'))['claim_uncertain']
        assert last_status(p.send('close', 'device-close'))['status'] != 0
        assert p.process.poll() is None
        p.finish()
    finally:
        p.close()

    # Unexpected double ownership is retained and blocks first-owner mutation.
    p = Probe(binary, 'double-claim')
    try:
        p.send('claim')
        check_events = p.send('second-claim', 'second_claim_check')
        check_result = check_events[-1]
        assert not check_result['exclusive_refusal_observed']
        assert check_result['unexpected_owner_retained']
        result = last_status(p.send('dma-bits 32'))
        assert result['status'] != 0 and result['second_claimed']
        p.send('second-release')
        p.send('second-close')
        p.finish()
    finally:
        p.close()

    # An IRQ operation's uncertain failure quarantines the live resource.
    for fault, operation in (
        ('irq-mask-io', 'irq-mask-zero'),
        ('irq-drain-protocol', 'irq-drain'),
        ('irq-rearm-stale', 'irq-rearm'),
        ('irq-rearm-unsafe', 'irq-rearm'),
    ):
        p = Probe(binary, fault)
        try:
            p.send('claim')
            p.send('irq-create')
            result = last_status(p.send(operation))
            assert result['status'] != 0 and result['irq_retained'] and result['cleanup_required']
            assert last_status(p.send('map-bar 0'))['status'] != 0
            result = last_status(p.send('irq-close'))
            assert result['status'] == 0 and not result['irq_retained'] and not result['cleanup_required']
            p.finish()
        finally:
            p.close()

    p = Probe(binary, 'irq-rearm-busy')
    try:
        p.send('claim')
        p.send('irq-create')
        result = last_status(p.send('irq-rearm'))
        assert result['status'] == -7 and not result['cleanup_required']
        assert last_status(p.send('irq-mask-zero'))['status'] == 0
        p.send('irq-close')
        p.finish()
    finally:
        p.close()

    p = Probe(binary)
    try:
        p.send('claim')
        assert p.process.stdin
        p.process.stdin.close()
        result = p.until('stdin_closed_retained')[-1]
        assert result['claimed'] and p.process.poll() is None
        p.process.terminate()
        result = p.until('signal_retained')[-1]
        assert result['claimed'] and p.process.poll() is None
    finally:
        p.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='/usr/bin/cc')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='extgpu-probe-contract-') as temporary:
        binary = Path(temporary) / 'simulated-native-probe'
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-I', str(ROOT / 'include'), str(ROOT / 'tools/native-probe.c'),
                        str(ROOT / 'tests/native_probe_fake_sdk.c'), '-o', str(binary)],
                       check=True, timeout=60)
        check(binary)
    print(json.dumps({'status': 'PASS_SIMULATED_NATIVE_PROBE_CONTRACT', 'hardware_access': False}))


if __name__ == '__main__':
    main()
