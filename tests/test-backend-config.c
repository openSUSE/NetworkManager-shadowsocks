/*
 * test-backend-config.c
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

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;

    g_autoptr(GError) error = NULL;
    g_autoptr(SsProfile) profile = ss_profile_load(argv[1], &error);

    if (!profile) {
        g_printerr("%s\n", error->message);
        return 1;
    }

    g_autofree char *config = ss_backend_json(profile);
    g_print("%s\n", config);
    return 0;
}
