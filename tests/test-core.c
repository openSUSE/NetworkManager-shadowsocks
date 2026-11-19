/*
 * test-core.c
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
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *fixture =
    "{\"name\":\"Test\",\"tcp\":{\"server\":\"192.0.2.1\",\"server_port\":8388,\"method\":\"aes-256-gcm\",\"password\":\"tcp-secret\"},\"udp\":{\"server\":\"2001:db8::2\",\"server_port\":443,\"method\":\"chacha20-ietf-poly1305\",\"password\":\"udp-secret\"}}";

static SsProfile *profile(void) {
    g_autoptr(GError) error = NULL;
    SsProfile *p = ss_profile_parse(fixture, &error);
    g_assert_no_error(error);
    g_assert_nonnull(p);
    return p;
}

static gboolean contains(GPtrArray *plan, const char *part) {
    for (guint i = 0; i < plan->len; i++)
        if (strstr(g_ptr_array_index(plan, i), part))
            return TRUE;
    return FALSE;
}

static void independent(void) {
    g_autoptr(SsProfile) p = profile();
    g_autofree char *text = ss_backend_json(p);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonArray *servers = json_object_get_array_member(root, "servers");
    g_assert_cmpuint(json_array_get_length(servers), ==, 2);
    JsonObject *tcp = json_array_get_object_element(servers, 0);
    JsonObject *udp = json_array_get_object_element(servers, 1);
    g_assert_cmpstr(ss_string(tcp, "mode"), ==, "tcp_only");
    g_assert_cmpstr(ss_string(tcp, "password"), ==, "tcp-secret");
    g_assert_cmpstr(ss_string(udp, "mode"), ==, "udp_only");
    g_assert_cmpstr(ss_string(udp, "password"), ==, "udp-secret");
    JsonObject *local =
        json_array_get_object_element(json_object_get_array_member(root, "locals"), 0);
    g_assert_cmpstr(ss_string(local, "mode"), ==, "tcp_and_udp");
    g_assert_cmpstr(ss_string(local, "tun_interface_name"), ==, "nmss-tun");
}

static void shared(void) {
    g_autoptr(SsProfile) p = profile();
    json_object_remove_member(p->obj, "udp");
    g_autofree char *text = ss_profile_json(p, TRUE);
    g_autoptr(SsProfile) q = ss_profile_parse(text, NULL);
    g_assert_nonnull(q);
    g_assert_cmpstr(ss_string(ss_node(q, 0), "password"), ==, ss_string(ss_node(q, 1), "password"));
}

static void secrets(void) {
    g_autoptr(SsProfile) p = profile();
    g_autofree char *public = ss_profile_json(p, FALSE);
    g_assert_null(strstr(public, "tcp-secret"));
    g_assert_null(strstr(public, "udp-secret"));
    g_autoptr(GError) error = NULL;
    g_autoptr(NMConnection) conn = ss_connection_new(p, &error);
    g_assert_no_error(error);
    g_assert_true(nm_connection_verify(conn, &error));
    g_assert_no_error(error);
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(conn);
    g_assert_cmpstr(nm_setting_vpn_get_secret(vpn, "tcp-password"), ==, "tcp-secret");
    g_assert_cmpint(
        nm_setting_ip_config_get_dns_priority(nm_connection_get_setting_ip4_config(conn)), ==,
        -5353);
    g_autoptr(SsProfile) q = ss_profile_from_connection(conn, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ss_string(ss_node(q, 1), "server"), ==, "2001:db8::2");
    nm_setting_vpn_remove_secret(vpn, "tcp-password");
    g_autoptr(SsProfile) blank = ss_profile_from_connection(conn, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ss_string(ss_node(blank, 0), "password"), ==, "");
    g_autoptr(SsProfile) rejected = ss_profile_from_connection(conn, TRUE, &error);
    g_assert_null(rejected);
    g_assert_nonnull(error);
}

static void invalid_node(void) {
    g_autoptr(SsProfile) p = profile();
    json_object_set_boolean_member(ss_node(p, 0), "server_port", TRUE);
    g_assert_false(ss_profile_validate(p, NULL));
    json_object_set_int_member(ss_node(p, 0), "server_port", 8388);
    json_object_set_string_member(ss_node(p, 0), "plugin", "obfs-local --help");
    g_assert_false(ss_profile_validate(p, NULL));
    json_object_remove_member(ss_node(p, 0), "plugin");
    json_object_set_string_member(ss_node(p, 0), "server", "example.com;id");
    g_assert_false(ss_profile_validate(p, NULL));
}

static void plugins(void) {
    g_autoptr(SsProfile) p = profile();
    JsonObject *tcp = ss_node(p, 0), *udp = ss_node(p, 1);
    json_object_set_string_member(tcp, "plugin", "obfs-local");
    json_object_set_string_member(tcp, "plugin_opts", "obfs=http;obfs-host=www.example.com");
    JsonArray *args = json_array_new();
    json_array_add_string_element(args, "--log-level");
    json_array_add_string_element(args, "a value;$(not-a-shell)");
    json_object_set_array_member(tcp, "plugin_args", args);
    json_object_set_string_member(udp, "plugin", "v2ray-plugin");
    json_object_set_string_member(udp, "plugin_opts", "mode=quic;host=udp.example.com");
    json_object_set_string_member(udp, "plugin_mode", "udp_only");
    g_assert_true(ss_profile_validate(p, NULL));
    g_autofree char *serialized = ss_profile_json(p, TRUE);
    g_autoptr(SsProfile) copy = ss_profile_parse(serialized, NULL);
    g_assert_nonnull(copy);
    g_autofree char *backend = ss_backend_json(copy);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, backend, -1, NULL));
    JsonArray *servers =
        json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "servers");
    g_assert_cmpstr(ss_string(json_array_get_object_element(servers, 0), "plugin"), ==,
                    "obfs-local");
    g_assert_cmpstr(ss_string(json_array_get_object_element(servers, 1), "plugin"), ==,
                    "v2ray-plugin");
    g_assert_nonnull(strstr(backend, "a value;$(not-a-shell)"));
    /* Preserve the selected plugin transport independently of node traffic. */
    g_assert_cmpstr(ss_string(json_array_get_object_element(servers, 1), "plugin_mode"), ==,
                    "udp_only");
    g_assert_cmpstr(ss_string(json_array_get_object_element(servers, 1), "mode"), ==, "udp_only");
    g_autofree char *public = ss_profile_json(p, FALSE);
    g_assert_nonnull(strstr(public, "obfs-host"));
    g_assert_nonnull(strstr(public, "not-a-shell"));
    g_autoptr(NMConnection) conn = ss_connection_new(p, NULL);
    g_assert_nonnull(conn);
    /* Settings hosts may reopen a connection without fetching any secrets. */
    nm_connection_clear_secrets(conn);
    g_autoptr(SsProfile) restored = ss_profile_from_connection(conn, FALSE, NULL);
    g_assert_nonnull(restored);
    g_assert_cmpstr(ss_string(ss_node(restored, 0), "plugin_opts"), ==,
                    ss_string(tcp, "plugin_opts"));
    g_assert_cmpstr(ss_string(ss_node(restored, 1), "plugin_opts"), ==,
                    ss_string(udp, "plugin_opts"));
    g_assert_cmpstr(json_array_get_string_element(
                        json_object_get_array_member(ss_node(restored, 0), "plugin_args"), 1),
                    ==, "a value;$(not-a-shell)");
    /* Migrate old connections whose plugin configuration lived in secrets. */
    g_autoptr(NMConnection) legacy = ss_connection_new(p, NULL);
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(legacy);
    g_autoptr(JsonParser) old_parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(old_parser, public, -1, NULL));
    JsonObject *old_profile = json_node_get_object(json_parser_get_root(old_parser));
    const char *protocols[] = {"tcp", "udp"};
    const char *fields[] = {"plugin_opts", "plugin_args"};
    for (guint i = 0; i < G_N_ELEMENTS(protocols); i++) {
        JsonObject *node = json_object_get_object_member(old_profile, protocols[i]);
        for (guint j = 0; j < G_N_ELEMENTS(fields); j++) {
            JsonNode *value = json_object_get_member(node, fields[j]);
            if (!value)
                continue;
            g_autofree char *key = g_strdup_printf("%s-%s", protocols[i], fields[j]);
            g_autofree char *encoded = json_to_string(value, FALSE);
            nm_setting_vpn_add_secret(vpn, key, encoded);
            json_object_remove_member(node, fields[j]);
        }
    }
    g_autofree char *old_text = json_to_string(json_parser_get_root(old_parser), FALSE);
    nm_setting_vpn_add_data_item(vpn, "profile", old_text);
    g_autoptr(SsProfile) migrated = ss_profile_from_connection(legacy, TRUE, NULL);
    g_assert_nonnull(migrated);
    g_assert_cmpstr(ss_string(ss_node(migrated, 0), "plugin_opts"), ==,
                    ss_string(tcp, "plugin_opts"));
    g_assert_true(ss_connection_update(migrated, legacy, NULL));
    nm_connection_clear_secrets(legacy);
    g_autoptr(SsProfile) reopened = ss_profile_from_connection(legacy, FALSE, NULL);
    g_assert_nonnull(reopened);
    g_assert_cmpstr(ss_string(ss_node(reopened, 1), "plugin_opts"), ==,
                    ss_string(udp, "plugin_opts"));
    g_autoptr(GPtrArray) plan = ss_route_plan(p);
    g_assert_true(contains(plan, "table 51820 throw 192.0.2.1/32"));
    g_assert_true(contains(plan, "table 51821 throw 2001:db8::2/128"));
    g_assert_true(contains(plan, "priority 10502 to 192.0.2.1/32 lookup main"));
    g_assert_true(contains(plan, "priority 10503 to 192.0.2.1/32 unreachable"));
    g_assert_true(contains(plan, "priority 10502 to 2001:db8::2/128 lookup main"));
    json_object_set_string_member(tcp, "plugin_mode", "invalid");
    g_assert_false(ss_profile_validate(p, NULL));
    json_object_remove_member(tcp, "plugin_mode");
    json_object_set_boolean_member(tcp, "plugin_opts", TRUE);
    g_assert_false(ss_profile_validate(p, NULL));
    json_object_set_string_member(tcp, "plugin_opts", "");
    json_array_add_int_element(args, 1);
    g_assert_false(ss_profile_validate(p, NULL));
    json_object_remove_member(tcp, "plugin_args");
    json_object_set_string_member(tcp, "plugin", "");
    json_object_set_string_member(tcp, "plugin_opts", "orphan-options");
    g_assert_false(ss_profile_validate(p, NULL));
    g_assert_null(ss_plugin_resolve("/usr/bin/sh", NULL));
    g_assert_null(ss_plugin_resolve(
        "/usr/libexec/NetworkManager/shadowsocks/plugins/../../../../bin/sh", NULL));
}

