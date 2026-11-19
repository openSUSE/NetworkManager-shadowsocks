/*
 * service.c
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
#include <glib-unix.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>

typedef struct {
    NMVpnServicePlugin parent;
    SsRuntime *runtime;
    GCancellable *cancel;
    gboolean starting, wanted, quitting;
    GMainLoop *loop;
} SsService;
typedef struct {
    NMVpnServicePluginClass parent;
} SsServiceClass;

G_DEFINE_TYPE(SsService, ss_service, NM_TYPE_VPN_SERVICE_PLUGIN)

static void stop_runtime(SsService *self) {
    if (self->cancel)
        g_cancellable_cancel(self->cancel);
    if (!self->starting) {
        g_clear_pointer(&self->runtime, ss_runtime_free);
        g_clear_object(&self->cancel);
    }
}

static gboolean disconnect_vpn(NMVpnServicePlugin *plugin, GError **error) {
    (void)error;
    SsService *self = (SsService *)plugin;
    self->wanted = FALSE;
    stop_runtime(self);
    return TRUE;
}

static void child_exited(GObject *child, GAsyncResult *res, gpointer data) {
    SsService *self = data;
    g_subprocess_wait_finish(G_SUBPROCESS(child), res, NULL);
    if (self->runtime && self->wanted && ss_runtime_child(self->runtime) == G_SUBPROCESS(child)) {
        g_warning(_("Shadowsocks backend exited"));
        nm_vpn_service_plugin_failure(NM_VPN_SERVICE_PLUGIN(self),
                                      NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
        nm_vpn_service_plugin_disconnect(NM_VPN_SERVICE_PLUGIN(self), NULL);
    }
    g_object_unref(self);
}

static void publish(SsService *self) {
    SsProfile *p = ss_runtime_profile(self->runtime);
    g_autoptr(GVariant) general = ss_general_config(p, ss_runtime_gateway(self->runtime));
    nm_vpn_service_plugin_set_config(NM_VPN_SERVICE_PLUGIN(self), general);
    for (guint family = 4; family <= 6; family += 2) {
        if (!ss_enabled(p, family))
            continue;
        g_autoptr(GVariant) config_ip = ss_ip_config(p, family);
        if (family == 4)
            nm_vpn_service_plugin_set_ip4_config(NM_VPN_SERVICE_PLUGIN(self), config_ip);
        else
            nm_vpn_service_plugin_set_ip6_config(NM_VPN_SERVICE_PLUGIN(self), config_ip);
    }
}

static void start_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)source;
    g_autoptr(GError) error = NULL;
    if (!ss_runtime_start(task_data, cancel, &error))
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_boolean(task, TRUE);
}

static void start_done(GObject *source, GAsyncResult *res, gpointer unused) {
    (void)unused;
    SsService *self = (SsService *)source;
    g_autoptr(GError) error = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &error);
    self->starting = FALSE;
    if (!ok || !self->wanted) {
        stop_runtime(self);
        if (self->wanted) {
            g_warning(_("Startup failed: %s"), error ? error->message : _("Connection cancelled"));
            nm_vpn_service_plugin_failure(NM_VPN_SERVICE_PLUGIN(self),
                                          NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
            nm_vpn_service_plugin_disconnect(NM_VPN_SERVICE_PLUGIN(self), NULL);
        }
    } else {
        g_subprocess_wait_async(ss_runtime_child(self->runtime), NULL, child_exited,
                                g_object_ref(self));
        publish(self);
    }
    if (self->quitting)
        g_main_loop_quit(self->loop);
}

static gboolean connect_vpn(NMVpnServicePlugin *plugin, NMConnection *conn, GError **error) {
    SsService *self = (SsService *)plugin;
    if (self->runtime) {
        g_set_error_literal(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_ALREADY_STARTED,
                            _("A connection is already running or being cleaned up"));
        return FALSE;
    }
    SsProfile *profile = ss_profile_from_connection(conn, TRUE, error);
    if (!profile)
        return FALSE;
    self->runtime = ss_runtime_new(profile);
    self->cancel = g_cancellable_new();
    self->wanted = self->starting = TRUE;
    g_autoptr(GTask) task = g_task_new(self, self->cancel, start_done, NULL);
    g_task_set_task_data(task, self->runtime, NULL);
    g_task_run_in_thread(task, start_worker);
    return TRUE;
}

static gboolean need_secrets(NMVpnServicePlugin *plugin, NMConnection *conn, const char **setting,
                             GError **error) {
    (void)plugin;
    (void)error;
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(conn);
    if (!vpn || !nm_setting_vpn_get_secret(vpn, "tcp-password") ||
        !nm_setting_vpn_get_secret(vpn, "udp-password")) {
        *setting = NM_SETTING_VPN_SETTING_NAME;
        return TRUE;
    }
    return FALSE;
}

static void ss_service_init(SsService *self) {
    (void)self;
}
static void ss_service_class_init(SsServiceClass *klass) {
    NMVpnServicePluginClass *vpn = NM_VPN_SERVICE_PLUGIN_CLASS(klass);
    vpn->connect = connect_vpn;
    vpn->disconnect = disconnect_vpn;
    vpn->need_secrets = need_secrets;
}

static gboolean request_quit(gpointer data) {
    SsService *self = data;
    self->quitting = TRUE;
    self->wanted = FALSE;
    stop_runtime(self);
    if (!self->starting)
        g_main_loop_quit(self->loop);
    return G_SOURCE_REMOVE;
}

static void quit_signal(NMVpnServicePlugin *plugin, gpointer unused) {
    (void)unused;
    request_quit(plugin);
}

int main(int argc, char **argv) {
    ss_i18n_init();
    char *name = NULL;
    gboolean persist = FALSE, self_test = FALSE;
    GOptionEntry entries[] = {
        {"bus-name", 0, 0, G_OPTION_ARG_STRING, &name, _("D-Bus service name"), "NAME"},
        {"persist", 0, 0, G_OPTION_ARG_NONE, &persist, _("Keep service after disconnect"), NULL},
        {"self-test", 0, 0, G_OPTION_ARG_NONE, &self_test, _("Initialize D-Bus only, then exit"),
         NULL},
        {NULL}};
    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, entries, GETTEXT_PACKAGE);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        g_free(name);
        return 1;
    }
    SsService *self = g_initable_new(
        ss_service_get_type(), NULL, &error, NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME,
        name ? name : SS_SERVICE, NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !self_test, NULL);
    g_free(name);
    if (!self) {
        g_printerr(_("D-Bus initialization failed: %s\n"), error->message);
        return 1;
    }
    if (!self_test) {
        self->loop = g_main_loop_new(NULL, FALSE);
        if (!persist)
            g_signal_connect(self, "quit", G_CALLBACK(quit_signal), NULL);
        g_unix_signal_add(SIGTERM, request_quit, self);
        g_unix_signal_add(SIGINT, request_quit, self);
        g_main_loop_run(self->loop);
        stop_runtime(self);
        g_main_loop_unref(self->loop);
    }
    nm_vpn_service_plugin_shutdown(NM_VPN_SERVICE_PLUGIN(self));
    g_object_unref(self);
    return 0;
}
