/*
 * plugin.c
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
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

/* Extra executables require an administrator-owned entry in this directory. */
#define PLUGIN_DIR "/usr/libexec/NetworkManager/shadowsocks/plugins/"

static gboolean trusted_path(const char *path) {
    g_autofree char *part = g_strdup(path);
    while (*part) {
        struct stat st;
        if (stat(part, &st) || st.st_uid != 0 || (st.st_mode & 0022))
            return FALSE;
        if (!strcmp(part, "/"))
            break;
        char *slash = strrchr(part, '/');
        if (!slash)
            return FALSE;
        if (slash == part)
            part[1] = '\0';
        else
            *slash = '\0';
    }
    return TRUE;
}

char *ss_plugin_resolve(const char *program, GError **error) {
    const char *known[] = {"obfs-local",    "v2ray-plugin", "xray-plugin",
                           "kcptun-client", "shadow-tls",   NULL};
    g_autofree char *base = g_path_get_basename(program);
    gboolean standard = g_strv_contains(known, base);
    g_autofree char *path = NULL;
    if (g_path_is_absolute(program)) {
        if (standard ||
            (g_str_has_prefix(program, PLUGIN_DIR) && !strchr(program + strlen(PLUGIN_DIR), '/') &&
             strcmp(base, ".") && strcmp(base, "..")))
            path = g_strdup(program);
    } else {
        const char *dirs[] = {PLUGIN_DIR, "/usr/bin", "/usr/local/bin", NULL};
        for (guint i = 0; dirs[i]; i++) {
            if (i && !standard)
                continue;
            g_autofree char *candidate = g_build_filename(dirs[i], program, NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
                path = g_steal_pointer(&candidate);
                break;
            }
        }
    }
    g_autofree char *resolved = path ? realpath(path, NULL) : NULL;
    struct stat st;
    if (!resolved || !trusted_path(path) || !trusted_path(resolved) || stat(resolved, &st) ||
        !S_ISREG(st.st_mode) || access(resolved, X_OK)) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
            _("Plugin is missing or untrusted; install a supported system plugin or ask an administrator to register it in /usr/libexec/NetworkManager/shadowsocks/plugins"));
        return NULL;
    }
    return g_steal_pointer(&resolved);
}
