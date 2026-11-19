# NetworkManager Shadowsocks

[简体中文](README-zh_CN.md)

A Linux NetworkManager VPN plugin built with **C / Meson / libnm / GTK 3 + GTK 4**, using **shadowsocks-rust's sslocal TUN** backend.

## Features

- Edit nodes manually or import standard Shadowsocks JSON, shadowsocks-rust multi-server JSON, and SIP008 JSON. Select a node by index when importing a file with multiple nodes.
- Use **separate TCP and UDP nodes**, or share one node. Each node must support its corresponding protocol.
- Global proxy, LAN bypass, or regional IP bypass (defaults to mainland China IPs) plus LAN bypass. Includes an updatable mainland China IPv4/IPv6 dataset and supports additional bypass CIDRs.
- Configurable DNS: UDP DNS uses the UDP node, and TCP DNS uses the TCP node. Configured DNS IPs always go through the proxy.
- IPv4, IPv6, or dual stack. Single-family modes block non-exempt TCP/UDP traffic of the other address family.
- Native `NMVpnEditorPlugin` / `NMVpnEditor` integration with `nm-connection-editor` and `nmcli connection import`.

## Building and installing

An openSUSE RPM spec template is available at [rpm/NetworkManager-shadowsocks.spec](rpm/NetworkManager-shadowsocks.spec). See [rpm/README.md](rpm/README.md) for source archive and build instructions. Other RPM-based distributions can use this template as a starting point.

Build dependencies: a C compiler, Meson ≥ 0.61, Ninja, gettext tools, Python ≥ 3.9, and development packages for libnm ≥ 1.20, GLib/GIO ≥ 2.66, JSON-GLib ≥ 1.6, GTK 3 ≥ 3.22, and GTK 4 ≥ 4.0. Runtime requirements are Linux TUN, iproute2 with `ip rule ipproto` support, NetworkManager, and shadowsocks-rust's `sslocal` built with `local-tun` enabled. The main plugin is written in C; only the standalone dataset updater and its tests use the Python standard library.

```sh
meson setup build --prefix=/usr
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

## Usage

Run `nm-connection-editor` and add a VPN → **Shadowsocks** connection. Enter separate TCP and UDP nodes, or select the option to reuse the TCP node for UDP. To import JSON, select the node index on the relevant node tab. Choose the proxy mode, address family, and DNS servers. Regional bypass mode uses the bundled mainland China dual-stack dataset by default, so no external list is required. The editor displays the IPv4/IPv6 prefix counts and also accepts custom CIDR files. Selected prefixes are saved in the connection; activation does not read the original file.

The settings window may also show generic IPv4/IPv6 address configuration options that do not apply to this plugin. These are part of NetworkManager's generic VPN editor interface and cannot be hidden by the plugin; you can ignore them.

Import separate nodes from the command line:

```sh
nm-shadowsocks import --name 'My transparent proxy' \
  --tcp tcp-node.json --udp udp-node.json \
  --mode lan --ip-mode dual --dns '1.1.1.1,2606:4700:4700::1111'
