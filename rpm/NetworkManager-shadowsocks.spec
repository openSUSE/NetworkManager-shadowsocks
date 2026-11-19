#
# spec file for package NetworkManager-shadowsocks
#
# Copyright (c) 2026 SUSE LLC and contributors
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via https://bugs.opensuse.org/
#

%bcond_without tests
%define vpnservicedir %{_prefix}/lib/NetworkManager/VPN

Name:           NetworkManager-shadowsocks
Version:        1.0.0
Release:        0
Summary:        Shadowsocks transparent VPN support for NetworkManager
License:        GPL-2.0-or-later
URL:            https://github.com/openSUSE/NetworkManager-shadowsocks
Source0:        https://github.com/openSUSE/NetworkManager-shadowsocks/archive/%{version}/%{name}-%{version}.tar.gz
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
BuildRequires: /usr/bin/dbus-run-session
BuildRequires:  dejavu-fonts
BuildRequires:  gdk-pixbuf-loader-rsvg
BuildRequires:  glibc-locale
BuildRequires:  shared-mime-info
BuildRequires:  xorg-x11-server-Xvfb
%endif
Requires:       NetworkManager >= 1.20
Requires:       iproute2
Requires:       shadowsocks-rust
Requires:       /usr/bin/setpriv
Supplements:    (NetworkManager and shadowsocks-rust)
Recommends:     %{name}-lang = %{version}
Recommends:     %{name}-tools = %{version}-%{release}

%description
A NetworkManager VPN plugin using the shadowsocks-rust TUN backend for
transparent TCP and UDP proxying. It supports separate TCP and UDP nodes,
IPv4 and IPv6, configurable DNS, LAN bypass and China IP bypass. A bundled
dual-stack China allocation snapshot is included for offline use.

%package -n NetworkManager-applet-shadowsocks
Summary:        GTK editor for NetworkManager Shadowsocks connections
Group:          Productivity/Networking/System
Requires:       %{name} = %{version}-%{release}
Supplements:    (%{name} and NetworkManager-applet)
Supplements:    (%{name} and gnome-control-center)

%description -n NetworkManager-applet-shadowsocks
The native GTK 3 VPN editor plugin for NetworkManager Shadowsocks. It supports
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
country/region selections, all five RIRs and offline conversion. The legacy
nm-shadowsocks-update-china command remains available as a compatibility entry
point. This tool does not modify system routes or NetworkManager connections.

%lang_package

%prep
%autosetup -p1

%build
%meson \
    -Dsslocal_binary=%{_bindir}/sslocal \
    -Dip_binary=%{_sbindir}/ip \
    -Dvpn_service_dir=%{vpnservicedir} \
    -Dnetwork_tests=false
%meson_build

%install
%meson_install

# Move Meson's documentation to the openSUSE documentation directory.
install -d %{buildroot}%{_docdir}
mv %{buildroot}%{_datadir}/doc/%{name} %{buildroot}%{_docdir}/%{name}

# The updater owns generated state; package only the empty parent directories.
install -d -m 0755 %{buildroot}%{_localstatedir}/lib/%{name}/china

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

%find_lang %{name}

%files
%license COPYING
%doc %{_docdir}/%{name}
%{_bindir}/nm-shadowsocks
%{_libexecdir}/nm-shadowsocks-service
%{vpnservicedir}/nm-shadowsocks-service.name
%{_datadir}/dbus-1/system.d/org.freedesktop.NetworkManager.shadowsocks.conf
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/china/
%dir %{_localstatedir}/lib/%{name}
%dir %{_localstatedir}/lib/%{name}/china

%files -n NetworkManager-applet-shadowsocks
%license COPYING
%{_libexecdir}/nm-shadowsocks-auth-dialog
%{_libdir}/NetworkManager/libnm-vpn-plugin-shadowsocks.so
%{_libdir}/NetworkManager/libnm-vpn-plugin-shadowsocks-editor.so
%{_libdir}/NetworkManager/libnm-gtk4-vpn-plugin-shadowsocks-editor.so

%files tools
%license COPYING
%doc README.md
%{_bindir}/nm-shadowsocks-update-routes

%files lang -f %{name}.lang

%changelog
