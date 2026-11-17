"""Offline tests for the standalone Python dataset updater."""
import gettext
import importlib.machinery
import importlib.util
import io
import json
import locale
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import urllib.request

UPDATER = Path(os.environ["SS_TEST_UPDATER"])
ROOT = Path(__file__).resolve().parent.parent
loader = importlib.machinery.SourceFileLoader("routes_updater", str(UPDATER))
spec = importlib.util.spec_from_loader(loader.name, loader)
updater = importlib.util.module_from_spec(spec)
loader.exec_module(updater)
FIXTURE = ("2|apnic|20261113|3|20261113|20261113|+0000\n"
           "apnic|CN|ipv4|192.0.2.1|6|20200101|allocated\n"
           "apnic|CN|ipv6|2001:db8::|32|20200101|assigned\n"
           "apnic|HK|ipv4|198.51.100.0|256|20200101|allocated\n")


class UpdaterTests(unittest.TestCase):
    def test_command_name(self):
        result = subprocess.run([sys.executable, str(UPDATER), '--help'],
                                check=True, capture_output=True, text=True)
        self.assertIn('nm-shadowsocks-update-routes', result.stdout)

    def test_non_aligned_range_and_country_filter(self):
        prefixes, serial = updater.parse_apnic(FIXTURE)
        self.assertEqual(serial, "20261113")
        self.assertEqual(prefixes, ["192.0.2.1/32", "192.0.2.2/31", "192.0.2.4/31",
                                    "192.0.2.6/32", "2001:db8::/32"])

    def test_invalid_and_incomplete_inputs(self):
        cases = ["<html>error</html>", FIXTURE.replace("|3|20261113", "|999|20261113"),
                 FIXTURE.replace("192.0.2.1|6", "255.255.255.255|2"),
                 FIXTURE.replace("2001:db8::|32", "2001:db8::1|32"),
                 FIXTURE.replace("2001:db8::|32", "2001:db8::|129"),
                 FIXTURE.replace("192.0.2.1|6", "192.0.2.1|bad"),
                 FIXTURE.replace("20261113", "20269999"), FIXTURE + FIXTURE,
                 FIXTURE.replace("|ipv6|", "|asn|")]
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                updater.parse_apnic(text)

    def test_bounded_read(self):
        self.assertEqual(updater.read_bounded(io.BytesIO(FIXTURE.encode())), FIXTURE)
        with mock.patch.object(updater, "MAX_BYTES", 4), self.assertRaises(ValueError):
            updater.read_bounded(io.BytesIO(b"12345"))
        with self.assertRaises(ValueError):
            updater.read_bounded(io.BytesIO(b"nul\0byte"))
        with self.assertRaises(TimeoutError):
            updater.read_bounded(io.BytesIO(b"data"), deadline=0)

    def test_reject_insecure_redirect(self):
        handler = updater.HTTPSRedirectHandler()
        request = urllib.request.Request(updater.APNIC_URL)
        with self.assertRaises(urllib.error.URLError):
            handler.redirect_request(request, None, 302, "Found", {}, "http://example.org/data")

    def test_download_uses_bounded_reader(self):
        opener = mock.Mock()
        opener.open.return_value = io.BytesIO(FIXTURE.encode())
        with mock.patch.object(updater.urllib.request, "build_opener", return_value=opener):
            self.assertEqual(updater.download(), FIXTURE)
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 15)

    def test_download_failure_preserves_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            old = directory / "china.cidr"
            old.write_text("previous dataset")
            with mock.patch.object(updater, "download", side_effect=OSError("offline")):
                with mock.patch("sys.stderr", new_callable=io.StringIO):
                    self.assertEqual(updater.main(["--output", str(directory)]), 1)
            self.assertEqual(old.read_text(), "previous dataset")

    def test_atomic_write_and_metadata(self):
        prefixes, serial = updater.parse_apnic(FIXTURE)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            updater.write_dataset(prefixes, serial, directory)
            combined = directory / "china.cidr"
            before = combined.read_bytes()
            metadata = json.loads((directory / "china.json").read_text())
            self.assertEqual(metadata["sha256"], updater.hashlib.sha256(before).hexdigest())
            self.assertEqual(metadata["ipv4_prefixes"], 4)
            self.assertEqual(metadata["ipv6_prefixes"], 1)
            self.assertEqual(combined.stat().st_mode & 0o777, 0o644)
            replace = os.replace
            def fail_commit(source, destination):
                if Path(destination).name == "china.cidr":
                    raise OSError("simulated commit failure")
                return replace(source, destination)
            with mock.patch.object(updater.os, "replace", side_effect=fail_commit):
                with self.assertRaises(OSError):
                    updater.write_dataset(prefixes, serial, directory)
            self.assertEqual(combined.read_bytes(), before)
            self.assertFalse(list(directory.glob(".routes-*")))

    def test_offline_cli_and_invalid_update(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            source = directory / "delegated"
            output = directory / "output"
            source.write_text(FIXTURE)
            command = [sys.executable, str(UPDATER), "--source", str(source), "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True)
            before = (output / "china.cidr").read_bytes()
            source.write_text("broken input")
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((output / "china.cidr").read_bytes(), before)

    def test_gettext_output(self):
        old_locale = locale.setlocale(locale.LC_ALL)
        try:
            locale.setlocale(locale.LC_ALL, "zh_CN.UTF-8")
        except locale.Error:
            self.skipTest("Simplified Chinese locale is not installed")
        finally:
            locale.setlocale(locale.LC_ALL, old_locale)
        catalog_dir = os.environ["SS_TEST_LOCALEDIR"]
        expected = gettext.translation(updater.DOMAIN, catalog_dir, languages=["zh_CN"]).gettext
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "updater.py"
            script.write_text(re.sub(r'^LOCALEDIR = .*$', f"LOCALEDIR = {catalog_dir!r}",
                                     UPDATER.read_text(), flags=re.MULTILINE))
            env = dict(os.environ, LC_ALL="zh_CN.UTF-8", LANGUAGE="zh_CN")
            result = subprocess.run([sys.executable, str(script), "--source", str(script)],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn(expected("Invalid RIR data: %s").split("%s")[0], result.stderr)
            self.assertNotIn("Invalid RIR data:", result.stderr)

    def test_region_normalization(self):
        self.assertEqual(updater.normalize_regions([" jp,sg ", "JP"]), ("JP", "SG"))
        for values in (["../CN"], [""], ["CN,"], ["China"], ["C1"]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                updater.normalize_regions(values)

    def test_multiple_regions_and_single_family(self):
        prefixes, _ = updater.parse_apnic(FIXTURE, ("CN", "HK"), "ipv4")
        self.assertEqual(len(prefixes), 5)
        self.assertIn("198.51.100.0/24", prefixes)
        self.assertFalse(any(":" in prefix for prefix in prefixes))
        with self.assertRaises(ValueError):
            updater.parse_apnic(FIXTURE, ("CN", "HK"))
        with self.assertRaises(ValueError):
            updater.parse_apnic(FIXTURE, ("ZZ",))

    def test_generic_registry_and_mismatched_header(self):
        text = FIXTURE.replace("2|apnic|20261113", "2.3|arin|1794578421220").replace("apnic|", "arin|").replace("|CN|", "|US|")
        prefixes, serial, registry, coverage = updater.parse_registry(text, ("US",))
        self.assertEqual(registry, "arin")
        self.assertEqual(serial, "1794578421220")
        self.assertEqual(coverage, {"US": {4, 6}})
        self.assertEqual(len(prefixes), 5)
        with self.assertRaises(ValueError):
            updater.parse_registry(text, ("US",), "apnic")

    def test_lacnic_asn_ranges_are_records(self):
        text = ("2|lacnic|20261112|5|19870101|20261112|-0300\n"
                "lacnic|BR|ipv4|192.0.2.0|256|20200101|allocated\n"
                "lacnic|BR|ipv6|2001:db8::|32|20200101|allocated\n"
                "lacnic|BR|asn|64512|3|20200101|allocated\n")
        with self.assertRaises(ValueError):
            updater.parse_registry(text, ("BR",), "lacnic")
        text = text.replace("|20261112|5|", "|20261112|3|")
        prefixes, _, registry, coverage = updater.parse_registry(text, ("BR",), "lacnic")
        self.assertEqual(registry, "lacnic")
        self.assertEqual(len(prefixes), 2)
        self.assertEqual(coverage["BR"], {4, 6})
        with self.assertRaises(ValueError):
            updater.parse_registry(text.rsplit("lacnic|BR|asn", 1)[0], ("BR",), "lacnic")

    def test_custom_cli_and_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            source = directory / "delegated"
            source.write_text(FIXTURE)
            output = directory / "output"
            result = subprocess.run([sys.executable, str(UPDATER), "--source", str(source),
                "--region", "hk,cn", "--region", "CN", "--family", "ipv4", "--output", str(output)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((output / "cn-hk.cidr").exists())
            self.assertFalse((output / "china.cidr").exists())
            metadata = json.loads((output / "cn-hk.json").read_text())
            self.assertEqual(metadata["regions"], ["CN", "HK"])
            self.assertEqual(metadata["family"], "ipv4")
            self.assertEqual(metadata["ipv6_prefixes"], 0)

    def test_custom_without_output_does_not_download(self):
        for args in (["--region", "JP"], ["--family", "ipv4"], ["--registry", "all"]):
            with self.subTest(args=args), mock.patch.object(updater, "download") as download:
                with mock.patch("sys.stderr", new_callable=io.StringIO):
                    self.assertEqual(updater.main(args), 1)
                download.assert_not_called()

    def test_merge_registries_and_preserve_on_failure(self):
        other = FIXTURE.replace("apnic", "arin").replace("192.0.2.1|6", "203.0.113.0|256")
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            sources = [directory / "apnic", directory / "arin"]
            sources[0].write_text(FIXTURE)
            sources[1].write_text(other)
            args = ["--source", str(sources[0]), "--source", str(sources[1]),
                    "--registry", "all", "--output", str(directory)]
            with mock.patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(updater.main(args), 0)
            metadata = json.loads((directory / "cn.json").read_text())
            self.assertEqual(len(metadata["sources"]), 2)
            self.assertEqual(metadata["ipv4_prefixes"], 5)
            before = (directory / "cn.cidr").read_bytes()
            sources[1].write_text("incomplete input")
            with mock.patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(updater.main(args), 1)
            self.assertEqual((directory / "cn.cidr").read_bytes(), before)

    def test_duplicate_registry_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "apnic"
            source.write_text(FIXTURE)
            with mock.patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(updater.main(["--source", str(source), "--source", str(source),
                    "--output", tmp]), 1)
            self.assertFalse((Path(tmp) / "china.cidr").exists())

    def test_all_registries_and_no_partial_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            def document(registry):
                return FIXTURE.replace("apnic", registry)
            with mock.patch.object(updater, "download", side_effect=document) as download:
                with mock.patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(updater.main(["--registry", "all", "--output", tmp]), 0)
                self.assertEqual([call.args[0] for call in download.call_args_list], list(updater.REGISTRIES))
            combined = Path(tmp) / "cn.cidr"
            before = combined.read_bytes()
            metadata = json.loads((Path(tmp) / "cn.json").read_text())
            self.assertEqual(len(metadata["sources"]), 5)
            def failed_download(registry):
                if registry == "ripencc":
                    raise OSError("offline")
                return document(registry)
            with mock.patch.object(updater, "download", side_effect=failed_download):
                with mock.patch("sys.stderr", new_callable=io.StringIO):
                    self.assertEqual(updater.main(["--registry", "all", "--output", tmp]), 1)
            self.assertEqual(combined.read_bytes(), before)

    def test_custom_ranges_are_collapsed(self):
        text = ("2|apnic|20261113|2|20261113|20261113|+0000\n"
                "apnic|JP|ipv4|192.0.2.0|128|20200101|allocated\n"
                "apnic|SG|ipv4|192.0.2.128|128|20200101|allocated\n")
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source"
            source.write_text(text)
            with mock.patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(updater.main(["--region", "JP,SG", "--family", "ipv4",
                    "--source", str(source), "--output", tmp]), 0)
            lines = (Path(tmp) / "jp-sg.cidr").read_text().splitlines()
            self.assertEqual([line for line in lines if not line.startswith("#")], ["192.0.2.0/24"])


if __name__ == "__main__":
    unittest.main()
