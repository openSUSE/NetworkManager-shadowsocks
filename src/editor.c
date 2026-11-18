/*
 * editor.c
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
#include <gtk/gtk.h>
#include <gmodule.h>
#include <string.h>
#include "gtk-compat.h"

static const char *methods[] = {"aes-128-gcm", "aes-256-gcm", "chacha20-ietf-poly1305",
    "2022-blake3-aes-128-gcm", "2022-blake3-aes-256-gcm", "2022-blake3-chacha20-poly1305", NULL};
    
typedef struct {
    GObject parent;
    GtkWidget *root, *tabs, *same, *mode, *ip, *dns, *bypass, *regional_button, *status;
    GtkWidget *server[2], *port[2], *method[2], *password[2], *index[2];
    GtkWidget *plugin[2], *plugin_opts[2], *plugin_args[2], *plugin_mode[2];
    JsonArray *regional;
    gboolean password_edited[2], applying_secrets, secrets_loading, persisted, initial_same;
    NMSettingSecretFlags saved_flags[2];
    GCancellable *secrets_cancel;
    char *uuid;
} SsEditor;

typedef struct { GObjectClass parent; } SsEditorClass;

static void editor_iface(NMVpnEditorInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SsEditor, ss_editor, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(NM_TYPE_VPN_EDITOR, editor_iface))

static void changed(GtkWidget *widget, gpointer data) {
    SsEditor *self = data;
    if (self->applying_secrets) return;
    for (guint i = 0; i < 2; i++)
        if (widget == self->password[i]) self->password_edited[i] = TRUE;
    g_signal_emit_by_name(data, "changed");
}

static GtkWidget *entry(const char *text) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text ? text : "");
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static void row(GtkWidget *grid, guint n, const char *label, GtkWidget *widget) {
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), text, 0, n, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, n, 1, 1);
}

static GtkWidget *combo(const char *const *ids, const char *const *labels, const char *value) {
    GtkWidget *w = gtk_combo_box_text_new();
    for (guint i = 0; ids[i]; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w), ids[i], labels ? labels[i] : ids[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(w), value);
    return w;
}

static void fill_node(SsEditor *self, guint i, JsonObject *node) {
    gtk_entry_set_text(GTK_ENTRY(self->server[i]), node ? ss_string(node, "server") : "");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->port[i]), node ? json_object_get_int_member(node, "server_port") : 8388);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->method[i]), node ? ss_string(node, "method") : "aes-256-gcm");
    gtk_entry_set_text(GTK_ENTRY(self->password[i]), node ? ss_string(node, "password") : "");
    gtk_entry_set_text(GTK_ENTRY(self->plugin[i]), ss_string(node, "plugin"));
    gtk_entry_set_text(GTK_ENTRY(self->plugin_opts[i]), ss_string(node, "plugin_opts"));
    const char *transport = ss_string(node, "plugin_mode");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->plugin_mode[i]), *transport ? transport : "tcp_only");
    JsonNode *args = node ? json_object_get_member(node, "plugin_args") : NULL;
    g_autofree char *text = args ? json_to_string(args, FALSE) : NULL;
    gtk_entry_set_text(GTK_ENTRY(self->plugin_args[i]), text ? text : "[]");
}

static void regional_label(SsEditor *self) {
    guint v4 = 0, v6 = 0;
    for (guint i = 0; i < json_array_get_length(self->regional); i++) {
        if (strchr(json_array_get_string_element(self->regional, i), ':')) v6++; else v4++;
    }
    g_autofree char *label = g_strdup_printf(_("Regional prefixes: IPv4 %u / IPv6 %u - Import custom list..."), v4, v6);
    gtk_button_set_label(GTK_BUTTON(self->regional_button), label);
}

static void regional_default_clicked(GtkButton *button, gpointer data) {
    (void) button;
    SsEditor *self = data;
    g_autoptr(GError) error = NULL;
    g_autofree char *source = NULL;
    JsonArray *array = ss_regional_default(&source, &error);
    if (!array) { gtk_label_set_text(GTK_LABEL(self->status), error->message); return; }
    json_array_unref(self->regional); self->regional = array;
    regional_label(self); gtk_label_set_text(GTK_LABEL(self->status), source);
    changed(NULL, self);
}

typedef struct {
    SsEditor *editor;
    guint protocol;
    gboolean cidrs;
} ImportRequest;
static void import_response(GtkNativeDialog *dialog, int response, gpointer data) {
    ImportRequest *request = data;
    SsEditor *self = request->editor;
    if (response == GTK_RESPONSE_ACCEPT) {
        g_autoptr(GFile) file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        g_autofree char *path = file ? g_file_get_path(file) : NULL;
        g_autoptr(GError) error = NULL;
        if (path && request->cidrs) {
            JsonArray *list = ss_cidrs_load(path, &error);
            if (list) {
                json_array_unref(self->regional);
                self->regional = list;
                regional_label(self);
                changed(NULL, self);
            }
        } else if (path) {
            guint i = request->protocol;
            JsonObject *node = ss_import_node(path,
                gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->index[i])), &error);
            if (node) {
                fill_node(self, i, node);
                json_object_unref(node);
                gtk_label_set_text(GTK_LABEL(self->status), _("Node imported. Save to apply changes."));
                changed(NULL, self);
            }
        }
        if (error) gtk_label_set_text(GTK_LABEL(self->status), error->message);
    }
    gtk_native_dialog_destroy(dialog);
    g_object_unref(dialog);
    g_object_unref(self);
    g_free(request);
}

static void choose(SsEditor *self, gboolean cidrs, guint protocol) {
#if GTK_CHECK_VERSION(4, 0, 0)
    GtkRoot *top = gtk_widget_get_root(self->root);
#else
    GtkWidget *top = gtk_widget_get_toplevel(self->root);
#endif
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        cidrs ? _("Select a CIDR text file (one prefix per line)")
              : _("Import node JSON (using the node index below)"),
        GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        _("Open"), _("Cancel"));
    ImportRequest *request = g_new0(ImportRequest, 1);
    request->editor = g_object_ref(self);
    request->cidrs = cidrs;
    request->protocol = protocol;
    g_signal_connect(dialog, "response", G_CALLBACK(import_response), request);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void import_clicked(GtkButton *button, gpointer data) {
    choose(data, FALSE, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "protocol")));
}

static void regional_clicked(GtkButton *button, gpointer data) {
    (void) button;
    choose(data, TRUE, 0);
}

static void same_toggled(SsCheckButton *button, gpointer data) {
    SsEditor *self = data;
    gtk_widget_set_sensitive(gtk_notebook_get_nth_page(GTK_NOTEBOOK(self->tabs), 1), !ss_check_button_get_active(button));
    changed(NULL, self);
}

static GObject *get_widget(NMVpnEditor *editor)
{
    return G_OBJECT(((SsEditor *) editor)->root);
}

static JsonObject *collect_node(SsEditor *self, guint i, GError **error) {
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "server", gtk_entry_get_text(GTK_ENTRY(self->server[i])));
    json_object_set_int_member(o, "server_port", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->port[i])));
    const char *method = gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->method[i]));
    json_object_set_string_member(o, "method", method ? method : "");
    json_object_set_string_member(o, "password", gtk_entry_get_text(GTK_ENTRY(self->password[i])));
    const char *program = gtk_entry_get_text(GTK_ENTRY(self->plugin[i]));
    const char *options = gtk_entry_get_text(GTK_ENTRY(self->plugin_opts[i]));
    const char *args = gtk_entry_get_text(GTK_ENTRY(self->plugin_args[i]));
    if (*program || *options || (*args && strcmp(args, "[]"))) {
        json_object_set_string_member(o, "plugin", program);
        json_object_set_string_member(o, "plugin_opts", options);
        json_object_set_string_member(o, "plugin_mode", gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->plugin_mode[i])));
        if (*args) {
            g_autoptr(JsonParser) parser = json_parser_new();
            if (!json_parser_load_from_data(parser, args, -1, error)) { json_object_unref(o); return NULL; }
            json_object_set_member(o, "plugin_args", json_node_copy(json_parser_get_root(parser)));
        }
    }
    return o;
}

static JsonArray *split_array(const char *text) {
    JsonArray *array = json_array_new();
    g_auto(GStrv) strings = g_strsplit(text, ",", -1);
    for (guint i = 0; strings[i]; i++) {
        char *s = g_strstrip(strings[i]);
        if (*s) json_array_add_string_element(array, s);
    }
    return array;
}

static gboolean update_connection_impl(NMVpnEditor *editor, NMConnection *conn, GError **error) {
    SsEditor *self = (SsEditor *) editor;
    g_autoptr(SsProfile) p = ss_profile_new();
    const char *id = nm_connection_get_id(conn);
    json_object_set_string_member(p->obj, "name", id && *id ? id : "Shadowsocks");
    JsonObject *tcp = collect_node(self, 0, error);
    if (!tcp) return FALSE;
    json_object_set_object_member(p->obj, "tcp", tcp);
    JsonObject *udp = collect_node(self, ss_check_button_get_active(SS_CHECK_BUTTON(self->same)) ? 0 : 1, error);
    if (!udp) return FALSE;
    json_object_set_object_member(p->obj, "udp", udp);
    json_object_set_string_member(p->obj, "mode", gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->mode)));
    json_object_set_string_member(p->obj, "ip_mode", gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->ip)));
    json_object_set_array_member(p->obj, "dns", split_array(gtk_entry_get_text(GTK_ENTRY(self->dns))));
    json_object_set_array_member(p->obj, "bypass", split_array(gtk_entry_get_text(GTK_ENTRY(self->bypass))));
    json_object_set_array_member(p->obj, "china", json_array_ref(self->regional));
    gboolean preserve = FALSE;
    for (guint i = 0; i < 2; i++) {
        guint source = ss_check_button_get_active(SS_CHECK_BUTTON(self->same)) ? 0 : i;
        JsonObject *node = ss_node(p, i);
        if (!*ss_string(node, "password") && self->persisted && !self->password_edited[source]) {
            NMSettingVpn *old = nm_connection_get_setting_vpn(conn);
            const char *known = old ? nm_setting_vpn_get_secret(old, i ? "udp-password" : "tcp-password") : NULL;
            if (known && *known) json_object_set_string_member(node, "password", known);
            else {
                /* Validate public fields without ever saving this temporary value. */
                json_object_set_string_member(node, "password", "__unchanged_secret__");
                preserve = TRUE;
            }
        }
    }
    if (preserve && ss_check_button_get_active(SS_CHECK_BUTTON(self->same)) != self->initial_same) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            _("Load or enter passwords before changing whether the UDP node shares the TCP node."));
        return FALSE;
    }
    if (preserve && (self->password_edited[0] ||
        (!ss_check_button_get_active(SS_CHECK_BUTTON(self->same)) && self->password_edited[1]))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            _("To change passwords while saved passwords are unavailable, enter passwords for both nodes."));
        return FALSE;
    }
    g_autoptr(NMConnection) updated = nm_simple_connection_new_clone(conn);
    if (!ss_profile_use_default_regional(p, error) || !ss_connection_update(p, updated, error)) return FALSE;
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(updated);
    if (preserve) {
        /* NetworkManager preserves stored secrets only when the update contains
         * no secrets at all. A partial secret update could erase the other key. */
        nm_connection_clear_secrets(updated);
        for (guint i = 0; i < 2; i++)
            nm_setting_set_secret_flags(NM_SETTING(vpn), i ? "udp-password" : "tcp-password",
                self->saved_flags[i], NULL);
    }
    nm_connection_add_setting(conn, nm_setting_duplicate(NM_SETTING(vpn)));
    for (guint i = 0; i < 2; i++) {
        NMSettingIPConfig *ip = i == 0 ? nm_connection_get_setting_ip4_config(conn) : nm_connection_get_setting_ip6_config(conn);
        if (!ip) {
            NMSetting *s = i == 0 ? nm_setting_ip4_config_new() : nm_setting_ip6_config_new();
            g_object_set(s, NM_SETTING_IP_CONFIG_METHOD, "auto", NULL);
            nm_connection_add_setting(conn, s);
            ip = NM_SETTING_IP_CONFIG(s);
        }
        g_object_set(ip, NM_SETTING_IP_CONFIG_NEVER_DEFAULT, TRUE, NM_SETTING_IP_CONFIG_DNS_PRIORITY, -5353, NULL);
        nm_setting_ip_config_add_dns_search(ip, "~.");
    }
    return TRUE;
}