static void dns_validation(void) {
    const char *invalid[] = {"127.0.0.53",  "::1",        "0.0.0.0",      "ff02::1", "fe80::1",
                             "dns.example", "198.18.0.1", "fd53:5353::1", NULL};
    for (guint i = 0; invalid[i]; i++) {
        g_autoptr(SsProfile) p = profile();
        JsonArray *array = json_array_new();
        json_array_add_string_element(array, invalid[i]);
        json_object_set_array_member(p->obj, "dns", array);
        g_assert_false(ss_profile_validate(p, NULL));
    }
    g_autoptr(SsProfile) p = profile();
    json_object_set_string_member(p->obj, "ip_mode", "ipv4");
    g_assert_false(ss_profile_validate(p, NULL));
}

static void regional_validation(void) {
    g_autoptr(SsProfile) p = profile();
    json_object_set_string_member(p->obj, "mode", "china");
    g_assert_false(ss_profile_validate(p, NULL));
    JsonArray *array = json_object_get_array_member(p->obj, "china");
    json_array_add_string_element(array, "1.0.1.0/24");
    g_assert_false(ss_profile_validate(p, NULL));
    json_array_add_string_element(array, "2400:3200::/32");
    g_assert_true(ss_profile_validate(p, NULL));
    json_array_add_string_element(array, "0.0.0.0/0");
    g_assert_false(ss_profile_validate(p, NULL));
}

