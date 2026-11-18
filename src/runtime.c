/*
 * runtime.c
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
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

struct _SsRuntime {
    SsProfile *profile;
    GSubprocess *child;
    GInetAddress *gateway;
    pid_t backend_group;
    char *directory;
    GPtrArray *plan;
    guint installed;
    guint route_count;
    gboolean tables_owned;
    int lock;
};

SsRuntime *ss_runtime_new(SsProfile *p) {
    SsRuntime *r = g_new0(SsRuntime, 1);
    r->profile = p;
    r->lock = -1;
    r->plan = ss_route_plan(p);
    return r;
}

SsProfile *ss_runtime_profile(SsRuntime *r) { return r->profile; }
GSubprocess *ss_runtime_child(SsRuntime *r) { return r->child; }
GInetAddress *ss_runtime_gateway(SsRuntime *r) { return r->gateway; }
static gboolean cancelled(GCancellable *c, GError **error) {
    return g_cancellable_set_error_if_cancelled(c, error);
}

static gboolean write_secret(const char *path, const char *text, GError **error) {
    /* GFile replace with PRIVATE gives 0600 and does not expose credentials in argv. */
    g_autoptr(GFile) file = g_file_new_for_path(path);
    return g_file_replace_contents(file, text, strlen(text), NULL, FALSE,
        G_FILE_CREATE_PRIVATE, NULL, NULL, error);
}

static void backend_session(gpointer data) {
    (void) data;
    if (setsid() < 0) _exit(127);
}

gboolean ss_runtime_start(SsRuntime *r, GCancellable *cancel, GError **error) {
    if (geteuid() != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, _("The VPN service must run as root")); return FALSE;
    }
    r->lock = open("/run/nm-shadowsocks.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (r->lock < 0 || fstat(r->lock, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        flock(r->lock, LOCK_EX | LOCK_NB) < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, _("Another Shadowsocks instance is running or the lock file is invalid")); return FALSE;
    }
    if (!ss_routes_preflight(error)) return FALSE;
    if (if_nametoindex("nmss-tun")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, _("The nmss-tun interface is already in use")); return FALSE;
    }
    r->directory = g_strdup("/run/nm-shadowsocks-XXXXXX");
    if (!g_mkdtemp_full(r->directory, 0700)) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), _("Cannot create runtime directory: %s"), g_strerror(errno));
        g_clear_pointer(&r->directory, g_free);
        return FALSE;
    }
    g_autoptr(GResolver) resolver = g_resolver_get_default();
    g_autofree char *profile_json = ss_profile_json(r->profile, TRUE);
    g_autoptr(SsProfile) routing = ss_profile_parse(profile_json, error);
    if (!routing) return FALSE;
    /* Bootstrap node DNS before proxy rules are installed. */
    for (guint i = 0; i < 2; i++) {
        if (cancelled(cancel, error)) return FALSE;
        JsonObject *node = ss_node(r->profile, i);
        const char *host = ss_string(node, "server");
        g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
        if (!address) {
            GList *answers = g_resolver_lookup_by_name(resolver, host, cancel, error);
            if (!answers) return FALSE;
            address = g_object_ref(answers->data);
            g_resolver_free_addresses(answers);
        }
        /* Report the actual TCP endpoint, not an unresolved hostname or TUN IP. */
        if (i == 0) g_set_object(&r->gateway, address);
        g_autofree char *server = g_inet_address_to_string(address);
        json_object_set_string_member(ss_node(routing, i), "server", server);
        if (*ss_string(node, "plugin")) {
            g_autofree char *executable = ss_plugin_resolve(ss_string(node, "plugin"), error);
            if (!executable) return FALSE;
            json_object_set_string_member(ss_node(routing, i), "plugin", executable);
            JsonArray *dns = json_object_get_array_member(r->profile->obj, "dns");
            for (guint j = 0; j < json_array_get_length(dns); j++) {
                g_autoptr(GInetAddress) upstream = g_inet_address_new_from_string(json_array_get_string_element(dns, j));
                if (g_inet_address_equal(address, upstream)) {
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        _("A plugin server must not also be an upstream DNS server"));
                    return FALSE;
                }
            }
        }
    }
    g_autofree char *config = ss_backend_json(routing);
    g_autofree char *path = g_build_filename(r->directory, "backend.json", NULL);
    if (!write_secret(path, config, error)) return FALSE;
    /* Keep backend diagnostics in the service journal. Silencing output hides
     * relay, plugin and DNS failures after NetworkManager reports connected. */
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_child_setup(launcher, backend_session, NULL, NULL);
    r->child = g_subprocess_launcher_spawn(launcher, error, SS_LOCAL, "-c", path, NULL);
    if (!r->child) return FALSE;
    r->backend_group = (pid_t) g_ascii_strtoll(g_subprocess_get_identifier(r->child), NULL, 10);
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!if_nametoindex("nmss-tun")) {
        if (cancelled(cancel, error)) return FALSE;
        if (g_get_monotonic_time() > deadline) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                _("TUN startup timed out: check sslocal local-tun support, cipher and node key"));
            return FALSE;
        }
        g_usleep(50000);
    }
    if (!ss_command(SS_IP " link set dev nmss-tun mtu 1400 up", NULL, error)) return FALSE;
    /* Ephemeral device only; loose RPF permits replies from the tunnel. */
    int fd = open("/proc/sys/net/ipv4/conf/nmss-tun/rp_filter", O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, "2\n", 2) != 2) {
        if (fd >= 0) close(fd);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot configure TUN rp_filter"));
        return FALSE;
    }
    close(fd);
    if (ss_enabled(r->profile, 6) &&
        !ss_command(SS_IP " -6 address add fd53:5353::1/126 dev nmss-tun nodad", NULL, error)) return FALSE;
    g_ptr_array_unref(r->plan);
    r->plan = ss_route_plan(routing);
    /* A mainland list can contain tens of thousands of routes. Feed routes in
     * one ip batch rather than spawning a process for every CIDR. No policy
     * rules point at these exclusively reserved tables until this succeeds. */
    while (r->route_count < r->plan->len) {
        const char *command = g_ptr_array_index(r->plan, r->route_count);
        if (strstr(command, " rule add ")) break;
        r->route_count++;
    }
    r->tables_owned = TRUE;
    if (!ss_route_tables_apply(r->plan, cancel, error)) return FALSE;
    r->installed = r->route_count;
    for (; r->installed < r->plan->len; r->installed++) {
        if (cancelled(cancel, error) || !ss_command(g_ptr_array_index(r->plan, r->installed), NULL, error)) return FALSE;
    }
    return !cancelled(cancel, error);
}