nm-shadowsocks list
nmcli connection up uuid '<UUID from the previous command>'
nmcli connection down uuid '<UUID>'
```

Omit `--udp` to reuse the TCP node. `--tcp-index` and `--udp-index` are zero-based. Add `--uuid UUID` to update an existing connection, or edit it in the native editor.

```sh
# Export locally without writing to NetworkManager. Includes passwords; mode 0600.
nm-shadowsocks import --tcp tcp.json --udp udp.json --output my-profile.json
# Validate offline or print the routing plan. Neither requires root or changes networking.
nm-shadowsocks validate my-profile.json
nm-shadowsocks plan my-profile.json
# Save the complete configuration to NetworkManager.
nm-shadowsocks apply my-profile.json
# Native import also accepts complete profiles or single-node files.
nmcli connection import type shadowsocks file my-profile.json
```

For regional bypass (defaults to mainland China IPs), add `--mode china` when importing to load the default dataset automatically. Use `--china-cidrs FILE` to override it. The complete bundled data is in `data/china/china-ipv4.cidr`, `china-ipv6.cidr`, and the combined `china.cidr`. The current snapshot has APNIC version **20261113**, with **8,798 IPv4 prefixes and 2,048 IPv6 prefixes**. `examples/regional-example.cidr` is only a format example.

Update the dataset without changing system routes:

```sh
nm-shadowsocks-update-routes          # Update the current user's dataset.
sudo nm-shadowsocks-update-routes     # Update the system dataset for all users.
# Convert an offline file or refresh the bundled source snapshot:
nm-shadowsocks-update-routes --source delegated-apnic-latest --output data/china
```

The updater uses Python's standard-library `urllib.request` and `ipaddress` modules to download data from APNIC over HTTPS; curl and pip dependencies are not required. It validates the CN dual-stack records before atomically replacing the combined dataset. Download or parsing failures leave the previous data intact. Lookup precedence is the user dataset, then the system dataset, then the installed snapshot. Building, installing, and activating the VPN never download data automatically. Existing connections retain their saved prefixes. To use updated data, click “Use latest default prefixes” in the editor, save, and reconnect. For IPv4-only proxying, use `--ip-mode ipv4 --dns 1.1.1.1`; use IPv6 DNS addresses for IPv6-only mode.

The default prefixes come from APNIC's `CN` allocation records, excluding HK/MO/TW. Allocation records do not guarantee physical location and are not live BGP routes. See the [dataset documentation](data/china/README.md) for sources, licensing, and metadata.

### Custom regions

Use `--region` (alias `--regions`) to select two-letter country/region codes, separated by commas or supplied repeatedly. The default is `CN`. Custom regions, address families, or registries require `--output` to avoid overwriting the default mainland China dataset.

```sh
# Japan and Singapore, using APNIC records.
nm-shadowsocks-update-routes --region JP,SG --output ./regional-routes
# US allocations held by ARIN.
nm-shadowsocks-update-routes --region US --registry arin --output ./us-routes
# Merge all five RIRs for the selected regions.
nm-shadowsocks-update-routes --region CN,HK,MO --registry all --output ./regional-routes
# Export only IPv4; useful when a region has no IPv6 allocations.
nm-shadowsocks-update-routes --region JP --family ipv4 --output ./jp-routes
```

`--registry` accepts `apnic` (default), `arin`, `ripencc`, `lacnic`, `afrinic`, and `all`. A single source covers only allocations reported by that registry. Use `all` to combine records for the same country/region across registries. For offline conversion, repeat `--source FILE`. When `--registry` is omitted or set to `all`, the registry is identified from each file's header, and only the supplied files are processed. Updates are rejected for duplicate registries, no matching regions, incomplete data, or missing required address families. `--family` accepts `dual` (default), `ipv4`, and `ipv6`. Custom results coalesce adjacent prefixes and enforce a limit on the final prefix count.

For example, `JP,SG` produces `jp-sg.cidr`, `jp-sg-ipv4.cidr`, `jp-sg-ipv6.cidr`, and `jp-sg.json`. Metadata records the selected regions, address families, registry versions, and checksum. Region codes are sorted alphabetically, so their input order does not affect filenames. The default CN/APNIC/dual-stack selection continues to produce compatible `china.*` files; a custom multi-registry CN selection produces `cn.*`. Generated files do not automatically change connections or system routes. Import them in the editor or configure them as additional bypass prefixes:

```sh
nm-shadowsocks import --tcp node.json --mode lan --bypass-cidrs ./regional-routes/jp-sg.cidr
```

Selected regions connect **directly**; other non-exempt TCP/UDP traffic uses the proxy. This does not mean “proxy only these regions.” The updater command is `nm-shadowsocks-update-routes`, and it updates CN prefixes by default. Source strings remain in English, with translations provided through PO files.

## Behavior and limitations

The NetworkManager service runs as root to manage routes. It launches `sslocal` and SIP003 plugins as the existing `shadowsocks` system account using util-linux `setpriv`, with supplementary groups cleared and only `CAP_NET_ADMIN` retained for TUN and socket marks. Plugins inherit this capability too. Privilege escalation through setuid programs or file capabilities is disabled. A missing account or failed privilege transition prevents startup; there is no root fallback. openSUSE supplies the account with Shadowsocks. Other distributions must provision it, or select an existing non-root account at build time with `-Dservice_user=NAME`.

- Proxies local TCP/UDP traffic, including UDP/QUIC. This is not a router or gateway configuration tool. It does not enable forwarding, modify the firewall, or proxy protocols such as raw ICMP. Ping cannot determine whether a proxy node works.
- Global mode still bypasses essential system destinations such as loopback, link-local, and multicast addresses. LAN mode additionally bypasses private networks and CGNAT. Regional mode also bypasses the selected regional CIDRs, which default to mainland China IPs.
- Node hostnames are resolved over the original network before routes are installed, and the backend uses those resolved addresses. Reconnect if node IPs or the underlying network change. The transport address family depends on the node and physical network, independently of the proxied destination's address family.
- DNS addresses are published directly through NetworkManager, with priority -5353 and routing domain `~.`. No local DNS forwarder is started, and nothing listens on local port 53. One TUN receives both TCP and UDP; the backend's two server entries are restricted to `tcp_only` and `udp_only`, respectively, so system resolvers bound to the VPN interface can still use the correct node. Application-managed DoH follows ordinary TCP/UDP rules; this cannot force applications to use the configured DNS servers.
- Passwords are stored as NetworkManager VPN secrets, owned by the system for unattended use, rather than in `vpn.data`. Runtime configuration under `/run` remains root-owned: mode-0640 files inside a mode-0710 directory allow the backend group to read the configuration without modifying it. Exported files contain passwords, and export refuses to overwrite existing files or symbolic links. A GNOME VPN authentication helper receives saved passwords from NetworkManager; GNOME Shell displays a password dialog when interaction is needed.
- Only the listed AEAD / Shadowsocks 2022 ciphers are supported. `ss://` links, subscription downloads, and JSON5 are not currently supported. Imports do not inherit listeners or ACLs from the source file.
- Only one connection using this plugin can be active at a time. It reserves interface `nmss-tun`, routing tables 51820/51821, rule priorities 10500/10501/10502/10503/10510/10511, and SO_MARK 0x5353. Activation is refused if these resources are occupied. Higher-priority rules from custom policy routing or other VPNs may alter the actual path and require administrator coordination.
- Normal disconnection, startup failure, and SIGTERM roll back routes and terminate the backend. There is **no persistent kill switch**: disconnecting restores the original network. Policies left behind after SIGKILL or a service crash require administrator cleanup; see DESIGN.md. Successful activation means the local tunnel is established, not that the remote password or network connectivity has been verified.

