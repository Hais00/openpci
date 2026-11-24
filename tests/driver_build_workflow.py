"""Local signing plan checks; never executes Xcode, codesign or activation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('openpci_driver_build', ROOT / 'tools/build_driver.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class LocalSigningPlan(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.products = self.root / 'DerivedData/Build/Products'

    def test_embedded_driver_is_signed_before_its_app(self):
        app = self.products / 'Debug/OpenPCI.app'
        dext = app / 'Contents/Library/SystemExtensions/org.openpci.driver.dext'
        dext.mkdir(parents=True)
        commands = build.development_signing_commands(self.root, False, True)
        self.assertEqual([c[-1] for c in commands], [str(dext), str(app), str(dext), str(app)])
        self.assertIn('VFIOUserPCIDriver.third-party.entitlements', commands[0][-2])
        self.assertIn('VFIOUserHostApp.entitlements', commands[1][-2])
        self.assertTrue(all('--deep' not in c for c in commands))
        self.assertTrue(all(c[0] == '/usr/bin/codesign' for c in commands))
        self.assertEqual(commands[0][commands[0].index('--sign') + 1], '-')

    def test_missing_embedded_driver_is_an_error(self):
        (self.products / 'Debug/OpenPCI.app').mkdir(parents=True)
        with self.assertRaisesRegex(RuntimeError, 'missing its embedded DEXT'):
            build.development_signing_commands(self.root, False, False)

    def test_isolated_driver_has_no_installer_signing(self):
        dext = self.products / 'Debug-driverkit/org.openpci.driver.dext'
        dext.mkdir(parents=True)
        commands = build.development_signing_commands(self.root, True, False)
        self.assertEqual(len(commands), 2)
        self.assertEqual(commands[0][-1], str(dext))
        self.assertTrue(commands[0][-2].endswith('/VFIOUserPCIDriver.entitlements'))

    def test_ambiguous_products_are_rejected(self):
        for configuration in ['Debug-driverkit', 'Release-driverkit']:
            (self.products / configuration / 'org.openpci.driver.dext').mkdir(parents=True)
        with self.assertRaisesRegex(RuntimeError, 'exactly one'):
            build.development_signing_commands(self.root, True, False)


if __name__ == '__main__':
    unittest.main()
