/*
 * test-regional.c
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

#include "regional.h"
#include <glib/gstdio.h>
#include <string.h>

static const char *fixture = "2|apnic|20261113|3|20261113|20261113|+0000\n"
                             "apnic|CN|ipv4|192.0.2.1|6|20200101|allocated\n"
                             "apnic|CN|ipv6|2001:db8::|32|20200101|assigned\n"
                             "apnic|HK|ipv4|198.51.100.0|256|20200101|allocated\n";

static char *test_home;

static void default_and_rollback(void) {
    g_autofree char *dir = g_build_filename(test_home, "NetworkManager-shadowsocks", "china", NULL);
    g_autofree char *input = g_build_filename(test_home, "delegated", NULL);
    g_assert_true(g_file_set_contents(input, fixture, -1, NULL));
    g_autoptr(GSubprocess) valid =
        g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                         NULL, UPDATER, "--source", input, "--output", dir, NULL);
    g_assert_nonnull(valid);
    g_assert_true(g_subprocess_wait_check(valid, NULL, NULL));
    g_autofree char *source = NULL;
    JsonArray *a = ss_regional_default(&source, NULL);
    g_assert_nonnull(a);
    g_assert_true(g_str_has_prefix(source, test_home));
    json_array_unref(a);
    const char *config =
        "{\"mode\":\"china\",\"tcp\":{\"server\":\"example.org\",\"server_port\":443,\"method\":\"aes-256-gcm\",\"password\":\"test\"}}";
    g_autoptr(SsProfile) p = ss_profile_parse(config, NULL);
    g_assert_nonnull(p);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(p->obj, "china")), ==, 5);
    g_autofree char *bad = g_build_filename(test_home, "invalid", NULL);
    g_assert_true(g_file_set_contents(bad, "bad upstream data", -1, NULL));
    g_autofree char *combined = g_build_filename(dir, "china.cidr", NULL);
    g_autofree char *before = NULL, *after = NULL;
    g_assert_true(g_file_get_contents(combined, &before, NULL, NULL));
    g_autoptr(GSubprocess) process =
        g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                         NULL, UPDATER, "--source", bad, "--output", dir, NULL);
    g_assert_nonnull(process);
    g_assert_true(g_subprocess_wait(process, NULL, NULL));
    g_assert_false(g_subprocess_get_successful(process));
    g_assert_true(g_file_get_contents(combined, &after, NULL, NULL));
    g_assert_cmpstr(before, ==, after);
    const char *files[] = {"china.cidr", "china-ipv4.cidr", "china-ipv6.cidr", "china.json"};
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *file = g_build_filename(dir, files[i], NULL);
        g_unlink(file);
    }
    g_unlink(input);
    g_unlink(bad);
    g_rmdir(dir);
    g_autofree char *parent = g_path_get_dirname(dir);
    g_rmdir(parent);
}

static void snapshot(void) {
    g_autoptr(GError) error = NULL;
    JsonArray *a = ss_cidrs_load(SOURCE_DIR "/data/china/china.cidr", &error);
    g_assert_no_error(error);
    g_assert_nonnull(a);
    guint counts[2] = {0, 0};
    for (guint i = 0; i < json_array_get_length(a); i++)
        counts[strchr(json_array_get_string_element(a, i), ':') ? 1 : 0]++;
    g_assert_cmpuint(counts[0], >, 1000);
    g_assert_cmpuint(counts[1], >, 100);
    json_array_unref(a);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_file(parser, SOURCE_DIR "/data/china/china.json", &error));
    g_assert_no_error(error);
    JsonObject *metadata = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpuint(counts[0], ==, json_object_get_int_member(metadata, "ipv4_prefixes"));
    g_assert_cmpuint(counts[1], ==, json_object_get_int_member(metadata, "ipv6_prefixes"));
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(SOURCE_DIR "/data/china/china.cidr", &text, NULL, NULL));
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
    g_assert_cmpstr(hash, ==, ss_string(metadata, "sha256"));
    for (guint family = 0; family < 2; family++) {
        a = ss_cidrs_load(family ? SOURCE_DIR "/data/china/china-ipv6.cidr"
                                 : SOURCE_DIR "/data/china/china-ipv4.cidr",
                          NULL);
        g_assert_nonnull(a);
        g_assert_cmpuint(json_array_get_length(a), ==, counts[family]);
        json_array_unref(a);
    }
}

int main(int argc, char **argv) {
    test_home = g_dir_make_tmp("nmss-regional-test-XXXXXX", NULL);
    g_assert_nonnull(test_home);
    g_setenv("XDG_DATA_HOME", test_home, TRUE);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/regional/default-and-failed-update", default_and_rollback);
    g_test_add_func("/regional/bundled-snapshot-integrity", snapshot);
    int result = g_test_run();
    g_rmdir(test_home);
    g_free(test_home);
    return result;
}