static gboolean update_connection(NMVpnEditor *editor, NMConnection *conn, GError **error) {
    g_autoptr(GError) local_error = NULL;
    gboolean ok = update_connection_impl(editor, conn, &local_error);
    SsEditor *self = (SsEditor *) editor;
    if (!self->secrets_loading)
        gtk_label_set_text(GTK_LABEL(self->status), local_error ? local_error->message : "");
    if (local_error) g_propagate_error(error, g_steal_pointer(&local_error));
    return ok;
}

static void editor_iface(NMVpnEditorInterface *iface) {
    iface->get_widget = get_widget;
    iface->update_connection = update_connection;
}

static void ss_editor_finalize(GObject *object) {
    SsEditor *self = (SsEditor *) object;
    if (self->root) {
#if !GTK_CHECK_VERSION(4, 0, 0)
        gtk_widget_destroy(self->root);
#endif
        g_object_unref(self->root);
    }
    if (self->secrets_cancel) g_cancellable_cancel(self->secrets_cancel);
    g_clear_object(&self->secrets_cancel);
    g_free(self->uuid);
    json_array_unref(self->regional);
    G_OBJECT_CLASS(ss_editor_parent_class)->finalize(object);
}

static void ss_editor_class_init(SsEditorClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ss_editor_finalize;
}