See [DESIGN.md](DESIGN.md) for implementation details, routing order, and the acceptance checklist.

### GNOME Settings editor compatibility

The editor entry module does not link against GTK. It selects a separate GTK 3 or GTK 4 UI module according to the host process. Modern GNOME Settings uses GTK 4, while `nm-connection-editor` uses GTK 3; neither loads the other GTK generation into the same process. When upgrading, install the entry module and both UI modules together, then fully exit and reopen Settings. Update the authentication helper and VPN descriptor at the same time: a missing authentication entry causes GNOME Shell password requests to fail or time out. If GNOME Shell still caches the old descriptor after installation, log out and back in.

### SIP003 obfuscation plugins

Both GTK editors provide separate obfuscation plugin, plugin options, and extra arguments (a JSON array) fields for the TCP and UDP nodes. Leave the plugin blank to disable it. Examples include `obfs-local` with `obfs=http;obfs-host=www.example.com`, or `v2ray-plugin` with options matching the server. Node JSON imports accept `plugin`, `plugin_opts`, and `plugin_args`, including through the CLI. Plugin transport can be TCP only, UDP only, or TCP and UDP; the selected value is passed to the backend and defaults to TCP only when omitted. Select a mode containing UDP only if the plugin supports a UDP input.

```json
{
  "server": "ss.example.com",
  "server_port": 443,
  "method": "aes-256-gcm",
  "password": "replace-me",
  "plugin": "obfs-local",
  "plugin_opts": "obfs=tls;obfs-host=www.example.com",
  "plugin_args": [],
  "plugin_mode": "tcp_only"
}
```

With plugin transport set to `tcp_only`, the UDP node still proxies application UDP datagrams, but sends them through the node's native Shadowsocks UDP port rather than the obfuscation plugin. `mode=quic` describes the plugin's own outer transport and does not change this behavior. Install plugin executables separately, and ensure the backend has SIP003 support enabled. Plugin options and extra arguments are saved with the connection configuration so the editor can restore them without loading secrets. Tokens or other sensitive values in these fields have the same visibility as the connection configuration. Node passwords remain NetworkManager secrets; exported JSON includes these values.

The service permits system-installed `obfs-local`, `v2ray-plugin`, `xray-plugin`, `kcptun-client`, and `shadow-tls`. It searches `/usr/bin` and `/usr/local/bin` by name; absolute paths are also accepted. For custom plugins, an administrator must place an executable or symbolic link in `/usr/libexec/NetworkManager/shadowsocks/plugins/`, then specify its name or full path within that directory. The executable and its path must be root-owned and not writable by the group or other users. The service does not search the user's PATH or execute arguments through a shell.

Plugin server addresses are resolved before startup. The IPv4/IPv6 addresses actually used are automatically routed directly to prevent the plugin's own traffic from looping through the TUN; other TCP/UDP traffic to those addresses also bypasses the proxy. Upstream DNS must not share an address with a plugin server. For TLS/WebSocket plugins that depend on hostnames, configure their host/SNI options explicitly, since the backend receives the server IP resolved at startup. Disconnection terminates the entire backend process group, including plugin subprocesses.
