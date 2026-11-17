#!@PYTHON@
"""Download RIR allocations and atomically update regional IP datasets."""

import argparse
import datetime
import gettext
import hashlib
import ipaddress
import json
import locale
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

DOMAIN = "NetworkManager-shadowsocks"
LOCALEDIR = "@LOCALEDIR@"
STATE_DIR = "@STATE_DIR@"
APNIC_URL = "https://ftp.apnic.net/stats/apnic/delegated-apnic-latest"
REGISTRIES = {
    "apnic": APNIC_URL,
    "arin": "https://ftp.arin.net/pub/stats/arin/delegated-arin-extended-latest",
    "ripencc": "https://ftp.ripe.net/pub/stats/ripencc/delegated-ripencc-latest",
    "lacnic": "https://ftp.lacnic.net/pub/stats/lacnic/delegated-lacnic-extended-latest",
    "afrinic": "https://ftp.afrinic.net/pub/stats/afrinic/delegated-afrinic-latest",
}
MAX_BYTES = 32 * 1024 * 1024
MAX_PREFIXES = 100000


def translator():
    """Use the same locale and message domain as the C executables."""
    try:
        locale.setlocale(locale.LC_ALL, "")
    except locale.Error:
        return gettext.NullTranslations().gettext
    language = locale.setlocale(locale.LC_MESSAGES)
    if language.split(".", 1)[0] in ("C", "POSIX"):
        return gettext.NullTranslations().gettext
    directory = LOCALEDIR
    if directory.startswith("@"):
        directory = str(Path(__file__).resolve().parent.parent / "build" / "po")
    return gettext.translation(DOMAIN, localedir=directory, fallback=True).gettext


_ = translator()


def invalid(message):
    return ValueError(_("Invalid RIR data: %s") % message)


def normalize_regions(values):
    regions = set()
    for value in values:
        for code in value.split(","):
            code = code.strip().upper()
            if not re.fullmatch(r"[A-Z]{2}", code):
                raise ValueError(_("Regions must be two-letter country/region codes"))
            regions.add(code)
    if not regions:
        raise ValueError(_("Specify at least one region"))
    return tuple(sorted(regions))


def parse_registry(text, regions=("CN",), registry=None, family="dual"):
    if not text or len(text.encode("utf-8")) > MAX_BYTES:
        raise invalid(_("Empty file or more than 32 MiB"))
    serial = None
    declared = records = 0
    prefixes = set()
    coverage = {code: set() for code in regions}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) >= 7 and fields[0] in ("2", "2.3") and fields[1] in REGISTRIES:
            if (serial is not None or not re.fullmatch(r"[0-9]{1,20}", fields[2])
                    or not re.fullmatch(r"[0-9]+", fields[3]) or int(fields[3]) == 0):
                raise invalid(_("Invalid version date, record count or duplicate version header"))
            if fields[1] == "apnic":
                try:
                    if len(fields[2]) != 8:
                        raise ValueError()
                    datetime.datetime.strptime(fields[2], "%Y%m%d")
                except ValueError:
                    raise invalid(_("Invalid version date, record count or duplicate version header")) from None
            serial, declared = fields[2], int(fields[3])
            if registry is not None and registry != fields[1]:
                raise invalid(_("Registry header does not match the selected source"))
            registry = fields[1]
            continue
        if len(fields) >= 7 and fields[0] == registry:
            records += 1
        if len(fields) < 2 or fields[0] != registry or fields[1] not in regions:
            continue
        if len(fields) < 7:
            raise invalid(_("Selected region record has missing fields"))
        if fields[2] not in ("ipv4", "ipv6") or fields[6] not in ("allocated", "assigned"):
            continue
        if not re.fullmatch(r"[0-9]+", fields[4]):
            raise invalid(_("Invalid address count or prefix length"))
        value = int(fields[4])
        version = 4 if fields[2] == "ipv4" else 6
        try:
            address = ipaddress.ip_address(fields[3])
        except ValueError:
            raise invalid(_("Invalid address family")) from None
        if address.version != version:
            raise invalid(_("Invalid address family"))
        if version == 4:
            if value < 1 or value > 2**32 - int(address):
                raise invalid(_("IPv4 range overflow"))
            blocks = ipaddress.summarize_address_range(
                address, ipaddress.IPv4Address(int(address) + value - 1))
        else:
            if not 1 <= value <= 128:
                raise invalid(_("IPv6 prefix length must be between 1 and 128"))
            try:
                blocks = [ipaddress.IPv6Network((address, value), strict=True)]
            except ValueError:
                raise invalid(_("IPv6 prefix is not aligned")) from None
        coverage[fields[1]].add(version)
        if family != "dual" and family != f"ipv{version}":
            continue
        for block in blocks:
            if not block.prefixlen:
                raise invalid(_("Bypass ranges must not include a /0 default route"))
            prefixes.add(str(block))
        if len(prefixes) > MAX_PREFIXES * 10:
            raise invalid(_("Too many network prefixes"))
    if serial is None or records != declared:
        raise invalid(_("Missing RIR header or mismatched record count; file may be incomplete"))
    return sorted(prefixes), serial, registry, coverage