static void routes(void) {
    g_autoptr(SsProfile) p = profile();
    g_autoptr(GPtrArray) plan = ss_route_plan(p);
    g_assert_true(contains(plan, "-4 rule add priority 10510 ipproto tcp lookup 51820"));
    g_assert_true(contains(plan, "-6 rule add priority 10511 ipproto udp lookup 51821"));
    g_assert_true(contains(plan, "fwmark 0x5353/0xffffffff lookup main"));
    g_assert_true(contains(plan, "fwmark 0x5353/0xffffffff unreachable"));
    g_assert_true(contains(plan, "table 51820 throw 10.0.0.0/8"));
    g_assert_true(contains(plan, "table 51821 1.1.1.1/32 dev nmss-tun"));
    gboolean seen_rule = FALSE;
    for (guint i = 0; i < plan->len; i++) {
        const char *cmd = g_ptr_array_index(plan, i);
        if (strstr(cmd, " rule add "))
            seen_rule = TRUE;
        if (seen_rule)
            g_assert_null(strstr(cmd, " route add "));
        g_autofree char *undo = ss_route_undo(cmd);
        g_assert_null(strstr(undo, " add "));
        g_assert_nonnull(strstr(undo, " del "));
    }
}

static void modes(void) {
    g_autoptr(SsProfile) p = profile();
    json_object_set_string_member(p->obj, "mode", "global");
    g_autoptr(GPtrArray) plan = ss_route_plan(p);
    g_assert_false(contains(plan, "throw 10.0.0.0/8"));
    g_assert_true(contains(plan, "throw 127.0.0.0/8"));
    json_object_set_string_member(p->obj, "ip_mode", "ipv4");
    JsonArray *dns = json_array_new();
    json_array_add_string_element(dns, "1.1.1.1");
    json_object_set_array_member(p->obj, "dns", dns);
    g_assert_true(ss_profile_validate(p, NULL));
    g_autoptr(GPtrArray) single = ss_route_plan(p);
    g_assert_true(contains(single, "-6 route add table 51821 unreachable default"));
    g_assert_false(contains(single, "-6 route add table 51821 default dev"));
}

