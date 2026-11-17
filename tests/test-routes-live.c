/*
 * test-routes-live.c
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


/* This executable refuses to run in the initial/host network namespace. */
#include "ss.h"
#include <unistd.h>
#include <string.h>

static GPtrArray *active_plan;

static void command(const char *text) {
    g_autoptr(GError) error = NULL;
    g_assert_true(ss_command(text, NULL, &error));
    g_assert_no_error(error);
}
static void expect_route(guint family, const char *destination, const char *proto, const char *mark, const char *device) {
    g_autofree char *cmd = g_strdup_printf(SS_IP " -%u route get %s ipproto %s %s", family, destination, proto, mark ? mark : "");
    g_autofree char *output = NULL;
    g_autoptr(GError) error = NULL;
    gboolean ok = ss_command(cmd, &output, &error);
    if (device) {
        g_assert_no_error(error); g_assert_true(ok);
        g_autofree char *expected = g_strdup_printf("dev %s", device);
        g_assert_nonnull(strstr(output, expected));
    } else {
        g_assert_false(ok); g_assert_nonnull(error);
    }
}
static void cleanup(void) {
    for (guint i = active_plan->len; i > 0; i--) {
        const char *entry = g_ptr_array_index(active_plan, i - 1);
        if (!strstr(entry, " rule add ")) continue;
        g_autofree char *undo = ss_route_undo(entry);
        command(undo);
    }
    g_clear_pointer(&active_plan, g_ptr_array_unref);
    for (guint family = 4; family <= 6; family += 2) {
        for (guint table = 51820; table <= 51821; table++) {
            g_autofree char *cmd = g_strdup_printf(SS_IP " -%u route flush table %u", family, table);
            command(cmd);
        }
    }
}
static void apply(SsProfile *p) {
    g_autoptr(GError) error = NULL;
    g_assert_true(ss_profile_validate(p, &error)); g_assert_no_error(error);
    g_assert_true(ss_routes_preflight(&error)); g_assert_no_error(error);
    g_autoptr(GPtrArray) plan = ss_route_plan(p);
    g_assert_true(ss_route_tables_apply(plan, NULL, &error)); g_assert_no_error(error);
    guint i = 0;
    for (; i < plan->len; i++) {
        const char *cmd = g_ptr_array_index(plan, i);
        if (strstr(cmd, " rule add ")) break;
    }
    for (; i < plan->len; i++) command(g_ptr_array_index(plan, i));
    active_plan = g_steal_pointer(&plan);
}
static SsProfile *profile(const char *mode) {
    SsProfile *p = ss_profile_load(SOURCE_DIR "/examples/profile.json", NULL);
    g_assert_nonnull(p);
    json_object_set_string_member(p->obj, "mode", mode);
    return p;
}
static void split_and_lan(void) {
    g_autoptr(SsProfile) p = profile("lan");
    apply(p);
    expect_route(4, "8.8.8.8", "tcp", NULL, "nmss-tun");
    expect_route(4, "8.8.8.8", "udp", NULL, "nmss-tun");
    expect_route(6, "2001:4860:4860::8888", "tcp", NULL, "nmss-tun");
    expect_route(6, "2001:4860:4860::8888", "udp", NULL, "nmss-tun");
    expect_route(4, "10.1.2.3", "tcp", NULL, "uplink");
    expect_route(6, "fd12::1", "udp", NULL, "uplink");
    expect_route(4, "8.8.8.8", "tcp", "mark 0x5353", "uplink");
    cleanup();
}
static void regional_and_dns(void) {
    g_autoptr(SsProfile) p = profile("china");
    JsonArray *regional = json_object_get_array_member(p->obj, "china");
    json_array_add_string_element(regional, "1.0.1.0/24"); json_array_add_string_element(regional, "2400:3200::/32");
    JsonArray *dns = json_array_new(); json_array_add_string_element(dns, "10.1.2.3");
    json_object_set_array_member(p->obj, "dns", dns);
    apply(p);
    expect_route(4, "1.0.1.2", "tcp", NULL, "uplink");
    expect_route(6, "2400:3200::1", "udp", NULL, "uplink");
    expect_route(4, "10.1.2.3", "udp", NULL, "nmss-tun");
    expect_route(4, "10.1.2.3", "tcp", NULL, "nmss-tun");
    expect_route(4, "10.1.2.3", "udp", "oif nmss-tun", "nmss-tun");
    expect_route(6, "2606:4700:4700::1111", "tcp", "oif nmss-tun", "nmss-tun");
    expect_route(6, "2606:4700:4700::1111", "udp", "oif nmss-tun", "nmss-tun");
    cleanup();
}
static void single_family(void) {
    g_autoptr(SsProfile) p = profile("global");
    json_object_set_string_member(p->obj, "ip_mode", "ipv4");
    JsonArray *dns = json_array_new(); json_array_add_string_element(dns, "1.1.1.1");
    json_object_set_array_member(p->obj, "dns", dns);
    apply(p);
    expect_route(4, "10.1.2.3", "tcp", NULL, "nmss-tun");
    expect_route(6, "2001:4860:4860::8888", "tcp", NULL, NULL);
    expect_route(6, "2001:4860:4860::8888", "udp", NULL, NULL);
    command(SS_IP " -4 route del default dev uplink");
    expect_route(4, "8.8.8.8", "tcp", "mark 0x5353", NULL);
    command(SS_IP " -4 route add default dev uplink");
    cleanup();
}
static void plugin_endpoints(void) {
    g_autoptr(SsProfile) p = profile("global");
    json_object_set_string_member(ss_node(p, 0), "server", "203.0.113.9");
    json_object_set_string_member(ss_node(p, 0), "plugin", "obfs-local");
    json_object_set_string_member(ss_node(p, 1), "server", "2001:db8:20::9");
    json_object_set_string_member(ss_node(p, 1), "plugin", "v2ray-plugin");
    apply(p);
    expect_route(4, "203.0.113.9", "tcp", NULL, "uplink");
    expect_route(4, "203.0.113.9", "udp", NULL, "uplink");
    expect_route(6, "2001:db8:20::9", "tcp", NULL, "uplink");
    expect_route(6, "2001:db8:20::9", "udp", NULL, "uplink");
    expect_route(4, "8.8.8.8", "tcp", NULL, "nmss-tun");
    expect_route(6, "2001:4860:4860::8888", "udp", NULL, "nmss-tun");
    cleanup();
}
static void conflicts(void) {
    g_autoptr(GError) error = NULL;
    command(SS_IP " -4 rule add priority 20000 lookup 51820");
    g_assert_false(ss_routes_preflight(&error)); g_assert_nonnull(error); g_clear_error(&error);
    command(SS_IP " -4 rule del priority 20000 lookup 51820");
    command(SS_IP " -6 rule add priority 10510 lookup main");
    g_assert_false(ss_routes_preflight(&error)); g_assert_nonnull(error); g_clear_error(&error);
    command(SS_IP " -6 rule del priority 10510 lookup main");
    command(SS_IP " -4 route add table 51821 unreachable default");
    g_assert_false(ss_routes_preflight(&error)); g_assert_nonnull(error); g_clear_error(&error);
    command(SS_IP " -4 route flush table 51821");
    g_assert_true(ss_routes_preflight(&error)); g_assert_no_error(error);
}
static void bundled_regional(void) {
    g_autoptr(SsProfile) p = profile("china");
    JsonArray *cidrs = ss_cidrs_load(SOURCE_DIR "/data/china/china.cidr", NULL);
    g_assert_nonnull(cidrs);
    json_object_set_array_member(p->obj, "china", cidrs);
    apply(p);
    expect_route(4, "1.0.1.2", "tcp", NULL, "uplink");
    expect_route(6, "2400:3200::1", "udp", NULL, "uplink");
    expect_route(4, "8.8.8.8", "udp", NULL, "nmss-tun");
    cleanup();
}
int main(int argc, char **argv) {
    g_autofree char *self = g_file_read_link("/proc/self/ns/net", NULL);
    const char *initial = g_getenv("SS_TEST_PARENT_NETNS");
    if (getuid() != 0 || !self || !initial || !g_str_has_prefix(initial, "net:[") || !strcmp(self, initial)) {
        g_printerr("Refusing: use tests/run-netns.sh %s\n", argv[0]);
        return 77;
    }
    g_test_init(&argc, &argv, NULL);
    command(SS_IP " link set lo up");
    const char *devices[] = {"uplink", "nmss-tun"};
    const char *v4[] = {"192.0.2.100/24", "198.18.0.1/30", "198.18.0.5/30"};
    const char *v6[] = {"2001:db8:10::1/64", "fd53:5353::1/126", "fd53:5353::5/126"};
    for (guint i = 0; i < G_N_ELEMENTS(devices); i++) {
        g_autofree char *link = g_strdup_printf(SS_IP " link add %s type dummy", devices[i]); command(link);
        g_autofree char *up = g_strdup_printf(SS_IP " link set %s up", devices[i]); command(up);
        g_autofree char *addr4 = g_strdup_printf(SS_IP " -4 addr add %s dev %s", v4[i], devices[i]); command(addr4);
        g_autofree char *addr6 = g_strdup_printf(SS_IP " -6 addr add %s dev %s nodad", v6[i], devices[i]); command(addr6);
    }
    command(SS_IP " -4 route add default dev uplink"); command(SS_IP " -6 route add default dev uplink");
    g_test_add_func("/live/plugin-endpoints", plugin_endpoints);
    g_test_add_func("/live/split-and-lan", split_and_lan);
    g_test_add_func("/live/regional-and-dns", regional_and_dns);
    g_test_add_func("/live/single-family-and-loop-prevention", single_family);
    g_test_add_func("/live/conflict-detection", conflicts);
    g_test_add_func("/live/bundled-china-full-table", bundled_regional);
    return g_test_run();
}
