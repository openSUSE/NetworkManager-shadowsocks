# Translations

All C and Python source messages use English gettext msgids. Human translations belong in
PO catalogs; do not translate configuration keys, command names, cipher names,
addresses, CIDRs, or the DNS helper's `READY` protocol marker.

Meson compiles the locales listed in `LINGUAS` into MO files and installs them
under `${localedir}/<locale>/LC_MESSAGES/NetworkManager-shadowsocks.mo`.
The executables initialize the locale from the environment. The editor module
uses its own gettext domain without changing the host's locale or text domain.
Gettext honors `LC_ALL`, `LC_MESSAGES`, `LANG` and `LANGUAGE`, with English
fallback when a translation is unavailable. The `C`/`C.UTF-8` locales use English.
Tests for other locales are skipped if those locales are not installed.

Update catalogs after changing source messages:

```sh
meson compile -C build NetworkManager-shadowsocks-pot
meson compile -C build NetworkManager-shadowsocks-update-po
msgfmt --check --check-format -o /dev/null po/zh_CN.po
```

Add a language by creating `<locale>.po` from the POT template and adding the
locale to `LINGUAS`. Keep format placeholders intact. Source paths for message
extraction are listed in `POTFILES`. No network access is needed for this process.
