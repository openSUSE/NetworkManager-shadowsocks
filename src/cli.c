/*
 * cli.c
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
#include "regional.h"
#include <string.h>

typedef struct {
    GMainLoop *loop;
    gboolean ok;
    GError *error;
} Wait;
static void added(GObject *source, GAsyncResult *res, gpointer data) {
    Wait *w = data;
    NMRemoteConnection *remote = nm_client_add_connection_finish(NM_CLIENT(source), res, &w->error);
    if (remote) {
        g_print(_("Saved: %s\nActivate: nmcli connection up uuid %s\n"),
                nm_connection_get_id(NM_CONNECTION(remote)),
                nm_connection_get_uuid(NM_CONNECTION(remote)));
        g_object_unref(remote);
        w->ok = TRUE;
    }
    g_main_loop_quit(w->loop);
}

static void updated(GObject *source, GAsyncResult *res, gpointer data) {
    Wait *w = data;
    g_autoptr(GVariant) result =
        nm_remote_connection_update2_finish(NM_REMOTE_CONNECTION(source), res, &w->error);
    w->ok = result != NULL;
    if (w->ok)
        g_print(_("Updated: %s\n"), nm_connection_get_uuid(NM_CONNECTION(source)));
    g_main_loop_quit(w->loop);
}

static gboolean save(SsProfile *p, const char *uuid, GError **error) {
    g_autoptr(NMClient) client = nm_client_new(NULL, error);
    if (!client)
        return FALSE;
    Wait w = {.loop = g_main_loop_new(NULL, FALSE)};
    if (uuid) {
        NMRemoteConnection *remote = nm_client_get_connection_by_uuid(client, uuid);
        NMSettingVpn *vpn = remote ? nm_connection_get_setting_vpn(NM_CONNECTION(remote)) : NULL;
        if (!vpn || g_strcmp0(nm_setting_vpn_get_service_type(vpn), SS_SERVICE)) {
            g_main_loop_unref(w.loop);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("No Shadowsocks connection found with this UUID"));
            return FALSE;
        }
        g_autoptr(NMConnection) copy = nm_simple_connection_new_clone(NM_CONNECTION(remote));
        if (!ss_connection_update(p, copy, error)) {
            g_main_loop_unref(w.loop);
            return FALSE;
        }
        g_object_set(nm_connection_get_setting_connection(copy), NM_SETTING_CONNECTION_ID,
                     ss_string(p->obj, "name"), NULL);
        g_autoptr(GVariant) settings = nm_connection_to_dbus(copy, NM_CONNECTION_SERIALIZE_ALL);
        nm_remote_connection_update2(remote, settings, NM_SETTINGS_UPDATE2_FLAG_TO_DISK, NULL, NULL,
                                     updated, &w);
    } else {
        g_autoptr(NMConnection) conn = ss_connection_new(p, error);
        if (!conn) {
            g_main_loop_unref(w.loop);
            return FALSE;
        }
        nm_client_add_connection_async(client, conn, TRUE, NULL, added, &w);
    }
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    if (w.error)
        g_propagate_error(error, w.error);
    return w.ok;
}

int main(int argc, char **argv) {
    ss_i18n_init();
    g_autofree char *tcp = NULL, *udp = NULL, *name = NULL, *mode = NULL, *ip_mode = NULL;
    g_autofree char *dns = NULL, *regional = NULL, *bypass = NULL, *output = NULL, *uuid = NULL;
    gint tcp_index = 0, udp_index = 0;
    gboolean version = FALSE;
    GOptionEntry entries[] = {
        {"tcp", 0, 0, G_OPTION_ARG_FILENAME, &tcp, _("TCP node JSON"), "FILE"},
        {"udp", 0, 0, G_OPTION_ARG_FILENAME, &udp, _("UDP node JSON (defaults to the TCP node)"),
         "FILE"},
        {"tcp-index", 0, 0, G_OPTION_ARG_INT, &tcp_index, _("TCP node index (starting at 0)"), "N"},
        {"udp-index", 0, 0, G_OPTION_ARG_INT, &udp_index, _("UDP node index (starting at 0)"), "N"},
        {"name", 0, 0, G_OPTION_ARG_STRING, &name, _("Connection name"), "NAME"},
        {"mode", 0, 0, G_OPTION_ARG_STRING, &mode, _("Proxy mode: global / lan / china"), "MODE"},
        {"ip-mode", 0, 0, G_OPTION_ARG_STRING, &ip_mode, _("IP mode: ipv4 / ipv6 / dual"), "MODE"},
        {"dns", 0, 0, G_OPTION_ARG_STRING, &dns, _("Comma-separated DNS IP addresses"), "IPS"},
        {"china-cidrs", 0, 0, G_OPTION_ARG_FILENAME, &regional, _("Regional CIDR text file"),
         "FILE"},
        {"bypass-cidrs", 0, 0, G_OPTION_ARG_FILENAME, &bypass,
         _("Additional bypass CIDR text file"), "FILE"},
        {"output", 'o', 0, G_OPTION_ARG_FILENAME, &output,
         _("Only export configuration (includes passwords, mode 0600)"), "FILE"},
        {"uuid", 0, 0, G_OPTION_ARG_STRING, &uuid, _("Update an existing connection"), "UUID"},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, _("Show version"), NULL},
        {NULL}};
    g_autoptr(GOptionContext) ctx = g_option_context_new(
        "import --tcp NODE.json [--udp NODE.json]\n  nm-shadowsocks validate|plan|apply PROFILE.json\n  nm-shadowsocks list");
    g_option_context_add_main_entries(ctx, entries, GETTEXT_PACKAGE);
    g_autoptr(GError) error = NULL;
    g_autoptr(SsProfile) p = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &error))
        goto fail;

    if (version) {
        g_print("%s\n", SS_VERSION);
        return 0;
    }

    if (argc < 2) {
        g_autofree char *help = g_option_context_get_help(ctx, TRUE, NULL);
        g_print("%s", help);
        return 1;
    }

    if (!strcmp(argv[1], "list")) {
        g_autoptr(NMClient) client = nm_client_new(NULL, &error);
        if (!client)
            goto fail;
        const GPtrArray *connections = nm_client_get_connections(client);
        for (guint i = 0; i < connections->len; i++) {
            NMConnection *c = g_ptr_array_index(connections, i);
            NMSettingVpn *vpn = nm_connection_get_setting_vpn(c);
            if (vpn && !g_strcmp0(nm_setting_vpn_get_service_type(vpn), SS_SERVICE))
                g_print("%s  %s\n", nm_connection_get_uuid(c), nm_connection_get_id(c));
        }
        return 0;
    }

    if (!strcmp(argv[1], "import")) {
        if (!tcp || tcp_index < 0 || udp_index < 0) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                _("Specify --tcp and non-negative node indexes"));
            goto fail;
        }
        p = ss_profile_new();
        for (guint i = 0; i < 2; i++) {
            JsonObject *node = ss_import_node(i == 0 || !udp ? tcp : udp,
                                              i == 0 || !udp ? tcp_index : udp_index, &error);
            if (!node)
                goto fail;
            json_object_set_object_member(p->obj, i == 0 ? "tcp" : "udp", node);
        }

        if (name)
            json_object_set_string_member(p->obj, "name", name);

        if (mode)
            json_object_set_string_member(p->obj, "mode", mode);

        if (ip_mode)
            json_object_set_string_member(p->obj, "ip_mode", ip_mode);

        if (dns || (ip_mode && strcmp(ip_mode, "dual"))) {
            g_auto(GStrv) servers = g_strsplit(
                dns ? dns : (!strcmp(ip_mode, "ipv4") ? "1.1.1.1" : "2606:4700:4700::1111"), ",",
                -1);
            JsonArray *array = json_array_new();
            for (guint i = 0; servers[i]; i++)
                json_array_add_string_element(array, g_strstrip(servers[i]));
            json_object_set_array_member(p->obj, "dns", array);
        }

        for (guint i = 0; i < 2; i++) {
            const char *path = i == 0 ? regional : bypass;
            if (path) {
                JsonArray *array = ss_cidrs_load(path, &error);
                if (!array)
                    goto fail;
                json_object_set_array_member(p->obj, i == 0 ? "china" : "bypass", array);
            }
        }
        if (!ss_profile_use_default_regional(p, &error) || !ss_profile_validate(p, &error))
            goto fail;
        if (output) {
            if (!ss_profile_export(p, output, &error))
                goto fail;
            g_print(_("Exported (includes passwords): %s\n"), output);
            return 0;
        }
    } else if (!strcmp(argv[1], "apply") || !strcmp(argv[1], "validate") ||
               !strcmp(argv[1], "plan")) {
        if (argc != 3) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                _("Specify one configuration file"));
            goto fail;
        }
        p = ss_profile_load(argv[2], &error);
        if (!p)
            goto fail;
        if (!strcmp(argv[1], "validate")) {
            g_print(_("Configuration is valid: %s\n"), ss_string(p->obj, "name"));
            return 0;
        }
        if (!strcmp(argv[1], "plan")) {
            g_autoptr(GPtrArray) plan = ss_route_plan(p);
            for (guint i = 0; i < plan->len; i++)
                g_print("%s\n", (char *)g_ptr_array_index(plan, i));
            return 0;
        }
    } else {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Unknown command"));
        goto fail;
    }

    if (!save(p, uuid, &error))
        goto fail;
    return 0;

fail:
    g_printerr(_("Error: %s\n"), error ? error->message : _("Operation failed"));

    return 1;
}
