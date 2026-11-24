#!/usr/bin/env python3
"""Preflight or explicitly build the owned DriverKit transport. Never installs or activates."""
import argparse
import datetime
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MIN_FREE = 2 * 1024**3


def probe(argv):
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=20)
        return {'argv': argv, 'exitCode': p.returncode, 'stdout': p.stdout.strip(), 'stderr': p.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'argv': argv, 'exitCode': None, 'error': str(error)}


def development_signing_commands(build_root, driver_only, third_party):
    products = build_root / 'DerivedData/Build/Products'
    if driver_only:
        matches = list(products.glob('*/org.openpci.driver.dext'))
        if len(matches) != 1:
            raise RuntimeError('Expected exactly one isolated DEXT build product')
        bundles = [(matches[0], ROOT / 'driver' / (
            'VFIOUserPCIDriver.third-party.entitlements' if third_party else
            'VFIOUserPCIDriver.entitlements'))]
    else:
        matches = list(products.glob('*/OpenPCI.app'))
        if len(matches) != 1:
            raise RuntimeError('Expected exactly one installer build product')
        app = matches[0]
        dext = app / 'Contents/Library/SystemExtensions/org.openpci.driver.dext'
        if not dext.is_dir():
            raise RuntimeError('Installer is missing its embedded DEXT at the required path')
        bundles = [(dext, ROOT / 'driver' / (
            'VFIOUserPCIDriver.third-party.entitlements' if third_party else
            'VFIOUserPCIDriver.entitlements')),
            (app, ROOT / 'installer/VFIOUserHostApp.entitlements')]
    commands = []
    # Sign the embedded DEXT first, then its containing app. Never deep-sign,
    # fetch a profile, create a certificate or change a system installation.
    for bundle, entitlements in bundles:
        commands.append(['/usr/bin/codesign', '--force', '--sign', '-',
                         '--timestamp=none', '--entitlements', str(entitlements), str(bundle)])
    for bundle, _ in bundles:
        commands.append(['/usr/bin/codesign', '--verify', '--strict', str(bundle)])
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--preflight', action='store_true')
    mode.add_argument('--unsigned-build', action='store_true', help='Compile only; artifact cannot activate')
    mode.add_argument('--signed-build', action='store_true', help='Real Xcode signing using supplied valid team/profile/identity')
    mode.add_argument('--development-build', action='store_true',
                      help='Ad-hoc local prototype build only; never activates or changes security settings')
    parser.add_argument('--driver-only', action='store_true',
                        help='Build only the isolated DEXT scheme, without the installer or any QEMU target')
    parser.add_argument('--plan', action='store_true', help='Print the build command without invoking Xcode')
    parser.add_argument('--build-root', type=Path)
    parser.add_argument('--allow-any-userclient-variant', action='store_true',
                        help='Select third-party entitlement variant; normal deployment still requires Apple approval')
    parser.add_argument('--team-id')
    parser.add_argument('--identity')
    parser.add_argument('--app-profile')
    parser.add_argument('--driver-profile')
    parser.add_argument('--receipt', type=Path, default=ROOT / 'preflight.json')
    args = parser.parse_args()
    building = args.unsigned_build or args.signed_build or args.development_build
    if building and args.build_root is None:
        parser.error('Builds require --build-root to select an explicit workspace with sufficient free space.')
    probes = [probe(c) for c in [
        ['xcode-select', '-p'], ['xcodebuild', '-version'],
        ['xcrun', '--sdk', 'driverkit', '--show-sdk-path'], ['xcrun', '--find', 'iig'],
        ['security', 'find-identity', '-v', '-p', 'codesigning']]]
    profile_dirs = [Path.home() / 'Library/MobileDevice/Provisioning Profiles',
                    Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles']
    report = {'schema': 1, 'atUTC': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'scope': 'Tool/profile inventory only; no installation, activation or SDK download.',
              'tools': probes, 'profileDirectoryCounts': {str(p): len(list(p.glob('*'))) if p.is_dir() else 0 for p in profile_dirs},
              'requiredBundleIdentifiers': {'app': 'org.openpci.app',
                                          'dext': 'org.openpci.driver'},
              'freeBytesAtSource': shutil.disk_usage(ROOT).free, 'minimumFreeBytes': MIN_FREE,
              'compiled': False, 'signed': False, 'activated': False,
              'driverOnly': args.driver_only, 'developmentAdHoc': args.development_build}
    report['toolchainReady'] = all(p['exitCode'] == 0 for p in probes[:4])
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(report, indent=2) + '\n')
    if not building:
        print(json.dumps(report, indent=2))
        return 0
    if not args.plan and not report['toolchainReady']:
        raise SystemExit('Build blocked: full Xcode, official DriverKit SDK and iig must all be available. See receipt.')
    if args.signed_build and not all([args.team_id, args.identity, args.app_profile, args.driver_profile]):
        parser.error('Signed builds require --team-id, --identity, --app-profile and --driver-profile; no automatic provisioning.')
    build_root = args.build_root.resolve()
    build_root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(build_root).free < MIN_FREE:
        raise SystemExit('Build blocked: less than2GiB free at selected build root.')
    command = ['xcodebuild', '-project', str(ROOT / 'OpenPCI.xcodeproj'),
               '-scheme', 'OpenPCIDriver' if args.driver_only else 'OpenPCI',
               '-configuration', 'Debug', '-jobs', '1',
               '-derivedDataPath', str(build_root / 'DerivedData'),
               'CLANG_MODULE_CACHE_PATH=' + str(build_root / 'ModuleCache'), 'build']
    if args.allow_any_userclient_variant:
        command += ['READY_DRIVER_ENTITLEMENTS=driver/VFIOUserPCIDriver.third-party.entitlements']
    if args.unsigned_build or args.development_build:
        command += ['CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO']
    else:
        command += ['READY_TEAM_ID=' + args.team_id, 'READY_SIGNING_IDENTITY=' + args.identity,
                    'READY_APP_PROFILE=' + args.app_profile, 'READY_DRIVER_PROFILE=' + args.driver_profile]
    report['buildCommand'] = command
    report['scope'] = 'Explicit build only. No install, activation, security-policy changes or SDK downloads.'
    args.receipt.write_text(json.dumps(report, indent=2) + '\n')
    if args.plan:
        print(json.dumps(report, indent=2))
        return 0
    with (build_root / 'xcodebuild.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    report.update(buildExitCode=result.returncode, compiled=result.returncode == 0,
                  signed=bool(args.signed_build and result.returncode == 0),
                  signingMode='ad-hoc-development' if args.development_build else
                              'provisioned' if args.signed_build else 'unsigned')
    final_code = result.returncode
    if args.development_build and final_code == 0:
        try:
            commands = development_signing_commands(build_root, args.driver_only,
                                                    args.allow_any_userclient_variant)
            report['localSigningCommands'] = []
            with (build_root / 'codesign.log').open('w') as log:
                for command in commands:
                    step = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
                    report['localSigningCommands'].append({'argv': command, 'exitCode': step.returncode})
                    if step.returncode:
                        final_code = step.returncode
                        break
            report['signed'] = final_code == 0
        except RuntimeError as error:
            report['localSigningError'] = str(error)
            final_code = 1
    args.receipt.write_text(json.dumps(report, indent=2) + '\n')
    return final_code


if __name__ == '__main__':
    sys.exit(main())