static void dns_override(void) {
    g_autoptr(SsProfile) p = profile();
    JsonArray *dns = json_array_new();
    json_array_add_string_element(dns, "10.1.2.3");
    json_array_add_string_element(dns, "10.1.2.3");
    json_object_set_array_member(p->obj, "dns", dns);
    json_array_add_string_element(json_object_get_array_member(p->obj, "bypass"), "10.1.2.3/32");
    g_assert_true(ss_profile_validate(p, NULL));
    g_autoptr(GPtrArray) plan = ss_route_plan(p);
    g_assert_false(contains(plan, "throw 10.1.2.3/32"));
    guint count = 0;
    for (guint i = 0; i < plan->len; i++)
        if (strstr(g_ptr_array_index(plan, i), "10.1.2.3/32 dev"))
            count++;
    g_assert_cmpuint(count, ==, 2);
}

static void published_dns(void) {
    g_autoptr(SsProfile) p = profile();
    const char *gateways[] = {"192.0.2.10", "2001:db8::10"};
    for (guint i = 0; i < G_N_ELEMENTS(gateways); i++) {
        g_autoptr(GInetAddress) gateway = g_inet_address_new_from_string(gateways[i]);
        g_autoptr(GVariant) general = ss_general_config(p, gateway);
        g_autoptr(GVariant) value = g_variant_lookup_value(general, "gateway", NULL);
        g_assert_nonnull(value);
        g_autoptr(GInetAddress) actual = NULL;
        if (i == 0) {
            guint32 address = g_variant_get_uint32(value);
            actual = g_inet_address_new_from_bytes((const guint8 *)&address, G_SOCKET_FAMILY_IPV4);
        } else {
            gsize size;
            const guint8 *bytes = g_variant_get_fixed_array(value, &size, 1);
            g_assert_cmpuint(size, ==, 16);
            actual = g_inet_address_new_from_bytes(bytes, G_SOCKET_FAMILY_IPV6);
        }
        g_assert_true(g_inet_address_equal(gateway, actual));
        gboolean enabled = FALSE;
        g_assert_true(g_variant_lookup(general, "has-ip4", "b", &enabled));
        g_assert_true(enabled);
        g_assert_true(g_variant_lookup(general, "has-ip6", "b", &enabled));
        g_assert_true(enabled);
    }
    for (guint family = 4; family <= 6; family += 2) {
        g_autoptr(GVariant) config = ss_ip_config(p, family);
        g_autoptr(GVariant) dns = g_variant_lookup_value(config, "dns", NULL);
        g_assert_nonnull(dns);
        g_assert_cmpuint(g_variant_n_children(dns), ==, 1);
        g_autoptr(GVariant) first = g_variant_get_child_value(dns, 0);
        g_autoptr(GInetAddress) actual = NULL;
        if (family == 4) {
            guint32 address = g_variant_get_uint32(first);
            actual = g_inet_address_new_from_bytes((const guint8 *)&address, G_SOCKET_FAMILY_IPV4);
        } else {
            gsize size;
            const guint8 *bytes = g_variant_get_fixed_array(first, &size, 1);
            g_assert_cmpuint(size, ==, 16);
            actual = g_inet_address_new_from_bytes(bytes, G_SOCKET_FAMILY_IPV6);
        }
        g_autofree char *address = g_inet_address_to_string(actual);
        g_assert_cmpstr(address, ==, family == 4 ? "1.1.1.1" : "2606:4700:4700::1111");
        g_autoptr(GVariant) domains =
            g_variant_lookup_value(config, "domains", G_VARIANT_TYPE("as"));
        g_assert_nonnull(domains);
        const char *domain;
        g_variant_get_child(domains, 0, "&s", &domain);
        g_assert_cmpstr(domain, ==, "~.");
    }
}

