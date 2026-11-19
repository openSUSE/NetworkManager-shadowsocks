/*
 * routes.c
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
#include <string.h>

static void add_cidr(GHashTable *set, const char *cidr, guint family) {
    g_autoptr(GInetAddressMask) mask = g_inet_address_mask_new_from_string(cidr, NULL);
    if (mask && g_inet_address_mask_get_family(mask) ==
                    (family == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6)) {
        g_autofree char *address = g_inet_address_to_string(g_inet_address_mask_get_address(mask));
        g_hash_table_add(set,
                         g_strdup_printf("%s/%u", address, g_inet_address_mask_get_length(mask)));
    }
}

static void add_array(GHashTable *set, JsonArray *array, guint family) {
    for (guint i = 0; i < json_array_get_length(array); i++)
        add_cidr(set, json_array_get_string_element(array, i), family);
}

static gint compare_strings(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

GPtrArray *ss_route_plan(SsProfile *p) {
    GPtrArray *plan = g_ptr_array_new_with_free_func(g_free);
    const char *special[] = {"0.0.0.0/8", "127.0.0.0/8", "224.0.0.0/4", "240.0.0.0/4", "::/128",
                             "::1/128",   "fe80::/10",   "ff00::/8",    NULL};
    const char *lan[] = {"10.0.0.0/8",
                         "172.16.0.0/12",
                         "192.168.0.0/16",
                         "100.64.0.0/10",
                         "169.254.0.0/16",
                         "fc00::/7",
                         NULL};
    const char *mode = ss_string(p->obj, "mode");
    JsonArray *dns = json_object_get_array_member(p->obj, "dns");
    for (guint family = 4; family <= 6; family += 2) {
        g_autoptr(GHashTable) set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint i = 0; special[i]; i++)
            add_cidr(set, special[i], family);
        if (strcmp(mode, "global"))
            for (guint i = 0; lan[i]; i++)
                add_cidr(set, lan[i], family);
        if (!strcmp(mode, "china"))
            add_array(set, json_object_get_array_member(p->obj, "china"), family);
        add_array(set, json_object_get_array_member(p->obj, "bypass"), family);
        g_autoptr(GHashTable) dns_set =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint i = 0; i < json_array_get_length(dns); i++) {
            g_autoptr(GInetAddress) ip =
                g_inet_address_new_from_string(json_array_get_string_element(dns, i));
            if (g_inet_address_get_family(ip) !=
                (family == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6))
                continue;
            g_autofree char *addr = g_inet_address_to_string(ip);
            g_autofree char *prefix = g_strdup_printf("%s/%u", addr, family == 4 ? 32 : 128);
            g_hash_table_remove(set, prefix);
            g_hash_table_add(dns_set, g_strdup(prefix));
        }
        /* SIP003 children do not inherit sslocal's SO_MARK. Bypass only the
         * resolved server endpoints, including in single-stack mode. */
        for (guint i = 0; i < 2; i++) {
            JsonObject *node = ss_node(p, i);
            if (!*ss_string(node, "plugin"))
                continue;
            g_autoptr(GInetAddress) ip = g_inet_address_new_from_string(ss_string(node, "server"));
            if (!ip || g_inet_address_get_family(ip) !=
                           (family == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6))
                continue;
            g_autofree char *addr = g_inet_address_to_string(ip);
            g_autofree char *prefix = g_strdup_printf("%s/%u", addr, family == 4 ? 32 : 128);
            g_hash_table_remove(dns_set, prefix);
            add_cidr(set, prefix, family);
        }
        g_autoptr(GPtrArray) cidrs = g_ptr_array_new();
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, set);
        while (g_hash_table_iter_next(&iter, &key, NULL))
            g_ptr_array_add(cidrs, key);
        g_ptr_array_sort(cidrs, compare_strings);
        for (guint proto = 0; proto < 2; proto++) {
            const char *dev = "nmss-tun";
            if (ss_enabled(p, family))
                g_ptr_array_add(plan,
                                g_strdup_printf(SS_IP " -%u route add table %u default dev %s",
                                                family, 51820 + proto, dev));
            else
                g_ptr_array_add(plan,
                                g_strdup_printf(SS_IP " -%u route add table %u unreachable default",
                                                family, 51820 + proto));
            for (guint i = 0; i < cidrs->len; i++)
                g_ptr_array_add(plan, g_strdup_printf(SS_IP " -%u route add table %u throw %s",
                                                      family, 51820 + proto,
                                                      (char *)g_ptr_array_index(cidrs, i)));
            g_hash_table_iter_init(&iter, dns_set);
            while (g_hash_table_iter_next(&iter, &key, NULL))
                g_ptr_array_add(plan, g_strdup_printf(SS_IP " -%u route add table %u %s dev %s",
                                                      family, 51820 + proto, (char *)key, dev));
        }
    }
    /* Publish rules only once BOTH families' tables are populated. */
    for (guint family = 4; family <= 6; family += 2) {
        g_ptr_array_add(
            plan,
            g_strdup_printf(
                SS_IP " -%u rule add priority 10500 fwmark 0x5353/0xffffffff lookup main", family));
        g_ptr_array_add(
            plan,
            g_strdup_printf(
                SS_IP " -%u rule add priority 10501 fwmark 0x5353/0xffffffff unreachable", family));
        /* nftables IPv6 reverse-path checks also perform policy lookups.
         * A throw route inside an ipproto-selected table is insufficient for
         * those lookups; select main explicitly before the proxy rules. */
        g_autoptr(GHashTable) endpoints =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint i = 0; i < 2; i++) {
            JsonObject *node = ss_node(p, i);
            if (!*ss_string(node, "plugin"))
                continue;
            g_autoptr(GInetAddress) address =
                g_inet_address_new_from_string(ss_string(node, "server"));
            if (!address || g_inet_address_get_family(address) !=
                                (family == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6))
                continue;
            g_autofree char *text = g_inet_address_to_string(address);
            if (g_hash_table_contains(endpoints, text))
                continue;
            g_hash_table_add(endpoints, g_strdup(text));
            g_ptr_array_add(
                plan, g_strdup_printf(SS_IP " -%u rule add priority 10502 to %s/%u lookup main",
                                      family, text, family == 4 ? 32 : 128));
            g_ptr_array_add(
                plan, g_strdup_printf(SS_IP " -%u rule add priority 10503 to %s/%u unreachable",
                                      family, text, family == 4 ? 32 : 128));
        }
        for (guint proto = 0; proto < 2; proto++)
            g_ptr_array_add(plan,
                            g_strdup_printf(SS_IP " -%u rule add priority %u ipproto %s lookup %u",
                                            family, 10510 + proto, proto == 0 ? "tcp" : "udp",
                                            51820 + proto));
    }
    return plan;
}

