"""Exercise the actual helper protocol without desktop or real credentials."""
import configparser
from pathlib import Path
import subprocess
import sys
import unittest

HELPER, METADATA = sys.argv[1:3]
del sys.argv[1:3]


class AuthDialogTests(unittest.TestCase):
    def invoke(self, secrets=(), options=(), external=True, service='org.freedesktop.NetworkManager.shadowsocks'):
        payload = 'DATA_KEY=profile\nDATA_VAL={}\n\n' + ''.join(f'SECRET_KEY={k}\nSECRET_VAL={v}\n\n' for k, v in secrets)
        args = [HELPER, '-u', 'test-uuid', '-n', 'Test VPN', '-s', service]
        if external:
            args.append('--external-ui-mode')
        return subprocess.run(args + list(options), input=payload + 'DONE\n\n',
                              text=True, capture_output=True, timeout=3)

    def parse(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        output = configparser.ConfigParser(interpolation=None)
        output.read_string(result.stdout)
        self.assertEqual(output['VPN Plugin UI']['Version'], '2')
        return output

    def test_metadata(self):
        config = configparser.ConfigParser()
        config.read(METADATA)
        self.assertEqual(Path(config['GNOME']['auth-dialog']).name, Path(HELPER).name)
        self.assertTrue(config['GNOME'].getboolean('supports-external-ui-mode'))

    def test_saved_passwords(self):
        values = [('tcp-password', 'fake-tcp'), ('udp-password', 'fake-udp')]
        config = self.parse(self.invoke(values))
        for key, value in values:
            self.assertEqual(config[key]['Value'], value)
            self.assertFalse(config[key].getboolean('ShouldAsk'))
            self.assertTrue(config[key].getboolean('IsSecret'))

    def test_missing_noninteractive(self):
        config = self.parse(self.invoke())
        self.assertFalse(config['tcp-password'].getboolean('ShouldAsk'))
        self.assertEqual(config['tcp-password']['Value'], '')

    def test_missing_interactive(self):
        config = self.parse(self.invoke([('tcp-password', 'known')], ['-i']))
        self.assertFalse(config['tcp-password'].getboolean('ShouldAsk'))
        self.assertTrue(config['udp-password'].getboolean('ShouldAsk'))

    def test_reprompt(self):
        config = self.parse(self.invoke([('tcp-password', 'known')], ['-i', '-r']))
        self.assertTrue(config['tcp-password'].getboolean('ShouldAsk'))

    def test_legacy_and_invalid_service(self):
        result = self.invoke([('tcp-password', 'known')], external=False)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, 'tcp-password\nknown\n\n\n')
        result = self.invoke(service='unrelated-service')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')


unittest.main()