def validate_coverage(coverage, regions, family):
    required = {4, 6} if family == "dual" else {int(family[-1])}
    missing = [code for code in regions if not required.issubset(coverage[code])]
    if missing:
        raise invalid(_("No records for the requested address families in regions: %s") % ",".join(missing))


def parse_apnic(text, regions=("CN",), family="dual"):
    prefixes, serial, _, coverage = parse_registry(text, regions, "apnic", family)
    validate_coverage(coverage, regions, family)
    if len(prefixes) > MAX_PREFIXES:
        raise invalid(_("Too many network prefixes"))
    return prefixes, serial


class HTTPSRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        if urllib.parse.urlsplit(new_url).scheme.lower() != "https":
            raise urllib.error.URLError(_("Refusing a redirect to a non-HTTPS URL"))
        return super().redirect_request(request, response, code, message, headers, new_url)


def read_bounded(stream, deadline=None):
    data = bytearray()
    while True:
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError(_("Download timed out"))
        chunk = stream.read(min(65536, MAX_BYTES + 1 - len(data)))
        if not chunk:
            break
        data.extend(chunk)
        if len(data) > MAX_BYTES:
            raise invalid(_("Empty file or more than 32 MiB"))
    text = data.decode("utf-8")
    if "\0" in text:
        raise invalid(_("File contains NUL bytes"))
    return text


def download(registry="apnic"):
    request = urllib.request.Request(REGISTRIES[registry], headers={"User-Agent": DOMAIN})
    opener = urllib.request.build_opener(HTTPSRedirectHandler())
    deadline = time.monotonic() + 90
    try:
        with opener.open(request, timeout=15) as response:
            return read_bounded(response, deadline)
    except (OSError, urllib.error.URLError, ValueError) as error:
        raise OSError(_("Download failed; keeping the old dataset: %s") % error) from error


def replace_file(path, text):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, prefix=".routes-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o644)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_dataset(prefixes, serial, directory, regions=("CN",), family="dual", sources=None):
    if sources is None:
        sources = [{"registry": "apnic", "url": APNIC_URL, "serial": serial}]
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    legacy = regions == ("CN",) and family == "dual" and (
        sources is None or len(sources) == 1 and sources[0]["registry"] == "apnic")
    basename = "china" if legacy else "-".join(regions).lower()
    header = (f"# Source: {APNIC_URL}\n"
              f"# APNIC serial: {serial}; country: CN; status: allocated/assigned\n"
              "# Allocation-country data, not authoritative IP geolocation or BGP routes.\n")
    if not legacy:
        header = ("# Sources: " + ",".join(source["url"] for source in sources) + "\n"
                  "# Countries/regions: " + ",".join(regions) + "; family: " + family + "\n"
                  "# Allocation-country data, not authoritative IP geolocation or BGP routes.\n")
    ipv4 = [prefix for prefix in prefixes if ":" not in prefix]
    ipv6 = [prefix for prefix in prefixes if ":" in prefix]
    combined = header + "".join(prefix + "\n" for prefix in prefixes)
    metadata = {"source": APNIC_URL, "serial": serial, "ipv4_prefixes": len(ipv4),
                "ipv6_prefixes": len(ipv6),
                "sha256": hashlib.sha256(combined.encode("utf-8")).hexdigest()}
    if not legacy:
        metadata = {"regions": list(regions), "family": family, "sources": sources,
                    "ipv4_prefixes": len(ipv4), "ipv6_prefixes": len(ipv6),
                    "sha256": hashlib.sha256(combined.encode("utf-8")).hexdigest()}
    # The combined file is the reader's commit point. Download, parse and all
    # auxiliary writes must succeed before replacing the old combined dataset.
    replace_file(directory / f"{basename}-ipv4.cidr", header + "".join(p + "\n" for p in ipv4))
    replace_file(directory / f"{basename}-ipv6.cidr", header + "".join(p + "\n" for p in ipv6))
    replace_file(directory / f"{basename}.json", json.dumps(metadata, indent=2) + "\n")
    replace_file(directory / f"{basename}.cidr", combined)
    return directory / f"{basename}.cidr"


