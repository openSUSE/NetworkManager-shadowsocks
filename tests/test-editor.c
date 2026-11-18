/*
 * test-editor.c
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
#include <gmodule.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <locale.h>

static NMVpnEditorPlugin *plugin;

static void import_export(void) {
    g_autoptr(GError) error = NULL;
    g_autoptr(NMConnection) conn = nm_vpn_editor_plugin_import(plugin, SOURCE_DIR "/examples/profile.json", &error);
    g_assert_no_error(error); g_assert_nonnull(conn);
    g_assert_true(nm_connection_verify(conn, &error)); g_assert_no_error(error);
    g_autoptr(SsProfile) p = ss_profile_from_connection(conn, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ss_string(ss_node(p, 0), "server"), !=, ss_string(ss_node(p, 1), "server"));
    g_autofree char *dir = g_dir_make_tmp("nmss-editor-export-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "export.json", NULL);
    g_assert_true(nm_vpn_editor_plugin_export(plugin, path, conn, &error)); g_assert_no_error(error);
    g_autoptr(SsProfile) q = ss_profile_load(path, &error); g_assert_no_error(error); g_assert_nonnull(q);
    g_assert_cmpstr(ss_string(ss_node(p, 1), "password"), ==, ss_string(ss_node(q, 1), "password"));
    g_unlink(path); g_rmdir(dir);
}

static void metadata(void) {
    GModule *host = g_module_open(NULL, 0);
    gpointer symbol = NULL;
#if GTK_CHECK_VERSION(4, 0, 0)
    g_assert_false(g_module_symbol(host, "gtk_container_add", &symbol));
#else
    g_assert_false(g_module_symbol(host, "gtk_widget_get_first_child", &symbol));
#endif
    g_module_close(host);
    g_autofree char *service = NULL;
    g_object_get(plugin, NM_VPN_EDITOR_PLUGIN_SERVICE, &service, NULL);
    g_assert_cmpstr(service, ==, SS_SERVICE);
    g_assert_true(nm_vpn_editor_plugin_get_capabilities(plugin) & NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6);
}

static GtkWidget *find_widget(GtkWidget *root, const char *name) {
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
#if GTK_CHECK_VERSION(4, 0, 0)
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, name);
        if (found) return found;
    }
#else
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        GtkWidget *found = NULL;
        for (GList *item = children; item && !found; item = item->next)
            found = find_widget(item->data, name);
        g_list_free(children);
        if (found) return found;
    }
#endif
    return NULL;
}

static void count_changed(NMVpnEditor *editor, gpointer data) {
    (void) editor;
    (*(guint *) data)++;
}

static const char *password_text(GtkWidget *widget) {
#if GTK_CHECK_VERSION(4, 0, 0)
    return gtk_editable_get_text(GTK_EDITABLE(widget));
#else
    return gtk_entry_get_text(GTK_ENTRY(widget));
#endif
}

static void set_password_text(GtkWidget *widget, const char *text) {
#if GTK_CHECK_VERSION(4, 0, 0)
    gtk_editable_set_text(GTK_EDITABLE(widget), text);
#else
    gtk_entry_set_text(GTK_ENTRY(widget), text);
#endif
}

static void ui(void) {
#if GTK_CHECK_VERSION(4, 0, 0)
    if (!gtk_init_check()) { g_test_skip("No display server for GTK widget validation"); return; }
#else
    if (!gtk_init_check(NULL, NULL)) { g_test_skip("No display server for GTK widget validation"); return; }
#endif
    /* No desktop XSettings or font configuration is guaranteed in RPM workers. */
    g_object_set(gtk_settings_get_default(),
                 "gtk-font-name", "DejaVu Sans 10",
                 "gtk-xft-dpi", 96 * 1024,
                 NULL);
    g_autoptr(NMConnection) conn = nm_simple_connection_new();
    g_autoptr(GError) error = NULL;
    g_autoptr(NMVpnEditor) editor = nm_vpn_editor_plugin_get_editor(plugin, conn, &error);
    g_assert_no_error(error); g_assert_nonnull(editor);
    g_assert_true(GTK_IS_WIDGET(nm_vpn_editor_get_widget(editor)));
    GtkWidget *root = GTK_WIDGET(nm_vpn_editor_get_widget(editor));
