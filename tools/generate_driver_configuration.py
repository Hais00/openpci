#!/usr/bin/env python3
"""Render the default OpenPCI DEXT personalities and signing entitlements."""

import argparse
from pathlib import Path
import plistlib


DRIVER = Path(__file__).resolve().parents[1] / 'driver'
BUNDLE = '$(PRODUCT_BUNDLE_IDENTIFIER)'
PCI_SERVICE = 'VFIOUserPCIDriver'
DISPLAY_CLIENT = 'VFIOUserPCIDriverUserClient'
DISPLAY_CLASS = '0x03000000&0xff000000'
AUDIO_CLASS = '0x04030000&0xffffff00'


def service(provider, user_class):
    return {
        'CFBundleIdentifier': BUNDLE,
        'IOClass': 'IOUserService',
        'IOProviderClass': provider,
        'IOUserClass': user_class,
        'IOUserServerName': BUNDLE,
        'IOUserServerOneProcess': True,
    }


def user_client(user_class):
    return {'IOClass': 'IOUserUserClient', 'IOUserClass': user_class}


def configuration():
    personalities = {}
    transport = []

    def add_pci(name, primary, pci_class, properties):
        match = {'IOPCIClassMatch': pci_class}
        if primary is not None:
            match['IOPCIPrimaryMatch'] = primary
        transport.append(match)
        personalities[name] = {
            **service('IOPCIDevice', PCI_SERVICE),
            **match,
            'IOPCITunnelCompatible': True,
            'IOPropertyMatch': {'IOPCITunnelled': True},
            **properties,
        }

    add_pci('OpenPCIExternalDisplay', None, DISPLAY_CLASS, {
        'OpenPCIUserClientProperties': user_client(DISPLAY_CLIENT),
    })
    for model, vendor_id, audio_id in (
        ('Vega', 0x1002, 0xAAF8),
        ('RTX3090', 0x10DE, 0x1AEF),
        ('Polaris', 0x1002, 0xAAF0),
    ):
        add_pci(f'OpenPCI{model}ResetCompanion', f'0x{audio_id:04x}{vendor_id:04x}', AUDIO_CLASS, {
            'OpenPCIResetCompanion': True,
        })
    personalities['OpenPCIRecoveryBroker'] = {
        **service('IOUserResources', 'VFIOUserRecoveryBroker'),
        'CFBundleIdentifierKernel': 'com.apple.kpi.iokit',
        'IOResourceMatch': 'IOKit',
        'IOMatchCategory': f'{BUNDLE}.cold-recovery',
        'RecoveryUserClientProperties': user_client('VFIOUserRecoveryUserClient'),
    }

    return {
        'Info.plist': {
            'CFBundleDisplayName': 'OpenPCI',
            'CFBundleName': 'OpenPCI',
            'IOKitPersonalities': personalities,
            'OSBundleUsageDescription': (
                'Allow authorized applications to access external PCIe GPUs through the OpenPCI SDK.'
            ),
        },
        'VFIOUserPCIDriver.entitlements': {
            'com.apple.developer.driverkit': True,
            'com.apple.developer.driverkit.transport.pci': transport,
            'com.apple.security.app-sandbox': True,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Check rather than write the generated files')
    args = parser.parse_args()
    for name, policy in configuration().items():
        path = DRIVER / name
        contents = plistlib.dumps(policy)
        if args.check:
            if path.read_bytes() != contents:
                raise SystemExit(f'{path} differs from the generated OpenPCI configuration')
        else:
            path.write_bytes(contents)


if __name__ == '__main__':
    main()