void ss_runtime_free(SsRuntime *r) {
    if (!r) return;
    while (r->installed > r->route_count) {
        g_autofree char *undo = ss_route_undo(g_ptr_array_index(r->plan, --r->installed));
        g_autoptr(GError) error = NULL;
        if (!ss_command(undo, NULL, &error)) g_warning(_("Route rollback failed: %s"), error->message);
    }
    if (r->tables_owned) for (guint family = 4; family <= 6; family += 2)
        for (guint table = 51820; table <= 51821; table++) {
            g_autofree char *flush = g_strdup_printf(SS_IP " -%u route flush table %u", family, table);
            g_autoptr(GError) error = NULL;
            if (!ss_command(flush, NULL, &error)) g_warning(_("Routing table cleanup failed: %s"), error->message);
        }
    if (r->child) {
        /* Kill SIP003 descendants as well as sslocal, even after its exit. */
        if (r->backend_group > 0) kill(-r->backend_group, SIGKILL);
        g_subprocess_force_exit(r->child);
        g_subprocess_wait(r->child, NULL, NULL);
        g_clear_object(&r->child);
    }
    if (r->directory) {
        g_autofree char *path = g_build_filename(r->directory, "backend.json", NULL);
        unlink(path);
    }
    if (r->directory) { rmdir(r->directory); g_free(r->directory); }
    if (r->lock >= 0) close(r->lock);
    g_ptr_array_unref(r->plan);
    g_clear_object(&r->gateway);
    ss_profile_free(r->profile);
    g_free(r);
}
