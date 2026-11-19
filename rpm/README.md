# openSUSE RPM packaging

`NetworkManager-shadowsocks.spec` targets openSUSE releases providing
Meson >= 0.61, GLib >= 2.66, libnm >= 1.20 and Python >= 3.9 (including current
Tumbleweed). It uses the distribution's Meson and language packaging macros.
The license is `GPL-2.0-or-later`, as declared in the project README and COPYING.
No public project URL has been supplied, so Source0 names a local source archive.

The spec produces:

- `NetworkManager-shadowsocks`: C service, CLI, D-Bus policy,
  VPN service metadata and bundled China CIDRs.
- `NetworkManager-shadowsocks-gnome`: toolkit-neutral loader, separate GTK 3/GTK 4 editor modules and GNOME VPN authentication helper.
- `NetworkManager-shadowsocks-tools`: standalone Python regional dataset updater;
  architecture-independent.
- `NetworkManager-shadowsocks-lang`: compiled gettext catalogs.

The NetworkManager metadata directory is `/usr/lib/NetworkManager/VPN`, even on
64-bit systems; the editor module uses the architecture-specific library path.
The bundled snapshot is built and installed offline. No scriptlet restarts
NetworkManager, activates a VPN or downloads updated data.

Create a source archive from the project root (adjust the version when needed):

```sh
mkdir -p /tmp/nmss-rpmbuild/SOURCES
tar --exclude=__pycache__ --exclude='*.pyc' \
  --transform='s,^,NetworkManager-shadowsocks-0.1.0/,' \
  -cJf /tmp/nmss-rpmbuild/SOURCES/NetworkManager-shadowsocks-0.1.0.tar.xz \
  COPYING README.md README-zh_CN.md DESIGN.md meson.build meson_options.txt \
  src tools po data examples tests rpm
rpmbuild -ba --define '_topdir /tmp/nmss-rpmbuild' \
  rpm/NetworkManager-shadowsocks.spec
```

Install the declared BuildRequires before invoking rpmbuild. The default check
phase runs offline unit, translation, updater and Xvfb editor tests. It does not
run network namespace tests or contact a Shadowsocks server. Pass
`--without tests` to disable the check phase and its locale/Xvfb dependencies.

UI tests also require `gdk-pixbuf-loader-rsvg` and `shared-mime-info` to load
GTK's built-in Adwaita SVG assets in minimal build workers. Leap needs the
SVG loader explicitly; on current Tumbleweed this capability is provided by
`glycin-loaders`.

The backend package must provide an `sslocal` binary built with `local-tun`.
After package installation, let NetworkManager rediscover VPN plugins at a
convenient time; this package deliberately does not interrupt active networking.

The backend uses the existing `shadowsocks` account supplied by the Shadowsocks
package. The service requires util-linux `setpriv` to drop the backend and plugin
UID/GID while retaining only `CAP_NET_ADMIN` for TUN and socket marks.
