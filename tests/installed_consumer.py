#!/usr/bin/env python3
"""Compile external consumers using installed SDK binaries/headers only."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--config', default='Release')
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    if bool(args.prefix) == bool(args.build):
        parser.error('Provide exactly one of --prefix or --build')
    args.work.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix='run-', dir=args.work)
    args.work = Path(temporary.name)
    package = args.work / 'SDK with spaces'
    if args.build:
        subprocess.run(['cmake', '--install', str(args.build), '--config', args.config,
                        '--prefix', str(package)], check=True)
    else:
        shutil.copytree(args.prefix, package, symlinks=True)
    assert not (package / 'include/extgpu/extgpu_driver_abi.h').exists()
    assert not (package / 'include/extgpu/extgpu_test.h').exists()
    source = args.work / 'application'
    source.mkdir()
    (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(IndependentConsumer LANGUAGES CXX)
find_package(extGPU 1 CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE extGPU::extgpu)
''')
    (source / 'main.cpp').write_text('''#include <extgpu/extgpu.h>
#include <cstdint>
static_assert(sizeof(extgpu_status) == sizeof(std::int32_t), "public status ABI");
int main() {
  if (extgpu_sdk_abi_version() != EXTGPU_SDK_ABI_VERSION) return 1;
  extgpu_mapping *mapping = nullptr;
  if (extgpu_bar_map_with_cache_policy(nullptr, 0, EXTGPU_BAR_CACHE_INHIBIT, &mapping)
      != EXTGPU_E_ARGUMENT || mapping) return 4;
  extgpu_session *session = nullptr;
  if (extgpu_session_create(&session) != EXTGPU_OK) return 2;
  return extgpu_session_destroy(&session) == EXTGPU_OK && !session ? 0 : 3;
}
''')
    commands = []

    def run(argv):
        result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, timeout=60)
        commands.append({'argv': [str(x) for x in argv], 'returnCode': result.returncode,
                         'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    try:
        build = args.work / 'consumer-build'
        run(['cmake', '-S', source, '-B', build,
             '-DextGPU_DIR=' + str(package / 'lib/cmake/extGPU'),
             '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'])
        run(['cmake', '--build', build, '--parallel', '1'])
        run([build / 'consumer'])
        run(['/usr/bin/arch', '-x86_64', build / 'consumer'])
        # The module map autolinks extgpu. Give Swift a directory containing
        # only the static archive, so no external dylib signing/rpath is needed.
        if shutil.which('swiftc'):
            static = args.work / 'static-lib'
            static.mkdir()
            shutil.copyfile(package / 'lib/libextgpu.a', static / 'libextgpu.a')
            swift = args.work / 'main.swift'
            swift.write_text('''import ExtGPU
var session: OpaquePointer? = nil
precondition(extgpu_sdk_abi_version() == EXTGPU_SDK_ABI_VERSION)
var mapping: OpaquePointer? = nil
precondition(extgpu_bar_map_with_cache_policy(nil, 0,
    extgpu_bar_cache_policy(EXTGPU_BAR_CACHE_INHIBIT), &mapping) == EXTGPU_E_ARGUMENT)
precondition(mapping == nil)
precondition(extgpu_session_create(&session) == EXTGPU_OK)
precondition(extgpu_session_destroy(&session) == EXTGPU_OK)
precondition(session == nil)
''')
            for architecture in ('arm64', 'x86_64'):
                binary = args.work / ('swift-' + architecture)
                run(['swiftc', '-module-cache-path', args.work / 'module-cache',
             '-target', architecture + '-apple-macosx13.0',
                     '-I', package / 'include/extgpu', '-L', static, swift, '-o', binary])
                run([binary] if architecture == 'arm64' else ['/usr/bin/arch', '-x86_64', binary])
        print('PASS external C++17 and available Swift consumers using relocated binary SDK only')
        status = 'PASS_BINARY_SDK_CONSUMERS'
    except Exception:
        status = 'FAIL_BINARY_SDK_CONSUMERS'
        raise
    finally:
        (args.work / 'result.json').write_text(json.dumps({'status': status, 'commands': commands,
            'sdkImplementationSourcesUsed': False, 'driverKitUsed': False,
            'deviceOpened': False, 'hardwareAccess': False}, indent=2) + '\n')


if __name__ == '__main__':
    main()