#if GTK_CHECK_VERSION(4, 0, 0)
    GtkWidget *first = gtk_widget_get_first_child(root);
    g_assert_true(GTK_IS_LABEL(first));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(first)), ==, _("Shadowsocks - Transparent proxy"));
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), root);
#else
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    g_assert_nonnull(children);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(children->data)), ==, _("Shadowsocks - Transparent proxy"));
    g_list_free(children);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), root);
#endif
    gtk_window_present(GTK_WINDOW(window));
    /* Exercise real layout and dispatch, as the Settings host does. */
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND / 4;
    while (g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_true(gtk_widget_get_realized(root));
    metadata();
    /* Empty editor must reject an incomplete node, rather than save a broken VPN. */
    g_assert_false(nm_vpn_editor_update_connection(editor, conn, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_autoptr(NMConnection) existing = nm_vpn_editor_plugin_import(plugin, SOURCE_DIR "/examples/profile.json", &error);
    g_assert_no_error(error); g_assert_nonnull(existing);
    g_autoptr(SsProfile) with_plugin = ss_profile_from_connection(existing, TRUE, &error);
    g_assert_no_error(error);
    json_object_set_string_member(ss_node(with_plugin, 0), "plugin", "obfs-local");
    json_object_set_string_member(ss_node(with_plugin, 0), "plugin_opts", "obfs=http;obfs-host=www.example.com");
    json_object_set_string_member(ss_node(with_plugin, 1), "plugin", "v2ray-plugin");
    json_object_set_string_member(ss_node(with_plugin, 1), "plugin_opts", "mode=quic");
    json_object_set_string_member(ss_node(with_plugin, 1), "plugin_mode", "udp_only");
    JsonArray *args = json_array_new();
    json_array_add_string_element(args, "--verbose");
    json_object_set_array_member(ss_node(with_plugin, 0), "plugin_args", args);
    g_assert_true(ss_connection_update(with_plugin, existing, &error));
    g_assert_no_error(error);
    /* Reopening in Settings does not guarantee secrets are supplied. */
    nm_setting_vpn_remove_secret(nm_connection_get_setting_vpn(existing), "tcp-plugin_opts");
    nm_setting_vpn_remove_secret(nm_connection_get_setting_vpn(existing), "udp-plugin_opts");
    nm_setting_vpn_remove_secret(nm_connection_get_setting_vpn(existing), "tcp-plugin_args");
    g_autoptr(NMVpnEditor) filled = nm_vpn_editor_plugin_get_editor(plugin, existing, &error);
    g_assert_no_error(error);
    g_assert_true(nm_vpn_editor_update_connection(filled, existing, &error)); g_assert_no_error(error);
    g_assert_true(nm_connection_verify(existing, &error)); g_assert_no_error(error);
    g_autoptr(SsProfile) saved = ss_profile_from_connection(existing, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ss_string(ss_node(saved, 0), "plugin"), ==, "obfs-local");
    g_assert_cmpstr(ss_string(ss_node(saved, 0), "plugin_opts"), ==, "obfs=http;obfs-host=www.example.com");
    g_assert_cmpstr(ss_string(ss_node(saved, 1), "plugin"), ==, "v2ray-plugin");
    g_assert_cmpstr(ss_string(ss_node(saved, 1), "plugin_mode"), ==, "udp_only");
    g_assert_cmpstr(ss_string(ss_node(saved, 0), "plugin_mode"), ==, "tcp_only");
    g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(ss_node(saved, 0), "plugin_args"), 0), ==, "--verbose");
    GtkWidget *filled_root = GTK_WIDGET(nm_vpn_editor_get_widget(filled));
    g_assert_nonnull(find_widget(filled_root, "tcp-plugin-mode"));
    g_assert_nonnull(find_widget(filled_root, "udp-plugin-mode"));
    GtkWidget *password = find_widget(filled_root, "tcp-password");
    g_assert_nonnull(password);
    g_assert_cmpstr(password_text(password), ==, ss_string(ss_node(saved, 0), "password"));
    guint changes = 0;
    g_signal_connect(filled, "changed", G_CALLBACK(count_changed), &changes);
    GtkWidget *transport = find_widget(filled_root, "udp-plugin-mode");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(transport), "tcp_and_udp"));
    g_assert_cmpuint(changes, >, 0);
    g_assert_true(nm_vpn_editor_update_connection(filled, existing, &error));
    g_assert_no_error(error);
    g_autoptr(SsProfile) transport_saved = ss_profile_from_connection(existing, TRUE, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(ss_string(ss_node(transport_saved, 1), "plugin_mode"), ==, "tcp_and_udp");
    set_password_text(password, "changed-test-password");
    g_assert_cmpuint(changes, >, 0);
    g_assert_true(nm_vpn_editor_update_connection(filled, existing, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(nm_setting_vpn_get_secret(nm_connection_get_setting_vpn(existing), "tcp-password"), ==, "changed-test-password");
    NMSettingSecretFlags flags;
    g_assert_true(nm_setting_get_secret_flags(NM_SETTING(nm_connection_get_setting_vpn(existing)), "tcp-password", &flags, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(flags, ==, NM_SETTING_SECRET_FLAG_NONE);
    /* A host that did not fetch secrets must not save empty passwords. */
    g_autoptr(NMConnection) no_secrets = nm_simple_connection_new_clone(existing);
    nm_connection_clear_secrets(no_secrets);
    g_autoptr(NMVpnEditor) unloaded = nm_vpn_editor_plugin_get_editor(plugin, no_secrets, &error);
    g_assert_no_error(error);
    g_assert_nonnull(unloaded);
    g_assert_false(nm_vpn_editor_update_connection(unloaded, existing, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpstr(nm_setting_vpn_get_secret(nm_connection_get_setting_vpn(existing), "tcp-password"), ==, "changed-test-password");
#if GTK_CHECK_VERSION(4, 0, 0)
    gtk_window_destroy(GTK_WINDOW(window));
#else
    gtk_widget_destroy(window);
#endif

}
static guint secret_calls;
static gboolean secret_error;

static void mock_method(GDBusConnection *bus, const char *sender, const char *path,
                        const char *interface, const char *method, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer data) {
    (void) bus; (void) sender; (void) path; (void) interface; (void) parameters; (void) data;
    if (!strcmp(method, "GetConnectionByUuid")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", "/org/freedesktop/NetworkManager/Settings/1"));
        return;
    }
    secret_calls++;
    g_assert_true(g_dbus_message_get_flags(g_dbus_method_invocation_get_message(invocation)) &
                  G_DBUS_MESSAGE_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION);
    if (secret_error) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.NetworkManager.PermissionDenied", "Test authorization failure");
        return;
    }
    GVariantBuilder settings, vpn, secrets;
    g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&vpn, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_init(&secrets, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&secrets, "{ss}", "tcp-password", "stored-tcp-test");
    g_variant_builder_add(&secrets, "{ss}", "udp-password", "stored-udp-test");
    g_variant_builder_add(&vpn, "{sv}", "secrets", g_variant_builder_end(&secrets));
    g_variant_builder_add(&settings, "{s@a{sv}}", "vpn", g_variant_builder_end(&vpn));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sa{sv}})", g_variant_builder_end(&settings)));
}
static void secrets_async(void) {
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        return;
    }
    g_autoptr(GTestDBus) daemon = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(daemon);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(daemon), TRUE);
#if GTK_CHECK_VERSION(4, 0, 0)
    if (!gtk_init_check()) { g_test_dbus_down(daemon); return; }
#else
    if (!gtk_init_check(NULL, NULL)) { g_test_dbus_down(daemon); return; }
#endif
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    g_assert_no_error(error);
    g_autoptr(GVariant) owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", NM_DBUS_SERVICE, 0),
        G_VARIANT_TYPE("(u)"), 0, 2000, NULL, &error);
    g_assert_no_error(error);
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.NetworkManager.Settings'>"
        "<method name='GetConnectionByUuid'><arg type='s' direction='in'/><arg type='o' direction='out'/></method>"
        "</interface><interface name='org.freedesktop.NetworkManager.Settings.Connection'>"
        "<method name='GetSecrets'><arg type='s' direction='in'/><arg type='a{sa{sv}}' direction='out'/></method>"
        "</interface></node>", &error);
    g_assert_no_error(error);
    const GDBusInterfaceVTable vtable = { .method_call = mock_method };
    guint settings_id = g_dbus_connection_register_object(bus, NM_DBUS_PATH_SETTINGS, info->interfaces[0], &vtable, NULL, NULL, &error);
    g_assert_no_error(error);
    guint secrets_id = g_dbus_connection_register_object(bus, "/org/freedesktop/NetworkManager/Settings/1", info->interfaces[1], &vtable, NULL, NULL, &error);
    g_assert_no_error(error);
    
    for (guint attempt = 0; attempt < 2; attempt++) {
        secret_error = attempt == 1;
        g_autoptr(NMConnection) conn = nm_vpn_editor_plugin_import(plugin, SOURCE_DIR "/examples/profile.json", &error);
        g_assert_no_error(error);
        nm_connection_clear_secrets(conn);
        g_autoptr(NMVpnEditor) editor = nm_vpn_editor_plugin_get_editor(plugin, conn, &error);
        g_assert_no_error(error);
        GtkWidget *root = GTK_WIDGET(nm_vpn_editor_get_widget(editor));
        GtkWidget *tcp = find_widget(root, "tcp-password");
        GtkWidget *udp = find_widget(root, "udp-password");
        g_assert_null(find_widget(root, "reload-passwords"));
        GtkWidget *status = find_widget(root, "editor-status");
        if (!secret_error) set_password_text(tcp, "user-edited-test");
        gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
        
        while ((secret_error
                ? !strstr(gtk_label_get_text(GTK_LABEL(status)), "Test authorization failure")
                : !*password_text(udp)) && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(NULL, FALSE);
            g_usleep(1000);
        }
        
        if (!secret_error) {
            g_assert_cmpstr(password_text(tcp), ==, "user-edited-test");
            g_assert_cmpstr(password_text(udp), ==, "stored-udp-test");
            g_assert_true(nm_vpn_editor_update_connection(editor, conn, &error));
            g_assert_no_error(error);
        } else {
            g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "Test authorization failure"));
            /* Existing profiles remain editable even when GetSecrets fails.
             * No secrets in the update means NM retains its stored values. */
            g_assert_true(nm_vpn_editor_update_connection(editor, conn, &error));
            g_assert_no_error(error);
            NMSettingVpn *vpn = nm_connection_get_setting_vpn(conn);
            g_assert_cmpuint(nm_setting_vpn_get_num_secrets(vpn), ==, 0);
            g_assert_null(strstr(nm_setting_vpn_get_data_item(vpn, "profile"), "__unchanged_secret__"));
            set_password_text(tcp, "replacement-tcp");
            g_assert_false(nm_vpn_editor_update_connection(editor, conn, &error));
            g_clear_error(&error);
            g_assert_cmpuint(nm_setting_vpn_get_num_secrets(nm_connection_get_setting_vpn(conn)), ==, 0);
            set_password_text(udp, "replacement-udp");
            g_assert_true(nm_vpn_editor_update_connection(editor, conn, &error));
            g_assert_no_error(error);
            g_assert_cmpstr(nm_setting_vpn_get_secret(nm_connection_get_setting_vpn(conn), "udp-password"), ==, "replacement-udp");
        }
    }
    
    g_assert_cmpuint(secret_calls, ==, 2);
    g_dbus_connection_unregister_object(bus, settings_id);
    g_dbus_connection_unregister_object(bus, secrets_id);
    g_dbus_connection_close_sync(bus, NULL, NULL);
    g_clear_object(&bus);
    g_test_dbus_down(daemon);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);
    GModule *module = g_module_open(EDITOR_PATH, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    g_assert_nonnull(module);
    NMVpnEditorPluginFactory factory = NULL;
    g_assert_true(g_module_symbol(module, "nm_vpn_editor_plugin_factory", (gpointer *) &factory));
    g_autoptr(GError) error = NULL;
    g_autofree char *host_locale = g_strdup(setlocale(LC_ALL, NULL));
    g_autofree char *host_domain = g_strdup(textdomain(NULL));
    plugin = factory(&error); g_assert_no_error(error); g_assert_nonnull(plugin);
    g_assert_cmpstr(setlocale(LC_ALL, NULL), ==, host_locale);
    g_assert_cmpstr(textdomain(NULL), ==, host_domain);
    bindtextdomain(GETTEXT_PACKAGE, TEST_LOCALEDIR);
    g_test_add_func("/editor/metadata", metadata); g_test_add_func("/editor/widget", ui);
    g_test_add_func("/editor/import-export", import_export);
    g_test_add_func("/editor/secrets-async", secrets_async);
    int status = g_test_run();
    g_object_unref(plugin);
    /* GTypes registered by a module remain process-global: keep it resident. */
    g_module_make_resident(module); g_module_close(module);
    return status;
}