static void ss_editor_init(SsEditor *self)
{
    self->regional = json_array_new();
}

static char *join_array(JsonArray *array) {
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        if (i) g_string_append(s, ", ");
        g_string_append(s, json_array_get_string_element(array, i));
    }
    return g_string_free(s, FALSE);
}

/* Hosts may give us a cloned connection with no secrets. Never block the UI
 * while contacting NetworkManager, and never overwrite a user's password edit. */
typedef struct {
    GWeakRef editor;
    GDBusConnection *bus;
} SecretRequest;

static void secret_request_free(SecretRequest *request) {
    g_weak_ref_clear(&request->editor);
    g_clear_object(&request->bus);
    g_free(request);
}

static void secrets_complete(SecretRequest *request, GVariant *settings, const GError *error) {
    g_autoptr(GObject) object = g_weak_ref_get(&request->editor);
    if (object) {
        SsEditor *self = (SsEditor *) object;
        g_autoptr(GVariant) vpn = settings ? g_variant_lookup_value(settings, "vpn", G_VARIANT_TYPE_VARDICT) : NULL;
        g_autoptr(GVariant) secrets = vpn ? g_variant_lookup_value(vpn, "secrets", G_VARIANT_TYPE("a{ss}")) : NULL;
        self->applying_secrets = TRUE;
        for (guint i = 0; i < 2; i++) {
            const char *password = NULL;
            if (!self->password_edited[i] && secrets &&
                g_variant_lookup(secrets, i ? "udp-password" : "tcp-password", "&s", &password))
                gtk_entry_set_text(GTK_ENTRY(self->password[i]), password);
        }
        self->applying_secrets = FALSE;
        self->secrets_loading = FALSE;
        gboolean missing = !*gtk_entry_get_text(GTK_ENTRY(self->password[0])) ||
            (!ss_check_button_get_active(SS_CHECK_BUTTON(self->same)) &&
             !*gtk_entry_get_text(GTK_ENTRY(self->password[1])));
        g_autofree char *message = missing ? (error
            ? g_strdup_printf(_("Cannot load saved passwords: %s. Leave passwords unchanged to keep the saved values."), error->message)
            : g_strdup(_("Saved passwords were not returned. Leave passwords unchanged to keep the saved values."))) : NULL;
        changed(NULL, self);
        /* A synchronous host validation must not hide the retrieval error. */
        gtk_label_set_text(GTK_LABEL(self->status), message ? message : "");
    }
    secret_request_free(request);
}

