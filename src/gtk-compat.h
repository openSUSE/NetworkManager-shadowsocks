/*
 * gtk-compat.h
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

#pragma once

#include <gtk/gtk.h>

#if GTK_CHECK_VERSION(4, 0, 0)
#define gtk_entry_set_text(e, t) gtk_editable_set_text(GTK_EDITABLE(e), t)
#define gtk_entry_get_text(e) gtk_editable_get_text(GTK_EDITABLE(e))
#define SsCheckButton GtkCheckButton
#define SS_CHECK_BUTTON GTK_CHECK_BUTTON
#define ss_check_button_get_active gtk_check_button_get_active
#define ss_check_button_set_active gtk_check_button_set_active
#define gtk_label_set_line_wrap gtk_label_set_wrap
#define GTK_CONTAINER(w) (w)

static inline void gtk_container_set_border_width(GtkWidget *widget, guint border) {
    gtk_widget_set_margin_start(widget, border);
    gtk_widget_set_margin_end(widget, border);
    gtk_widget_set_margin_top(widget, border);
    gtk_widget_set_margin_bottom(widget, border);
}

static inline void gtk_box_pack_start(GtkBox *box, GtkWidget *child, gboolean expand, gboolean fill,
                                      guint padding) {
    (void)fill;
    (void)padding;
    gtk_widget_set_vexpand(child, expand);
    gtk_box_append(box, child);
}

#else
#define SsCheckButton GtkToggleButton
#define SS_CHECK_BUTTON GTK_TOGGLE_BUTTON
#define ss_check_button_get_active gtk_toggle_button_get_active
#define ss_check_button_set_active gtk_toggle_button_set_active
#endif
