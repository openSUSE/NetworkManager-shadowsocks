/*
 * test-backend-live.c
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


/* Real shadowsocks-rust protocol selection, confined to a private netns. */
#include "ss.h"
#include <glib/gstdio.h>
#include <net/if.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

static GSocket *udp_echo_socket, *tcp_echo_socket;

static const guint8 query[] = {0x12, 0x34, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void read_exact(GInputStream *in, void *buf, gsize len) {
    gsize count;
    g_assert_true(g_input_stream_read_all(in, buf, len, &count, NULL, NULL));
    g_assert_cmpuint(count, ==, len);
}

static void send_exact(GOutputStream *out, const void *buf, gsize len) {
    g_assert_true(g_output_stream_write_all(out, buf, len, NULL, NULL, NULL));
}

static GSocketAddress *endpoint(guint port) {
    g_autoptr(GInetAddress) ip = g_inet_address_new_from_string("127.0.0.1");
    return g_inet_socket_address_new(ip, port);
}

static GSocketConnection *connect_socks(guint8 command, guint8 reply[10]) {
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_socket_client_set_enable_proxy(client, FALSE);
    g_socket_client_set_timeout(client, 5);
    g_autoptr(GSocketAddress) addr = endpoint(11080);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_assert_nonnull(conn);
    g_socket_set_timeout(g_socket_connection_get_socket(conn), 5);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    const guint8 hello[] = {5, 1, 0};
    send_exact(out, hello, sizeof(hello));
    guint8 response[2]; read_exact(in, response, sizeof(response));
    g_assert_cmpuint(response[0], ==, 5); g_assert_cmpuint(response[1], ==, 0);
    guint8 request[] = {5, command, 0, 1, 127, 0, 0, 1, 15353 >> 8, 15353 & 255};
    if (command == 3) memset(request + 4, 0, 6);
    send_exact(out, request, sizeof(request));
    read_exact(in, reply, 10);
    g_assert_cmpuint(reply[0], ==, 5); g_assert_cmpuint(reply[1], ==, 0);
    g_assert_cmpuint(reply[3], ==, 1);
    return conn;
}

static gpointer tcp_echo(gpointer unused) {
    (void) unused;
    g_autoptr(GSocket) socket = g_socket_accept(tcp_echo_socket, NULL, NULL);
    g_assert_nonnull(socket); g_socket_set_timeout(socket, 5);
    g_autoptr(GSocketConnection) conn = g_socket_connection_factory_create_connection(socket);
    guint8 packet[sizeof(query)];
    read_exact(g_io_stream_get_input_stream(G_IO_STREAM(conn)), packet, sizeof(packet));
    packet[2] |= 0x80;
    send_exact(g_io_stream_get_output_stream(G_IO_STREAM(conn)), packet, sizeof(packet));
    return NULL;
}

static gpointer udp_echo(gpointer unused) {
    (void) unused;
    guint8 packet[512];
    g_autoptr(GSocketAddress) peer = NULL;
    gssize n = g_socket_receive_from(udp_echo_socket, &peer, (char *) packet, sizeof(packet), NULL, NULL);
    g_assert_cmpint(n, ==, sizeof(query)); packet[2] |= 0x80;
    g_assert_cmpint(g_socket_send_to(udp_echo_socket, peer, (char *) packet, n, NULL, NULL), ==, n);
    return NULL;
}
static void protocol_selection(void) {
    GThread *tcp_thread = g_thread_new("tcp-target", tcp_echo, NULL);
    GThread *udp_thread = g_thread_new("udp-target", udp_echo, NULL);
    guint8 reply[10], packet[sizeof(query)];
    g_autoptr(GSocketConnection) tcp = connect_socks(1, reply);
    send_exact(g_io_stream_get_output_stream(G_IO_STREAM(tcp)), query, sizeof(query));
    read_exact(g_io_stream_get_input_stream(G_IO_STREAM(tcp)), packet, sizeof(packet));
    g_assert_cmpuint(packet[0], ==, query[0]); g_assert_true(packet[2] & 0x80);
    g_autoptr(GSocketConnection) control = connect_socks(3, reply);
    guint port = ((guint) reply[8] << 8) | reply[9];
    g_autoptr(GSocketAddress) address = endpoint(port);
    g_autoptr(GSocket) udp = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
    g_socket_set_timeout(udp, 5);
    guint8 datagram[10 + sizeof(query)] = {0, 0, 0, 1, 127, 0, 0, 1, 15353 >> 8, 15353 & 255};
    memcpy(datagram + 10, query, sizeof(query));
    g_assert_cmpint(g_socket_send_to(udp, address, (char *) datagram, sizeof(datagram), NULL, NULL), ==, sizeof(datagram));
    guint8 response[512];
    g_assert_cmpint(g_socket_receive(udp, (char *) response, sizeof(response), NULL, NULL), ==, sizeof(datagram));
    g_assert_cmpuint(response[10], ==, query[0]); g_assert_true(response[12] & 0x80);
    g_thread_join(tcp_thread); g_thread_join(udp_thread);
}

static void child_setup(gpointer drop_bind) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL)) _exit(127);
    if (GPOINTER_TO_INT(drop_bind) && prctl(PR_CAPBSET_DROP, CAP_NET_BIND_SERVICE, 0, 0, 0)) _exit(127);
}