static void secrets_loaded(GObject *source, GAsyncResult *result, gpointer data) {
    SecretRequest *request = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_autoptr(GVariant) settings = reply ? g_variant_get_child_value(reply, 0) : NULL;
    secrets_complete(request, settings, error);
}

static void secrets_path_ready(GObject *source, GAsyncResult *result, gpointer data) {
    SecretRequest *request = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_autoptr(GObject) object = g_weak_ref_get(&request->editor);
    if (reply && object) {
        SsEditor *self = (SsEditor *) object;
        self->persisted = TRUE;
        for (guint i = 0; i < 2; i++)
            gtk_entry_set_placeholder_text(GTK_ENTRY(self->password[i]),
                _("Leave unchanged to keep the saved password"));
        changed(NULL, self);
        const char *path;
        g_variant_get(reply, "(&o)", &path);
        g_dbus_connection_call(request->bus, NM_DBUS_SERVICE, path,
            NM_DBUS_INTERFACE_SETTINGS_CONNECTION, "GetSecrets", g_variant_new("(s)", "vpn"),
            G_VARIANT_TYPE("(a{sa{sv}})"), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
            15000, self->secrets_cancel, secrets_loaded, request);
        return;
    }
    secrets_complete(request, NULL, error);
}

static void secrets_bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
    (void) source;
    SecretRequest *request = data;
    g_autoptr(GError) error = NULL;
    request->bus = g_bus_get_finish(result, &error);
    g_autoptr(GObject) object = g_weak_ref_get(&request->editor);
    if (request->bus && object) {
        SsEditor *self = (SsEditor *) object;
        g_dbus_connection_call(request->bus, NM_DBUS_SERVICE, NM_DBUS_PATH_SETTINGS,
            NM_DBUS_INTERFACE_SETTINGS, "GetConnectionByUuid", g_variant_new("(s)", self->uuid),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000,
            self->secrets_cancel, secrets_path_ready, request);
        return;
    }
    secrets_complete(request, NULL, error);
}

