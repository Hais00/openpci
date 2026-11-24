#!/usr/bin/env python3
"""Build and package the native SDK without installing or activating a driver."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
FLOOR = 2 * 1024**3
GROWTH = 512 * 1024**2
OUTPUT_CAP = 512 * 1024**2


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path: Path, value: dict) -> None:
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def active_jobs() -> list[dict]:
    inventory = subprocess.check_output(['/bin/ps', '-axo', 'pid=,comm='], text=True)
    names = {'clang', 'clang++', 'cc1', 'g++', 'ninja', 'wine', 'wine64', 'wineserver'}
    found = []
    for row in inventory.splitlines():
        fields = row.strip().split(None, 1)
        if len(fields) != 2:
            continue
        name = Path(fields[1]).name.lower()
        if name in names or name.startswith('qemu-system-'):
            found.append({'pid': int(fields[0]), 'name': name})
    return found


def input_files() -> list[dict]:
    files = [ROOT / 'LICENSE', ROOT / 'NOTICE', ROOT / 'CMakeLists.txt',
             ROOT / 'cmake/extGPUConfig.cmake.in', Path(__file__).resolve()]
    files.extend(p for p in (ROOT / 'LICENSES').glob('*') if p.is_file())
    files.extend(p for p in (ROOT / 'driver').glob('*.hpp') if p.is_file())
    for folder in ('include', 'sdk', 'tests'):
        files.extend(p for p in (ROOT / folder).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts)
    files.extend(p for p in (ROOT / 'tools').glob('*.c'))
    return [{'path': str(p), 'sha256': digest(p), 'bytes': p.stat().st_size}
            for p in sorted(set(files))]


def output_bytes(path: Path) -> int:
    total = 0
    for p in path.rglob('*'):
        try:
            if p.is_file() and not p.is_symlink():
                total += p.stat().st_size
        except FileNotFoundError:
            # Compilers atomically replace temporary output files.
            continue
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', type=Path,
                        default=Path.home() / 'Library/Caches/ext-GPU/sdk-build')
    parser.add_argument('--architecture', choices=('universal', 'arm64', 'x86_64'), default='universal')
    parser.add_argument('--skip-tests', action='store_true', help='Not accepted for a qualified SDK package')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('macOS is required')
    if args.skip_tests:
        parser.error('SDK packaging requires its verification checks')
    if shutil.which('cmake') is None:
        parser.error('cmake is required')
    args.build_root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(args.build_root).free < FLOOR + GROWTH:
        parser.error('build requires 2 GiB floor plus 512 MiB output/growth allowance')
    busy = active_jobs()
    if busy:
        parser.error('another compiler/VM/Wine job is active: ' + json.dumps(busy))
    run = args.build_root / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    run.mkdir(mode=0o700)
    (run / 'logs').mkdir()
    (run / 'tmp').mkdir()
    state = {'schema': 'extgpu.sdk-build.v1', 'status': 'RUNNING',
             'sourceRoot': str(ROOT), 'runRoot': str(run), 'architecture': args.architecture,
             'minimumFreeBytes': FLOOR, 'outputCapBytes': OUTPUT_CAP,
             'inputs': input_files(), 'commands': [], 'hardwareAccess': False,
             'driverBuilt': False, 'driverInstalled': False, 'driverActivated': False}
    receipt = run / 'result.json'
    for item in state['inputs']:
        relative = Path(item['path']).relative_to(ROOT)
        snapshot = run / 'inputs' / relative
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(item['path'], snapshot)
        if digest(snapshot) != item['sha256']:
            raise RuntimeError('input changed while snapshotting: ' + item['path'])
        item['snapshotPath'] = str(snapshot)
    save(receipt, state)
    env = os.environ.copy()
    env.update(TMPDIR=str(run / 'tmp'), CLANG_MODULE_CACHE_PATH=str(run / 'ModuleCache'),
               CTEST_OUTPUT_ON_FAILURE='1')

    def command(name: str, argv: list[str], timeout: int = 300) -> str:
        start = time.monotonic()
        log = run / 'logs' / (name + '.log')
        with log.open('xb') as stream:
            proc = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT, env=env,
                                    start_new_session=True)
            try:
                while proc.poll() is None:
                    if time.monotonic() - start > timeout:
                        raise RuntimeError(name + ' exceeded its deadline')
                    if shutil.disk_usage(run).free < FLOOR:
                        raise RuntimeError('disk floor reached')
                    if output_bytes(run) > OUTPUT_CAP:
                        raise RuntimeError('SDK build output cap reached')
                    time.sleep(0.1)
            except BaseException:
                try:
                    os.killpg(proc.pid, 15)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, 9)
                    proc.wait(timeout=5)
                raise
        record = {'name': name, 'argv': argv, 'pid': proc.pid, 'returnCode': proc.returncode,
                  'elapsedSeconds': round(time.monotonic() - start, 3),
                  'logPath': str(log), 'logSHA256': digest(log)}
        state['commands'].append(record)
        save(receipt, state)
        if proc.returncode != 0:
            raise RuntimeError(name + ' failed; see ' + str(log))
        return log.read_text(errors='replace')

    try:
        architectures = 'arm64;x86_64' if args.architecture == 'universal' else args.architecture
        command('configure', ['cmake', '-S', str(ROOT), '-B', str(run / 'build'),
                             '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                             '-DCMAKE_OSX_ARCHITECTURES=' + architectures,
                             '-DCMAKE_INSTALL_PREFIX=' + str(run / 'package'), '-DBUILD_TESTING=ON'])
        command('build', ['cmake', '--build', str(run / 'build'), '--parallel', '1'])
        command('package', ['cmake', '--install', str(run / 'build')])
        listing = command('test-inventory', ['ctest', '--test-dir', str(run / 'build'), '-N'])
        if 'Total Tests: 0' in listing:
            raise RuntimeError('refusing to qualify an SDK with no registered tests')
        command('tests', ['ctest', '--test-dir', str(run / 'build'), '--output-on-failure'])
        for item in state['inputs']:
            if digest(Path(item['path'])) != item['sha256']:
                raise RuntimeError('build input changed during verification: ' + item['path'])
        libraries = [run / 'package/lib/libextgpu.a', run / 'package/lib/libextgpu.dylib']
        for index, library in enumerate(libraries):
            # Check each architecture separately: the installed lipo accepts
            # one -verify_arch argument even though its usage prints "...".
            for architecture in architectures.split(';'):
                command('verify-arch-' + str(index) + '-' + architecture,
                        ['/usr/bin/lipo', str(library), '-verify_arch', architecture])
        state['packageFiles'] = [{'path': str(p.relative_to(run / 'package')),
                                  'sha256': digest(p), 'bytes': p.stat().st_size}
                                 for p in sorted((run / 'package').rglob('*'))
                                 if p.is_file() and not p.is_symlink()]
        archive = run / 'ext-GPU-SDK-macos.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            stream.add(run / 'package', arcname='ext-GPU-SDK')
        state['archive'] = {'path': str(archive), 'sha256': digest(archive),
                            'bytes': archive.stat().st_size}
        state['status'] = 'PASS_SDK_BUILD_TEST_AND_PACKAGE'
    except BaseException as error:
        state['status'] = 'FAIL_SDK_BUILD'
        state['failure'] = str(error)
        save(receipt, state)
        raise
    finally:
        state['freeBytesAfter'] = shutil.disk_usage(run).free
        save(receipt, state)
    print(json.dumps({'status': state['status'], 'receipt': str(receipt), 'archive': state['archive']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
