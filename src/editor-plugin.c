/*
 * editor-plugin.c
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


#define _GNU_SOURCE
#include "ss.h"
#include <gmodule.h>
#include <dlfcn.h>

typedef struct { GObject parent; } SsPlugin;
typedef struct { GObjectClass parent; } SsPluginClass;
static void plugin_iface(NMVpnEditorPluginInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SsPlugin, ss_plugin, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(NM_TYPE_VPN_EDITOR_PLUGIN, plugin_iface))
static void plugin_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
    (void) object;
    if (id == 1) g_value_set_string(value, "Shadowsocks");
    else if (id == 2) g_value_set_string(value, _("Transparent proxy using shadowsocks-rust with separate TCP/UDP nodes"));
    else if (id == 3) g_value_set_string(value, SS_SERVICE);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void ss_plugin_init(SsPlugin *self) { (void) self; }
static void ss_plugin_class_init(SsPluginClass *klass) {
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->get_property = plugin_get_property;
    g_object_class_override_property(object, 1, NM_VPN_EDITOR_PLUGIN_NAME);
    g_object_class_override_property(object, 2, NM_VPN_EDITOR_PLUGIN_DESCRIPTION);
    g_object_class_override_property(object, 3, NM_VPN_EDITOR_PLUGIN_SERVICE);
}
static NMVpnEditor *plugin_editor(NMVpnEditorPlugin *plugin, NMConnection *conn, GError **error) {
    (void) plugin;
    typedef NMVpnEditor *(*EditorFactory)(NMConnection *, GError **);
    gpointer gtk3_symbol = NULL;
    GModule *host = g_module_open(NULL, 0);
    if (!host) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, g_module_error());
        return NULL;
    }
    g_module_symbol(host, "gtk_container_add", &gtk3_symbol);
    g_module_close(host);
    const char *name = gtk3_symbol ? "libnm-vpn-plugin-shadowsocks-editor.so"
                                   : "libnm-gtk4-vpn-plugin-shadowsocks-editor.so";
    Dl_info info;
    if (!dladdr((gpointer) plugin_editor, &info) || !info.dli_fname) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot locate the Shadowsocks editor module"));
        return NULL;
    }
    g_autofree char *dir = g_path_get_dirname(info.dli_fname);
    g_autofree char *path = g_build_filename(dir, name, NULL);
    GModule *module = g_module_open(path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (!module) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot load editor module: %s"), g_module_error());
        return NULL;
    }
    EditorFactory factory = NULL;
    if (!g_module_symbol(module, "ss_editor_factory", (gpointer *) &factory)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot load editor module: %s"), g_module_error());
        g_module_close(module);
        return NULL;
    }
    /* Registered GTypes and widget callbacks must outlive the editor instance. */
    g_module_make_resident(module);
    NMVpnEditor *editor = factory(conn, error);
    g_module_close(module);
    return editor;
}
static NMVpnEditorPluginCapability capabilities(NMVpnEditorPlugin *plugin) {
    (void) plugin;
    return NM_VPN_EDITOR_PLUGIN_CAPABILITY_IMPORT | NM_VPN_EDITOR_PLUGIN_CAPABILITY_EXPORT | NM_VPN_EDITOR_PLUGIN_CAPABILITY_IPV6;
}
static NMConnection *import_file(NMVpnEditorPlugin *plugin, const char *path, GError **error) {
    (void) plugin;
    g_autoptr(SsProfile) p = ss_profile_load(path, NULL);
    if (!p) {
        JsonObject *node = ss_import_node(path, 0, error);
        if (!node) return NULL;
        p = ss_profile_new();
        json_object_set_object_member(p->obj, "tcp", node);
        json_object_set_object_member(p->obj, "udp", json_object_ref(node));
    }
    return ss_connection_new(p, error);
}
static gboolean export_file(NMVpnEditorPlugin *plugin, const char *path, NMConnection *conn, GError **error) {
    (void) plugin;
    g_autoptr(SsProfile) p = ss_profile_from_connection(conn, TRUE, error);
    return p && ss_profile_export(p, path, error);
}
static char *suggested(NMVpnEditorPlugin *plugin, NMConnection *conn) {
    (void) plugin; (void) conn;
    return g_strdup("shadowsocks.json");
}
static void plugin_iface(NMVpnEditorPluginInterface *iface) {
    iface->get_editor = plugin_editor; iface->get_capabilities = capabilities;
    iface->import_from_file = import_file; iface->export_to_file = export_file;
    iface->get_suggested_filename = suggested;
}
G_MODULE_EXPORT NMVpnEditorPlugin *nm_vpn_editor_plugin_factory(GError **error) {
    (void) error;
    ss_i18n_bind();
    return g_object_new(ss_plugin_get_type(), NULL);
}
