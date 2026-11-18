/*
 * auth-dialog.c
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
#include <stdio.h>
#include <unistd.h>

/* GNOME Shell invokes this unprivileged helper with the VPN agent protocol.
 * Never request secrets from NetworkManager here: that would recurse into the
 * same pending request. System secrets arrive on stdin from NetworkManager. */
int main(int argc, char **argv) {
    char *uuid = NULL, *name = NULL, *service = NULL;
    char **hints = NULL;
    gboolean external = FALSE, allow = FALSE, retry = FALSE;
    
    GOptionEntry options[] = {
        {"uuid", 'u', 0, G_OPTION_ARG_STRING, &uuid, NULL, NULL},
        {"name", 'n', 0, G_OPTION_ARG_STRING, &name, NULL, NULL},
        {"service", 's', 0, G_OPTION_ARG_STRING, &service, NULL, NULL},
        {"external-ui-mode", 0, 0, G_OPTION_ARG_NONE, &external, NULL, NULL},
        {"allow-interaction", 'i', 0, G_OPTION_ARG_NONE, &allow, NULL, NULL},
        {"reprompt", 'r', 0, G_OPTION_ARG_NONE, &retry, NULL, NULL},
        {"hint", 't', 0, G_OPTION_ARG_STRING_ARRAY, &hints, NULL, NULL},
        {NULL}
    };
    
    ss_i18n_init();
    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, options, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(GHashTable) data = NULL;
    g_autoptr(GHashTable) secrets = NULL;
    g_autoptr(GKeyFile) output = g_key_file_new();
    g_autoptr(GString) legacy = g_string_new(NULL);
    
    int result = 1;
    
    if (!g_option_context_parse(context, &argc, &argv, &error) ||
        !uuid || !name || g_strcmp0(service, SS_SERVICE)) goto out;
        
    if (!nm_vpn_service_plugin_read_vpn_details(STDIN_FILENO, &data, &secrets)) goto out;
    
    g_key_file_set_integer(output, "VPN Plugin UI", "Version", 2);
    g_key_file_set_string(output, "VPN Plugin UI", "Title", _("Shadowsocks authentication"));
    g_key_file_set_string(output, "VPN Plugin UI", "Description", name);
    const char *keys[] = {"tcp-password", "udp-password"};
    const char *labels[] = {_("TCP node password"), _("UDP node password")};
    
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *value = g_hash_table_lookup(secrets, keys[i]);
        gboolean ask = allow && (retry || !value || !*value);
        if (!external && ask) goto out;
        g_key_file_set_string(output, keys[i], "Value", value ? value : "");
        g_key_file_set_string(output, keys[i], "Label", labels[i]);
        g_key_file_set_boolean(output, keys[i], "IsSecret", TRUE);
        g_key_file_set_boolean(output, keys[i], "ShouldAsk", ask);
        if (!external && value && *value) {
            if (strchr(value, '\n') || strchr(value, '\r')) goto out;
            g_string_append_printf(legacy, "%s\n%s\n", keys[i], value);
        }
    }
    
    if (external) {
        g_autofree char *text = g_key_file_to_data(output, NULL, NULL);
        fputs(text, stdout);
    } else { fputs(legacy->str, stdout); fputs("\n\n", stdout); }
    
    result = fflush(stdout) == 0 ? 0 : 1;
    
out:
    g_free(uuid);
    g_free(name);
    g_free(service);
    g_strfreev(hints);
    
    return result;
}
