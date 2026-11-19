/*
 * regional.c
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
#include <string.h>

static gboolean invalid(GError **error, const char *detail) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("Invalid APNIC data: %s"), detail);
    return FALSE;
}

JsonArray *ss_regional_default(char **source, GError **error) {
    g_autofree char *user = g_build_filename(g_get_user_data_dir(), "NetworkManager-shadowsocks",
                                             "china", "china.cidr", NULL);
    const char *paths[] = {user, SS_REGIONAL_STATE "/china.cidr",
                           SS_REGIONAL_BUNDLED "/china.cidr"};
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        if (!g_file_test(paths[i], G_FILE_TEST_EXISTS))
            continue;
        JsonArray *array = ss_cidrs_load(paths[i], error);
        if (!array)
            return NULL; /* Do not silently hide a broken user update. */
        gboolean v4 = FALSE, v6 = FALSE;
        for (guint j = 0; j < json_array_get_length(array); j++) {
            g_autoptr(GInetAddressMask) mask =
                g_inet_address_mask_new_from_string(json_array_get_string_element(array, j), NULL);
            if (g_inet_address_mask_get_family(mask) == G_SOCKET_FAMILY_IPV4)
                v4 = TRUE;
            else
                v6 = TRUE;
        }
        if (!v4 || !v6) {
            json_array_unref(array);
            invalid(error, _("Default dataset is missing IPv4 or IPv6 prefixes"));
            return NULL;
        }
        if (source)
            *source = g_strdup(paths[i]);
        return array;
    }
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        _("Default China dataset not found; install the data files or run nm-shadowsocks-update-routes"));
    return NULL;
}

gboolean ss_profile_use_default_regional(SsProfile *p, GError **error) {
    JsonNode *n = json_object_get_member(p->obj, "china");
    if (strcmp(ss_string(p->obj, "mode"), "china") || !n || !JSON_NODE_HOLDS_ARRAY(n) ||
        json_array_get_length(json_node_get_array(n)))
        return TRUE;
    JsonArray *array = ss_regional_default(NULL, error);
    if (!array)
        return FALSE;
    json_object_set_array_member(p->obj, "china", array);
    return TRUE;
}