static void load_passwords(SsEditor *self) {
    if (!self->uuid || self->secrets_loading) return;
    self->secrets_loading = TRUE;
    g_clear_object(&self->secrets_cancel);
    self->secrets_cancel = g_cancellable_new();
    gtk_label_set_text(GTK_LABEL(self->status), _("Loading saved passwords... You can also enter them manually."));
    SecretRequest *request = g_new0(SecretRequest, 1);
    g_weak_ref_init(&request->editor, self);
    g_bus_get(G_BUS_TYPE_SYSTEM, self->secrets_cancel, secrets_bus_ready, request);
}

static NMVpnEditor *new_editor(NMConnection *conn, GError **error) {
    g_autoptr(SsProfile) p = NULL;
    NMSettingVpn *vpn = conn ? nm_connection_get_setting_vpn(conn) : NULL;
    if (vpn && nm_setting_vpn_get_data_item(vpn, "profile")) {
        p = ss_profile_from_connection(conn, FALSE, error);
        if (!p) return NULL;
    } else p = ss_profile_new();
    SsEditor *self = g_object_new(ss_editor_get_type(), NULL);
    self->root = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_container_set_border_width(GTK_CONTAINER(self->root), 12);
    GtkWidget *title = gtk_label_new(_("Shadowsocks - Transparent proxy"));
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(self->root), title, FALSE, FALSE, 0);
    self->status = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(self->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->status), 0);
    gtk_widget_set_name(self->status, "editor-status");
    gtk_box_pack_start(GTK_BOX(self->root), self->status, FALSE, FALSE, 0);
    self->uuid = vpn ? g_strdup(nm_connection_get_uuid(conn)) : NULL;
    for (guint i = 0; vpn && i < 2; i++)
        nm_setting_get_secret_flags(NM_SETTING(vpn), i ? "udp-password" : "tcp-password",
            &self->saved_flags[i], NULL);
    self->tabs = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(self->root), self->tabs, FALSE, FALSE, 0);
    for (guint i = 0; i < 2; i++) {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
        self->server[i] = entry("");
        self->port[i] = gtk_spin_button_new_with_range(1, 65535, 1);
        self->method[i] = combo(methods, NULL, "aes-256-gcm");
        self->password[i] = entry("");
        gtk_widget_set_name(self->password[i], i ? "udp-password" : "tcp-password");
        gtk_entry_set_visibility(GTK_ENTRY(self->password[i]), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(self->password[i]), GTK_INPUT_PURPOSE_PASSWORD);
        row(grid, 0, _("Server"), self->server[i]); row(grid, 1, _("Port"), self->port[i]);
        row(grid, 2, _("Cipher"), self->method[i]); row(grid, 3, _("Password / Key"), self->password[i]);
        GtkWidget *button = gtk_button_new_with_label(_("Import from JSON..."));
        g_object_set_data(G_OBJECT(button), "protocol", GUINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(import_clicked), self);
        gtk_grid_attach(GTK_GRID(grid), button, 0, 4, 2, 1);
        self->index[i] = gtk_spin_button_new_with_range(0, 100000, 1);
        row(grid, 5, _("Import node index"), self->index[i]);
        self->plugin[i] = entry("");
        self->plugin_opts[i] = entry("");
        self->plugin_args[i] = entry("[]");
        const char *transport_ids[] = {"tcp_only", "udp_only", "tcp_and_udp", NULL};
        const char *transport_labels[] = {_("TCP only"), _("UDP only"), _("TCP and UDP"), NULL};
        self->plugin_mode[i] = combo(transport_ids, transport_labels, "tcp_only");
        gtk_widget_set_name(self->plugin_mode[i], i ? "udp-plugin-mode" : "tcp-plugin-mode");
        gtk_widget_set_tooltip_text(self->plugin_mode[i], _("Most SIP003 plugins support TCP only. Select UDP only or TCP and UDP only if your plugin supports it."));
        gtk_widget_set_name(self->plugin[i], i ? "udp-plugin" : "tcp-plugin");
        gtk_widget_set_name(self->plugin_opts[i], i ? "udp-plugin-options" : "tcp-plugin-options");
        gtk_widget_set_name(self->plugin_args[i], i ? "udp-plugin-arguments" : "tcp-plugin-arguments");
        gtk_entry_set_placeholder_text(GTK_ENTRY(self->plugin[i]), _("None, obfs-local, v2ray-plugin, ..."));
        gtk_entry_set_placeholder_text(GTK_ENTRY(self->plugin_opts[i]), "obfs=http;obfs-host=www.example.com");
        gtk_widget_set_tooltip_text(self->plugin_args[i], _("JSON array of command arguments; no shell is used"));
        row(grid, 6, _("Obfuscation plugin (SIP003)"), self->plugin[i]);
        row(grid, 7, _("Plugin options"), self->plugin_opts[i]);
        row(grid, 8, _("Plugin arguments (JSON)"), self->plugin_args[i]);
        row(grid, 9, _("Plugin transport"), self->plugin_mode[i]);
        gtk_notebook_append_page(GTK_NOTEBOOK(self->tabs), grid, gtk_label_new(i == 0 ? _("TCP node") : _("UDP node")));
        fill_node(self, i, ss_node(p, i));
        g_signal_connect(self->server[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->port[i], "value-changed", G_CALLBACK(changed), self);
        g_signal_connect(self->method[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->password[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->plugin[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->plugin_opts[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->plugin_args[i], "changed", G_CALLBACK(changed), self);
        g_signal_connect(self->plugin_mode[i], "changed", G_CALLBACK(changed), self);
    }
    self->same = gtk_check_button_new_with_label(_("Use the TCP node for UDP (uncheck to configure separately)"));
    gboolean equal = !ss_node(p, 0);
    if (ss_node(p, 0) && ss_node(p, 1)) {
        equal = !strcmp(ss_string(ss_node(p, 0), "server"), ss_string(ss_node(p, 1), "server")) &&
            !strcmp(ss_string(ss_node(p, 0), "password"), ss_string(ss_node(p, 1), "password")) &&
            !strcmp(ss_string(ss_node(p, 0), "method"), ss_string(ss_node(p, 1), "method")) &&
            json_object_get_int_member(ss_node(p, 0), "server_port") == json_object_get_int_member(ss_node(p, 1), "server_port");
        const char *fields[] = {"plugin", "plugin_opts", "plugin_args", "plugin_mode", NULL};
        for (guint j = 0; equal && fields[j]; j++) {
            JsonNode *a = json_object_get_member(ss_node(p, 0), fields[j]);
            JsonNode *b = json_object_get_member(ss_node(p, 1), fields[j]);
            g_autofree char *at = a ? json_to_string(a, FALSE) : NULL;
            g_autofree char *bt = b ? json_to_string(b, FALSE) : NULL;
            equal = !g_strcmp0(at, bt);
        }
    }
    self->initial_same = equal;
    ss_check_button_set_active(SS_CHECK_BUTTON(self->same), equal);
    g_signal_connect(self->same, "toggled", G_CALLBACK(same_toggled), self);
    gtk_widget_set_sensitive(gtk_notebook_get_nth_page(GTK_NOTEBOOK(self->tabs), 1), !equal);
    gtk_box_pack_start(GTK_BOX(self->root), self->same, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10); gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    const char *mode_ids[] = {"global", "lan", "china", NULL};
    const char *mode_labels[] = {_("Global proxy"), _("Bypass LAN"), _("Bypass regional IPs (defaults to mainland China IPs) and LAN"), NULL};
    const char *ip_ids[] = {"dual", "ipv4", "ipv6", NULL};
    const char *ip_labels[] = {"IPv4 + IPv6", _("IPv4 only"), _("IPv6 only"), NULL};
    self->mode = combo(mode_ids, mode_labels, ss_string(p->obj, "mode"));
    self->ip = combo(ip_ids, ip_labels, ss_string(p->obj, "ip_mode"));
    g_autofree char *dns = join_array(json_object_get_array_member(p->obj, "dns"));
    g_autofree char *bypass = join_array(json_object_get_array_member(p->obj, "bypass"));
    self->dns = entry(dns); self->bypass = entry(bypass);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->bypass), _("Comma-separated CIDRs (optional)"));
    gtk_widget_set_tooltip_text(self->dns, _("Comma-separated DNS IPs matching the enabled address families; DNS always uses the proxy."));
    row(grid, 0, _("Proxy mode"), self->mode); row(grid, 1, _("Address families"), self->ip);
    row(grid, 2, "DNS", self->dns); row(grid, 3, _("Additional bypass"), self->bypass);
    gtk_box_pack_start(GTK_BOX(self->root), grid, FALSE, FALSE, 0);
    json_array_unref(self->regional);
    self->regional = json_array_ref(json_object_get_array_member(p->obj, "china"));
    self->regional_button = gtk_button_new(); regional_label(self);
    g_signal_connect(self->regional_button, "clicked", G_CALLBACK(regional_clicked), self);
    gtk_box_pack_start(GTK_BOX(self->root), self->regional_button, FALSE, FALSE, 0);
    GtkWidget *defaults = gtk_button_new_with_label(_("Use latest default prefixes (updated dataset / bundled snapshot)"));
    g_signal_connect(defaults, "clicked", G_CALLBACK(regional_default_clicked), self);
    gtk_box_pack_start(GTK_BOX(self->root), defaults, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new(_("UDP and TCP DNS queries use their respective nodes. Only TCP/UDP is proxied.\nSingle-stack mode blocks non-bypassed TCP/UDP traffic of the other address family."));
    gtk_label_set_xalign(GTK_LABEL(hint), 0); gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(self->root), hint, FALSE, FALSE, 0);
    GtkWidget *widgets[] = {self->mode, self->ip, self->dns, self->bypass};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) g_signal_connect(widgets[i], "changed", G_CALLBACK(changed), self);
#if !GTK_CHECK_VERSION(4, 0, 0)
    gtk_widget_show_all(self->root);
#endif
    if (vpn && nm_connection_get_uuid(conn) &&
        (!nm_setting_vpn_get_secret(vpn, "tcp-password") ||
         !nm_setting_vpn_get_secret(vpn, "udp-password"))) {
        load_passwords(self);
    }
    return NM_VPN_EDITOR(self);
}

G_MODULE_EXPORT NMVpnEditor *ss_editor_factory(NMConnection *conn, GError **error) {
    return new_editor(conn, error);
}