char *ss_route_undo(const char *command) {
    const char *add = strstr(command, " add ");
    g_return_val_if_fail(add, NULL);
    g_autofree char *before = g_strndup(command, add - command);
    return g_strconcat(before, " del ", add + 5, NULL);
}
gboolean ss_route_tables_apply(GPtrArray *plan, GCancellable *cancel, GError **error) {
    for (guint family = 4; family <= 6; family += 2) {
        g_autofree char *flag = g_strdup_printf("-%u", family);
        g_autofree char *prefix = g_strdup_printf(SS_IP " -%u ", family);
        GString *batch = g_string_new(NULL);
        for (guint i = 0; i < plan->len; i++) {
            const char *cmd = g_ptr_array_index(plan, i);
            if (g_str_has_prefix(cmd, prefix) && strstr(cmd, " route add ")) {
                g_string_append(batch, cmd + strlen(prefix));
                g_string_append_c(batch, '\n');
            }
        }
        g_autoptr(GSubprocess) ip =
            g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                 G_SUBPROCESS_FLAGS_STDERR_PIPE,
                             error, SS_IP, flag, "-batch", "-", NULL);
        if (!ip) {
            g_string_free(batch, TRUE);
            return FALSE;
        }
        g_autofree char *diagnostic = NULL;
        gboolean sent =
            g_subprocess_communicate_utf8(ip, batch->str, cancel, NULL, &diagnostic, error);
        g_string_free(batch, TRUE);
        if (!sent) {
            g_subprocess_force_exit(ip);
            g_subprocess_wait(ip, NULL, NULL);
            return FALSE;
        }
        if (!g_subprocess_get_successful(ip)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("IPv%u route batch failed: %s"),
                        family, diagnostic ? diagnostic : "");
            return FALSE;
        }
    }
    return TRUE;
}

gboolean ss_command(const char *command, char **output, GError **error) {
    g_auto(GStrv) argv = NULL;
    if (!g_shell_parse_argv(command, NULL, &argv, error))
        return FALSE;
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    gint status;
    if (!g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &stdout_text, &stderr_text, &status, error))
        return FALSE;
    if (!g_spawn_check_wait_status(status, NULL)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("iproute2 failed: %s"), stderr_text);
        return FALSE;
    }
    if (output)
        *output = g_steal_pointer(&stdout_text);
    return TRUE;
}

gboolean ss_routes_preflight(GError **error) {
    for (guint family = 4; family <= 6; family += 2) {
        for (guint type = 0; type < 2; type++) {
            g_autofree char *cmd = g_strdup_printf(
                SS_IP " -%u -j %s", family, type == 0 ? "rule show" : "route show table all");
            g_autofree char *text = NULL;
            g_autoptr(JsonParser) parser = json_parser_new();
            if (!ss_command(cmd, &text, error) ||
                !json_parser_load_from_data(parser, text, -1, error))
                return FALSE;
            JsonNode *root = json_parser_get_root(parser);
            if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    _("iproute2 did not return a JSON array"));
                return FALSE;
            }
            JsonArray *array = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(array); i++) {
                JsonObject *o = json_array_get_object_element(array, i);
                JsonNode *n = json_object_get_member(o, type == 0 ? "priority" : "table");
                if (!n)
                    continue;
                gint64 value = json_node_get_value_type(n) == G_TYPE_STRING
                                   ? g_ascii_strtoll(json_node_get_string(n), NULL, 10)
                                   : json_node_get_int(n);
                JsonNode *table_node = json_object_get_member(o, "table");
                gint64 referenced =
                    table_node ? (json_node_get_value_type(table_node) == G_TYPE_STRING
                                      ? g_ascii_strtoll(json_node_get_string(table_node), NULL, 10)
                                      : json_node_get_int(table_node))
                               : 0;
                if ((type == 0 && (value == 10500 || value == 10501 || value == 10502 ||
                                   value == 10503 || value == 10510 || value == 10511 ||
                                   referenced == 51820 || referenced == 51821)) ||
                    (type == 1 && (value == 51820 || value == 51821))) {
                    g_set_error_literal(
                        error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        _("Routing tables 51820/51821 or rule priorities 10500/10501/10502/10503/10510/10511 are in use"));
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
}