static void file_io(void) {
    g_autofree char *dir = g_dir_make_tmp("nmss-test-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree char *path = g_build_filename(dir, "profile.json", NULL);
    g_autoptr(SsProfile) p = profile();
    g_assert_true(ss_profile_export(p, path, NULL));
    struct stat st;
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_assert_false(ss_profile_export(p, path, NULL));
    g_autoptr(SsProfile) q = ss_profile_load(path, NULL);
    g_assert_nonnull(q);
    g_autofree char *link = g_build_filename(dir, "link", NULL);
    g_assert_cmpint(symlink(path, link), ==, 0);
    g_assert_false(ss_profile_export(p, link, NULL));
    g_unlink(link);
    g_unlink(path);
    g_rmdir(dir);
}

static void import_nodes(void) {
    g_autofree char *dir = g_dir_make_tmp("nmss-import-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "nodes.json", NULL);
    const char *text =
        "{\"servers\":[{\"address\":\"example.org\",\"port\":443,\"method\":\"aes-128-gcm\",\"password\":\"test\",\"plugin\":\"obfs-local\",\"plugin_opts\":\"obfs=tls\",\"acl\":\"/etc/shadow\"}]}";
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    JsonObject *n = ss_import_node(path, 0, NULL);
    g_assert_nonnull(n);
    g_assert_cmpstr(ss_string(n, "server"), ==, "example.org");
    g_assert_cmpstr(ss_string(n, "plugin_opts"), ==, "obfs=tls");
    g_assert_false(json_object_has_member(n, "acl"));
    json_object_unref(n);
    g_assert_null(ss_import_node(path, 1, NULL));
    g_assert_true(g_file_set_contents(path, "1.0.1.0/24 # test\n\n2400:3200::/32\n", -1, NULL));
    JsonArray *a = ss_cidrs_load(path, NULL);
    g_assert_nonnull(a);
    g_assert_cmpuint(json_array_get_length(a), ==, 2);
    json_array_unref(a);
    g_unlink(path);
    g_rmdir(dir);
}

static void bad_json(void) {
    const char *bad[] = {"[]", "null", "{", "{\"tcp\":false}", "{\"tcp\":{}}", NULL};
    for (guint i = 0; bad[i]; i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(SsProfile) p = ss_profile_parse(bad[i], &error);
        g_assert_null(p);
        g_assert_nonnull(error);
    }
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dns/networkmanager-config", published_dns);
    g_test_add_func("/profile/plugins", plugins);
    g_test_add_func("/profile/independent", independent);
    g_test_add_func("/profile/shared", shared);
    g_test_add_func("/profile/secrets-libnm", secrets);
    g_test_add_func("/profile/invalid-node", invalid_node);
    g_test_add_func("/profile/dns-validation", dns_validation);
    g_test_add_func("/profile/regional-validation", regional_validation);
    g_test_add_func("/routes/protocol-and-order", routes);
    g_test_add_func("/routes/modes", modes);
    g_test_add_func("/routes/dns-override", dns_override);
    g_test_add_func("/files/export", file_io);
    g_test_add_func("/files/import", import_nodes);
    g_test_add_func("/profile/bad-json", bad_json);
    return g_test_run();
}
