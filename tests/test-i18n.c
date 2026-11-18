/*
 * test-i18n.c
 * This file is part of NetworkManager-shadowsocks
 *
 * Copyright (C) 2026 - Hillwood Yang
 *
 * NetworkManager-shadowsocks is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * NetworkManager-shadowsocks is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NetworkManager-shadowsocks. If not, see <http://www.gnu.org/licenses/>.
 */


#include "ss.h"
#include <locale.h>
#include <string.h>

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 3);
    
    if (!setlocale(LC_ALL, "")) {
        g_print("Requested locale is not installed; skipping.\n");
        return 77;
    }
    
    gboolean translated = !strcmp(argv[2], "translated");
    g_autofree char *host_domain = g_strdup(textdomain(NULL));
    ss_i18n_init();
    g_assert_cmpstr(textdomain(NULL), ==, host_domain);
    /* Test the same compiled catalogs that Meson installs, without installing
     * anything into the host's locale directory. */
    bindtextdomain(GETTEXT_PACKAGE, argv[1]);
    const char *server = _("Server");
    g_assert_true(g_utf8_validate(server, -1, NULL));
    if (translated) g_assert_cmpstr(server, !=, "Server");
    else g_assert_cmpstr(server, ==, "Server");
    g_autoptr(GError) error = NULL;
    g_autoptr(SsProfile) profile = ss_profile_parse("[]", &error);
    g_assert_null(profile); g_assert_nonnull(error);
    g_assert_cmpstr(error->message, ==, _("Configuration must be a JSON object"));
    if (translated) g_assert_cmpstr(error->message, !=, "Configuration must be a JSON object");
    g_autofree char *counts = g_strdup_printf(_("Regional prefixes: IPv4 %u / IPv6 %u - Import custom list..."), 8798, 2048);
    g_assert_nonnull(strstr(counts, "8798")); g_assert_nonnull(strstr(counts, "2048"));
    g_print("%s\n%s\n", server, error->message);
    return 0;
}