def default_directory():
    if os.geteuid() == 0:
        return Path(STATE_DIR if not STATE_DIR.startswith("@")
                    else "/var/lib/NetworkManager-shadowsocks/china")
    data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    if not data_home.is_absolute():
        data_home = Path.home() / ".local/share"
    return data_home / "NetworkManager-shadowsocks/china"


def main(argv=None):
    parser = argparse.ArgumentParser(description=_("Download and update regional IPv4/IPv6 prefixes"))
    parser.add_argument("--source", type=Path, metavar="FILE", action="append",
                        help=_("Convert local RIR delegated files (offline; repeatable)"))
    parser.add_argument("--region", "--regions", action="append", metavar="CC[,CC]",
                        help=_("Country/region codes, comma-separated or repeatable (default: CN)"))
    parser.add_argument("--registry", choices=tuple(REGISTRIES) + ("all",),
                        help=_("RIR source (default: apnic); all merges the five registries"))
    parser.add_argument("--family", choices=("dual", "ipv4", "ipv6"), default="dual",
                        help=_("Address families to export (default: dual)"))
    parser.add_argument("--output", "-o", type=Path, metavar="DIR",
                        help=_("Output directory (default: user data directory, or system state directory for root)"))
    args = parser.parse_args(argv)
    try:
        regions = normalize_regions(args.region if args.region is not None else ["CN"])
        legacy = regions == ("CN",) and args.family == "dual" and args.registry in (None, "apnic")
        if not legacy and args.output is None:
            raise ValueError(_("Custom regions or sources require --output to preserve the default China dataset"))
        documents = []
        if args.source:
            for source in args.source:
                with source.open("rb") as stream:
                    documents.append(parse_registry(read_bounded(stream), regions,
                        args.registry if args.registry != "all" else None, args.family))
        else:
            registries = tuple(REGISTRIES) if args.registry == "all" else (args.registry or "apnic",)
            for registry in registries:
                documents.append(parse_registry(download(registry), regions, registry, args.family))
        coverage = {code: set() for code in regions}
        prefixes = set()
        sources = []
        seen = set()
        for selected, serial, registry, found in documents:
            if registry in seen:
                raise invalid(_("Duplicate registry input: %s") % registry)
            seen.add(registry)
            prefixes.update(selected)
            for code in regions:
                coverage[code].update(found[code])
            sources.append({"registry": registry, "url": REGISTRIES[registry], "serial": serial})
        validate_coverage(coverage, regions, args.family)
        if not legacy or seen != {"apnic"}:
            networks = [ipaddress.ip_network(prefix) for prefix in prefixes]
            prefixes = {str(prefix) for version in (4, 6)
                        for prefix in ipaddress.collapse_addresses(
                            network for network in networks if network.version == version)}
        if len(prefixes) > MAX_PREFIXES:
            raise invalid(_("Too many network prefixes"))
        if legacy and seen != {"apnic"} and args.output is None:
            raise ValueError(_("Custom regions or sources require --output to preserve the default China dataset"))
        directory = args.output if args.output is not None else default_directory()
        combined = write_dataset(sorted(prefixes), sources[0]["serial"], directory, regions, args.family, sources)
        print(_("Updated regions %(regions)s: %(path)s\nTotal prefixes: %(count)d\n")
              % {"regions": ",".join(regions), "path": combined, "count": len(prefixes)}, end="")
        if combined.name == "china.cidr":
            print(_("For existing connections, select \"Use latest default prefixes\" in the editor, save and reconnect."))
        return 0
    except (OSError, ValueError, urllib.error.URLError) as error:
        print(_("Error: %s\n") % error, end="", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