static GSubprocess *spawn_config(const char *program, const char *path, gboolean drop_bind) {
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
    g_subprocess_launcher_set_child_setup(launcher, child_setup, GINT_TO_POINTER(drop_bind), NULL);
    g_autoptr(GError) error = NULL;
    GSubprocess *child = g_subprocess_launcher_spawn(launcher, &error, program, "-c", path, NULL);
    g_assert_no_error(error); g_assert_nonnull(child);
    return child;
}
static void stop(GSubprocess *child) {
    g_subprocess_force_exit(child); g_subprocess_wait(child, NULL, NULL); g_object_unref(child);
}

int main(int argc, char **argv) {
    g_autofree char *self = g_file_read_link("/proc/self/ns/net", NULL);
    const char *parent = g_getenv("SS_TEST_PARENT_NETNS");
    if (getuid() != 0 || !self || !parent || !strcmp(self, parent)) return 77;
    g_autofree char *server_program = g_find_program_in_path("ssserver");
    if (!server_program || !g_file_test(SS_LOCAL, G_FILE_TEST_IS_EXECUTABLE)) return 77;
    g_test_init(&argc, &argv, NULL);
    g_assert_true(ss_command(SS_IP " link set lo up", NULL, NULL));
    int sysctl_fd = open("/proc/sys/net/ipv4/ip_unprivileged_port_start", O_WRONLY);
    g_assert_cmpint(sysctl_fd, >=, 0); g_assert_cmpint(write(sysctl_fd, "1024\n", 5), ==, 5); close(sysctl_fd);
    g_autofree char *dir = g_dir_make_tmp("nmss-backend-test-XXXXXX", NULL);
    g_autoptr(SsProfile) profile = ss_profile_new();
    GSubprocess *servers[2];
    char *paths[4];
    for (guint i = 0; i < 2; i++) {
        JsonObject *node = json_object_new();
        json_object_set_string_member(node, "server", "127.0.0.1");
        json_object_set_int_member(node, "server_port", 18388 + i);
        json_object_set_string_member(node, "method", i ? "aes-256-gcm" : "aes-128-gcm");
        json_object_set_string_member(node, "password", i ? "udp-test-secret" : "tcp-test-secret");
        json_object_set_object_member(profile->obj, i ? "udp" : "tcp", node);
        json_object_set_string_member(node, "mode", i ? "udp_only" : "tcp_only");
        g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT); json_node_set_object(root, node);
        g_autofree char *text = json_to_string(root, FALSE);
        paths[i] = g_build_filename(dir, i ? "udp.json" : "tcp.json", NULL);
        g_assert_true(g_file_set_contents(paths[i], text, -1, NULL));
        servers[i] = spawn_config(server_program, paths[i], FALSE);
    }
    g_autofree char *backend = ss_backend_json(profile);
    paths[2] = g_build_filename(dir, "tun.json", NULL);
    g_assert_true(g_file_set_contents(paths[2], backend, -1, NULL));
    GSubprocess *tun = spawn_config(SS_LOCAL, paths[2], TRUE);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!if_nametoindex("nmss-tun") && g_get_monotonic_time() < deadline) g_usleep(10000);
    g_assert_cmpuint(if_nametoindex("nmss-tun"), >, 0);
    g_test_message("Real TUN starts without CAP_NET_BIND_SERVICE");
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, backend, -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonObject *local = json_array_get_object_element(json_object_get_array_member(root, "locals"), 0);
    json_object_remove_member(local, "tun_interface_name"); json_object_remove_member(local, "tun_interface_address");
    json_object_set_string_member(local, "protocol", "socks");
    json_object_set_string_member(local, "local_address", "127.0.0.1");
    json_object_set_int_member(local, "local_port", 11080);
    g_autofree char *socks_config = json_to_string(json_parser_get_root(parser), FALSE);
    paths[3] = g_build_filename(dir, "socks.json", NULL);
    g_assert_true(g_file_set_contents(paths[3], socks_config, -1, NULL));
    GSubprocess *socks = spawn_config(SS_LOCAL, paths[3], TRUE);
    g_usleep(300000);
    g_autoptr(GSocketAddress) target = endpoint(15353);
    udp_echo_socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
    tcp_echo_socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
    g_assert_true(g_socket_bind(udp_echo_socket, target, FALSE, NULL));
    g_assert_true(g_socket_bind(tcp_echo_socket, target, FALSE, NULL));
    g_assert_true(g_socket_listen(tcp_echo_socket, NULL));
    g_socket_set_timeout(udp_echo_socket, 5); g_socket_set_timeout(tcp_echo_socket, 5);
    g_test_add_func("/backend-live/separate-tcp-udp-nodes", protocol_selection);
    int result = g_test_run();
    stop(socks); stop(tun); stop(servers[0]); stop(servers[1]);
    g_object_unref(udp_echo_socket); g_object_unref(tcp_echo_socket);
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) { g_unlink(paths[i]); g_free(paths[i]); }
    g_rmdir(dir);
    return result;
}
