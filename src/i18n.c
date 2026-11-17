/*
 * i18n.c
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


#include "i18n.h"
#include <locale.h>

void ss_i18n_bind(void) {
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        bindtextdomain(GETTEXT_PACKAGE, SS_LOCALEDIR);
        bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
        g_once_init_leave(&initialized, 1);
    }
}

void ss_i18n_init(void) {
    setlocale(LC_ALL, "");
    ss_i18n_bind();
}
