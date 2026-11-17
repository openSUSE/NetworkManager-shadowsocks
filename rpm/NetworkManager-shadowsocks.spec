#
# spec file for package NetworkManager-shadowsocks
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

%bcond_without tests
%define vpnservicedir %{_prefix}/lib/NetworkManager/VPN

Name:           NetworkManager-shadowsocks
Version:        0.1.0
Release:        0
Summary:        Shadowsocks transparent VPN support for NetworkManager
License:        GPL-2.0-or-later
Group:          Productivity/Networking/System
# The project does not yet publish release tarballs at a public URL.
Source0:        %{name}-%{version}.tar.xz
BuildRequires:  gcc
BuildRequires:  gettext-tools
BuildRequires:  meson >= 0.61
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  python3-base >= 3.9
BuildRequires:  pkgconfig(gio-unix-2.0) >= 2.66
BuildRequires:  pkgconfig(gmodule-2.0)
BuildRequires:  pkgconfig(gtk+-3.0) >= 3.22
BuildRequires:  pkgconfig(gtk4) >= 4.0
BuildRequires:  pkgconfig(json-glib-1.0) >= 1.6
BuildRequires:  pkgconfig(libnm) >= 1.20
%if %{with tests}
BuildRequires:  /usr/bin/dbus-run-session
BuildRequires:  dejavu-fonts
# GTK's Adwaita check/radio assets need SVG decoding during UI tests.
BuildRequires:  gdk-pixbuf-loader-rsvg
BuildRequires:  glibc-locale
BuildRequires:  shared-mime-info
BuildRequires:  xorg-x11-server-Xvfb
%endif
Requires:       NetworkManager >= 1.20
Requires:       iproute2
Requires:       shadowsocks-rust
Recommends:     %{name}-tools = %{version}-%{release}
Suggests:       %{name}-gnome = %{version}-%{release}

%description
A NetworkManager VPN plugin using the shadowsocks-rust TUN backend for
transparent TCP and UDP proxying. It supports separate TCP and UDP nodes,
IPv4 and IPv6, configurable DNS, LAN bypass and regional IP bypass. A bundled
dual-stack China allocation snapshot is included for offline use.

%package gnome
Summary:        GTK editor for NetworkManager Shadowsocks connections
Group:          Productivity/Networking/System
Requires:       %{name} = %{version}-%{release}

%description gnome
The native GTK 3 and GTK 4 VPN editor modules for NetworkManager Shadowsocks. They support
manual node configuration, JSON import, separate TCP and UDP nodes, DNS
settings and custom bypass CIDR lists.

%package tools
Summary:        Regional IP allocation dataset updater
Group:          Productivity/Networking/System
BuildArch:      noarch
Requires:       ca-certificates
Requires:       python3-base >= 3.9

%description tools
A standalone Python standard-library tool for downloading and converting RIR
allocation statistics into IPv4 and IPv6 CIDR lists. It supports custom
country/region selections, all five RIRs and offline conversion. This tool does
not modify system routes or NetworkManager connections.

%lang_package

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Dsslocal_binary=%{_bindir}/sslocal \
    -Dip_binary=%{_sbindir}/ip \
    -Dvpn_service_dir=%{vpnservicedir} \
    -Dnetwork_tests=false
%meson_build

%install
%meson_install

# The updater owns generated state; package only the empty parent directories.
install -d -m 0755 %{buildroot}%{_localstatedir}/lib/%{name}/china

%find_lang %{name}

%check
%if %{with tests}
export LC_ALL=C.UTF-8
export GSETTINGS_BACKEND=memory
%meson_test
# Private network namespace tests stay disabled in distribution build workers.
# Run the native editor test with a private display instead of skipping its UI.
sh tests/run-ui.sh %{_vpath_builddir}/test-editor
sh tests/run-ui.sh %{_vpath_builddir}/test-editor-gtk4
%endif

%files -f %{name}.lang
%license COPYING
%doc README.md README-zh_CN.md
%{_bindir}/nm-shadowsocks
%{_libexecdir}/nm-shadowsocks-service
%{vpnservicedir}/nm-shadowsocks-service.name
%{_datadir}/dbus-1/system.d/org.freedesktop.NetworkManager.shadowsocks.conf
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/china/
%dir %{_localstatedir}/lib/%{name}
%dir %{_localstatedir}/lib/%{name}/china

%files gnome
%{_libexecdir}/nm-shadowsocks-auth-dialog
%{_libdir}/NetworkManager/libnm-vpn-plugin-shadowsocks.so
%{_libdir}/NetworkManager/libnm-vpn-plugin-shadowsocks-editor.so
%{_libdir}/NetworkManager/libnm-gtk4-vpn-plugin-shadowsocks-editor.so

%files tools
%license COPYING
%doc README.md README-zh_CN.md
%{_bindir}/nm-shadowsocks-update-routes

%changelog
