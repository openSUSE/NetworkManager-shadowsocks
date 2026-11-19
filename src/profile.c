/*
 * profile.c
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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

static gboolean fail(GError **error, const char *message) {
    g_set_error_literal(error, SS_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

const char *ss_string(JsonObject *o, const char *key) {
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
               ? json_node_get_string(n)
               : "";
}

static JsonObject *object_member(JsonObject *o, const char *key) {
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

static JsonArray *array_member(JsonObject *o, const char *key) {
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}

static char *serialize(JsonObject *o) {
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, o);
    return json_to_string(root, TRUE);
}

static JsonObject *copy_object(JsonObject *o) {
    /* json_node_copy shares containers. Parse a serialization for a deep copy. */
    g_autofree char *text = serialize(o);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL))
        return NULL;
    return json_node_dup_object(json_parser_get_root(parser));
}

static JsonObject *parse_object(const char *text, GError **error) {
    g_autoptr(JsonParser) parser = json_parser_new();
    if (strlen(text) > SS_MAX_CONFIG) {
        fail(error, _("Configuration must not exceed 4 MiB"));
        return NULL;
    }
    if (!json_parser_load_from_data(parser, text, -1, error))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        fail(error, _("Configuration must be a JSON object"));
        return NULL;
    }
    return json_node_dup_object(root);
}

static char *read_bounded(const char *path, GError **error) {
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
    if (!stream)
        return NULL;
    GString *text = g_string_new(NULL);
    char buf[8192];
    gssize n;
    while ((n = g_input_stream_read(G_INPUT_STREAM(stream), buf, sizeof(buf), NULL, error)) > 0) {
        if (text->len + n > SS_MAX_CONFIG) {
            fail(error, _("File must not exceed 4 MiB"));
            g_string_free(text, TRUE);
            return NULL;
        }
        g_string_append_len(text, buf, n);
    }
    if (n < 0 || memchr(text->str, '\0', text->len)) {
        if (n >= 0)
            fail(error, _("File contains NUL bytes"));
        g_string_free(text, TRUE);
        return NULL;
    }
    return g_string_free(text, FALSE);
}

SsProfile *ss_profile_new(void) {
    SsProfile *p = g_new0(SsProfile, 1);
    p->obj = json_object_new();
    json_object_set_string_member(p->obj, "name", "Shadowsocks");
    json_object_set_string_member(p->obj, "mode", "lan");
    json_object_set_string_member(p->obj, "ip_mode", "dual");
    JsonArray *dns = json_array_new();
    json_array_add_string_element(dns, "1.1.1.1");
    json_array_add_string_element(dns, "2606:4700:4700::1111");
    json_object_set_array_member(p->obj, "dns", dns);
    json_object_set_array_member(p->obj, "china", json_array_new());
    json_object_set_array_member(p->obj, "bypass", json_array_new());
    return p;
}

void ss_profile_free(SsProfile *p) {
    if (p) {
        json_object_unref(p->obj);
        g_free(p);
    }
}

JsonObject *ss_node(SsProfile *p, guint proto) {
    return object_member(p->obj, proto == 0 ? "tcp" : "udp");
}

static gboolean node_validate(JsonObject *o, GError **error) {
    if (!o)
        return fail(error, _("Missing TCP or UDP node"));
    const char *server = ss_string(o, "server");
    const char *method = ss_string(o, "method");
    const char *password = ss_string(o, "password");
    const char *methods[] = {"aes-128-gcm",
                             "aes-256-gcm",
                             "chacha20-ietf-poly1305",
                             "2022-blake3-aes-128-gcm",
                             "2022-blake3-aes-256-gcm",
                             "2022-blake3-chacha20-poly1305",
                             NULL};
    if (!*server || strlen(server) > 253)
        return fail(error, _("Server address must contain 1-253 characters"));
    g_autoptr(GInetAddress) ip = g_inet_address_new_from_string(server);
    if (!ip) {
        for (const char *c = server; *c; c++)
            if (!g_ascii_isalnum(*c) && *c != '-' && *c != '.')
                return fail(error, _("Server must be an IP address or an ASCII DNS hostname"));
        if (!g_ascii_isalnum(*server))
            return fail(error, _("Hostname must start with a letter or digit"));
    }
    JsonNode *port = json_object_get_member(o, "server_port");
    if (!port || json_node_get_value_type(port) != G_TYPE_INT64 || json_node_get_int(port) < 1 ||
        json_node_get_int(port) > 65535)
        return fail(error, _("Port must be an integer between 1 and 65535"));
    if (!g_strv_contains(methods, method))
        return fail(error, _("Unsupported cipher; use an AEAD node"));
    if (!*password || strlen(password) > 4096)
        return fail(error, _("Node password must not be empty or exceed 4096 bytes"));
    const char *strings[] = {"plugin", "plugin_opts", "plugin_mode", NULL};
    for (guint i = 0; strings[i]; i++) {
        JsonNode *value = json_object_get_member(o, strings[i]);
        if (value &&
            (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING))
            return fail(error, _("Plugin name, options and mode must be strings"));
    }
    const char *plugin = ss_string(o, "plugin");
    if (strlen(plugin) > 4096 || strlen(ss_string(o, "plugin_opts")) > 16384)
        return fail(error, _("Plugin name or options are too long"));
    for (const char *c = plugin; *c; c++)
        if (!g_ascii_isalnum(*c) && !strchr("/-_.+", *c))
            return fail(
                error,
                _("Plugin must be an executable name or an absolute path, without arguments"));
    if (*plugin && (plugin[0] == '-' || (!g_path_is_absolute(plugin) && strchr(plugin, '/'))))
        return fail(error,
                    _("Plugin must be an executable name or an absolute path, without arguments"));
    const char *modes[] = {"tcp_only", "udp_only", "tcp_and_udp", NULL};
    if (json_object_has_member(o, "plugin_mode") &&
        !g_strv_contains(modes, ss_string(o, "plugin_mode")))
        return fail(error, _("Invalid plugin mode"));
    JsonNode *args = json_object_get_member(o, "plugin_args");
    if (args) {
        if (!JSON_NODE_HOLDS_ARRAY(args) || json_array_get_length(json_node_get_array(args)) > 64)
            return fail(error, _("Plugin arguments must be a JSON array of at most 64 strings"));
        JsonArray *array = json_node_get_array(args);
        for (guint i = 0; i < json_array_get_length(array); i++) {
            JsonNode *arg = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_VALUE(arg) || json_node_get_value_type(arg) != G_TYPE_STRING ||
                strlen(json_node_get_string(arg)) > 4096)
                return fail(error, _("Plugin arguments must be strings of at most 4096 bytes"));
        }
    }
    if (!*plugin && (*ss_string(o, "plugin_opts") ||
                     (args && json_array_get_length(json_node_get_array(args)))))
        return fail(error,
                    _("Specify a plugin executable before setting plugin options or arguments"));
    return TRUE;
}

static JsonObject *clean_node(JsonObject *node) {
    JsonObject *clean = json_object_new();
    const char *keys[] = {"server",      "server_port", "method",      "password", "plugin",
                          "plugin_opts", "plugin_args", "plugin_mode", NULL};
    for (guint i = 0; keys[i]; i++)
        if (json_object_has_member(node, keys[i]))
            json_object_set_member(clean, keys[i],
                                   json_node_copy(json_object_get_member(node, keys[i])));
    return clean;
}

static gboolean cidrs_validate(JsonArray *array, GError **error) {
    if (!array || json_array_get_length(array) > 100000)
        return fail(error, _("Invalid CIDR list or more than 100000 entries"));
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonNode *n = json_array_get_element(array, i);
        if (json_node_get_value_type(n) != G_TYPE_STRING)
            return fail(error, _("CIDR entries must be strings"));
        const char *cidr = json_node_get_string(n);
        if (!strchr(cidr, '/'))
            return fail(error, _("CIDR entries must include a prefix length"));
        g_autoptr(GInetAddressMask) mask = g_inet_address_mask_new_from_string(cidr, error);
        if (!mask)
            return FALSE;
        if (g_inet_address_mask_get_length(mask) == 0)
            return fail(error, _("Bypass ranges must not include a /0 default route"));
    }
    return TRUE;
}

gboolean ss_enabled(SsProfile *p, guint family) {
    const char *mode = ss_string(p->obj, "ip_mode");
    return !strcmp(mode, "dual") || !strcmp(mode, family == 4 ? "ipv4" : "ipv6");
}

gboolean ss_profile_validate(SsProfile *p, GError **error) {
    if (!node_validate(ss_node(p, 0), error) || !node_validate(ss_node(p, 1), error))
        return FALSE;
    const char *name = ss_string(p->obj, "name");
    if (!*name || strlen(name) > 128)
        return fail(error, _("Connection name must contain 1-128 bytes"));
    const char *modes[] = {"global", "lan", "china", NULL};
    const char *ip_modes[] = {"ipv4", "ipv6", "dual", NULL};
    if (!g_strv_contains(modes, ss_string(p->obj, "mode")) ||
        !g_strv_contains(ip_modes, ss_string(p->obj, "ip_mode")))
        return fail(error, _("Invalid proxy mode or IP mode"));
    JsonArray *dns = array_member(p->obj, "dns");
    if (!dns || !json_array_get_length(dns) || json_array_get_length(dns) > 8)
        return fail(error, _("Specify 1-8 DNS IP addresses"));
    for (guint i = 0; i < json_array_get_length(dns); i++) {
        JsonNode *n = json_array_get_element(dns, i);
        if (json_node_get_value_type(n) != G_TYPE_STRING)
            return fail(error, _("DNS addresses must be IP address strings"));
        g_autoptr(GInetAddress) ip = g_inet_address_new_from_string(json_node_get_string(n));
        if (!ip || g_inet_address_get_is_loopback(ip) || g_inet_address_get_is_any(ip) ||
            g_inet_address_get_is_multicast(ip) || g_inet_address_get_is_link_local(ip))
            return fail(
                error, _("DNS must be a unicast IP reachable through the proxy, not a local stub"));
        g_autoptr(GInetAddressMask) reserved = g_inet_address_mask_new_from_string(
            g_inet_address_get_family(ip) == G_SOCKET_FAMILY_IPV4 ? "198.18.0.0/29"
                                                                  : "fd53:5353::/125",
            NULL);
        if (g_inet_address_mask_matches(reserved, ip))
            return fail(error, _("Upstream DNS must not use the internal TUN addresses"));
        if (!ss_enabled(p, g_inet_address_get_family(ip) == G_SOCKET_FAMILY_IPV4 ? 4 : 6))
            return fail(error, _("DNS address family does not match the IP mode"));
    }
    JsonArray *regional = array_member(p->obj, "china");
    if (!cidrs_validate(regional, error) || !cidrs_validate(array_member(p->obj, "bypass"), error))
        return FALSE;
    if (!strcmp(ss_string(p->obj, "mode"), "china")) {
        gboolean has4 = FALSE, has6 = FALSE;
        for (guint i = 0; i < json_array_get_length(regional); i++) {
            g_autoptr(GInetAddressMask) mask = g_inet_address_mask_new_from_string(
                json_array_get_string_element(regional, i), NULL);
            if (g_inet_address_mask_get_family(mask) == G_SOCKET_FAMILY_IPV4)
                has4 = TRUE;
            else
                has6 = TRUE;
        }
        if ((ss_enabled(p, 4) && !has4) || (ss_enabled(p, 6) && !has6))
            return fail(error, _("Regional CIDRs must cover each enabled address family"));
    }
    return TRUE;
}

SsProfile *ss_profile_parse(const char *text, GError **error) {
    JsonObject *o = parse_object(text, error);
    if (!o)
        return NULL;
    SsProfile *p = ss_profile_new();
    const char *keys[] = {"name", "tcp", "udp", "mode", "ip_mode", "dns", "china", "bypass", NULL};
    for (guint i = 0; keys[i]; i++)
        if (json_object_has_member(o, keys[i]))
            json_object_set_member(p->obj, keys[i],
                                   json_node_copy(json_object_get_member(o, keys[i])));
    json_object_unref(o);
    if (!json_object_has_member(p->obj, "udp") && ss_node(p, 0))
        json_object_set_object_member(p->obj, "udp", copy_object(ss_node(p, 0)));
    if (!ss_profile_use_default_regional(p, error) || !ss_profile_validate(p, error)) {
        ss_profile_free(p);
        return NULL;
    }
    for (guint i = 0; i < 2; i++) {
        JsonObject *clean = clean_node(ss_node(p, i));
        json_object_set_object_member(p->obj, i == 0 ? "tcp" : "udp", clean);
    }
    return p;
}

SsProfile *ss_profile_load(const char *path, GError **error) {
    g_autofree char *text = read_bounded(path, error);
    return text ? ss_profile_parse(text, error) : NULL;
}

char *ss_profile_json(SsProfile *p, gboolean secrets) {
    JsonObject *o = copy_object(p->obj);
    if (!secrets)
        for (guint i = 0; i < 2; i++) {
            JsonObject *n = object_member(o, i == 0 ? "tcp" : "udp");
            if (n) {
                json_object_remove_member(n, "password");
            }
        }
    char *text = serialize(o);
    json_object_unref(o);
    return text;
}

JsonObject *ss_import_node(const char *path, guint index, GError **error) {
    g_autofree char *text = read_bounded(path, error);
    if (!text)
        return NULL;
    JsonObject *root = parse_object(text, error);
    if (!root)
        return NULL;
    JsonObject *source = root;
    JsonArray *nodes = array_member(root, "servers");
    if (!nodes)
        nodes = array_member(root, "shadowsocks");
    if (nodes) {
        if (index >= json_array_get_length(nodes) ||
            !JSON_NODE_HOLDS_OBJECT(json_array_get_element(nodes, index))) {
            fail(error, _("Node index is out of range; indexes start at 0"));
            json_object_unref(root);
            return NULL;
        }
        source = json_array_get_object_element(nodes, index);
    } else if (index != 0) {
        fail(error, _("Single-node files only support index 0"));
        json_object_unref(root);
        return NULL;
    }
    JsonObject *result = copy_object(source);
    json_object_unref(root);
    if (!json_object_has_member(result, "server") && json_object_has_member(result, "address"))
        json_object_set_member(result, "server",
                               json_node_copy(json_object_get_member(result, "address")));
    if (!json_object_has_member(result, "server_port") && json_object_has_member(result, "port"))
        json_object_set_member(result, "server_port",
                               json_node_copy(json_object_get_member(result, "port")));
    if (!node_validate(result, error)) {
        json_object_unref(result);
        return NULL;
    }
    /* Only allow node fields into the stored profile. */
    JsonObject *clean = clean_node(result);
    json_object_unref(result);
    return clean;
}

JsonArray *ss_cidrs_load(const char *path, GError **error) {
    g_autofree char *text = read_bounded(path, error);
    if (!text)
        return NULL;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    JsonArray *array = json_array_new();
    for (guint i = 0; lines[i]; i++) {
        char *comment = strchr(lines[i], '#');
        if (comment)
            *comment = '\0';
        char *line = g_strstrip(lines[i]);
        if (*line)
            json_array_add_string_element(array, line);
    }
    if (!cidrs_validate(array, error)) {
        json_array_unref(array);
        return NULL;
    }
    return array;
}

gboolean ss_profile_export(SsProfile *p, const char *path, GError **error) {
    g_autofree char *text = ss_profile_json(p, TRUE);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    _("Cannot create configuration file: %s"), g_strerror(errno));
        return FALSE;
    }
    gsize written = 0, len = strlen(text);
    while (written < len) {
        ssize_t n = write(fd, text + written, len - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int saved = errno;
            close(fd);
            unlink(path);
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                        _("Cannot write configuration file"));
            return FALSE;
        }
        written += n;
    }

    if (close(fd) != 0) {
        fail(error, _("Cannot close configuration file"));
        return FALSE;
    }

    return TRUE;
}

gboolean ss_connection_update(SsProfile *p, NMConnection *conn, GError **error) {
    if (!ss_profile_validate(p, error))
        return FALSE;
    NMSettingVpn *vpn = NM_SETTING_VPN(nm_setting_vpn_new());
    g_object_set(vpn, NM_SETTING_VPN_SERVICE_TYPE, SS_SERVICE, NULL);
    g_autofree char *public = ss_profile_json(p, FALSE);
    nm_setting_vpn_add_data_item(vpn, "profile", public);
    for (guint i = 0; i < 2; i++) {
        const char *key = i == 0 ? "tcp-password" : "udp-password";
        nm_setting_vpn_add_secret(vpn, key, ss_string(ss_node(p, i), "password"));
        nm_setting_set_secret_flags(NM_SETTING(vpn), key, NM_SETTING_SECRET_FLAG_NONE, NULL);
    }
    nm_connection_add_setting(conn, NM_SETTING(vpn));
    return TRUE;
}

NMConnection *ss_connection_new(SsProfile *p, GError **error) {
    NMConnection *conn = nm_simple_connection_new();
    NMSetting *meta = nm_setting_connection_new();
    g_autofree char *uuid = nm_utils_uuid_generate();
    g_object_set(meta, NM_SETTING_CONNECTION_ID, ss_string(p->obj, "name"),
                 NM_SETTING_CONNECTION_UUID, uuid, NM_SETTING_CONNECTION_TYPE, "vpn",
                 NM_SETTING_CONNECTION_AUTOCONNECT, FALSE, NULL);
    nm_connection_add_setting(conn, meta);
    if (!ss_connection_update(p, conn, error)) {
        g_object_unref(conn);
        return NULL;
    }
    for (guint i = 0; i < 2; i++) {
        NMSetting *ip = i == 0 ? nm_setting_ip4_config_new() : nm_setting_ip6_config_new();
        g_object_set(ip, NM_SETTING_IP_CONFIG_METHOD, "auto", NM_SETTING_IP_CONFIG_NEVER_DEFAULT,
                     TRUE, NM_SETTING_IP_CONFIG_DNS_PRIORITY, -5353, NULL);
        nm_setting_ip_config_add_dns_search(NM_SETTING_IP_CONFIG(ip), "~.");
        nm_connection_add_setting(conn, ip);
    }
    if (!nm_connection_verify(conn, error)) {
        g_object_unref(conn);
        return NULL;
    }
    return conn;
}

SsProfile *ss_profile_from_connection(NMConnection *conn, gboolean require, GError **error) {
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(conn);
    if (!vpn || g_strcmp0(nm_setting_vpn_get_service_type(vpn), SS_SERVICE)) {
        fail(error, _("Not a Shadowsocks VPN connection"));
        return NULL;
    }
    const char *text = nm_setting_vpn_get_data_item(vpn, "profile");
    JsonObject *o = text ? parse_object(text, error) : NULL;
    if (!o) {
        if (!text)
            fail(error, _("Missing configuration"));
        return NULL;
    }
    for (guint i = 0; i < 2; i++) {
        JsonObject *node = object_member(o, i == 0 ? "tcp" : "udp");
        const char *pw = nm_setting_vpn_get_secret(vpn, i == 0 ? "tcp-password" : "udp-password");
        if (node)
            json_object_set_string_member(node, "password", pw ? pw : "");
        if (node) {
            const char *fields[] = {"plugin_opts", "plugin_args", NULL};
            for (guint j = 0; fields[j]; j++) {
                g_autofree char *key = g_strdup_printf("%s-%s", i == 0 ? "tcp" : "udp", fields[j]);
                const char *value = nm_setting_vpn_get_secret(vpn, key);
                /* Prefer configuration data; fall back to legacy secrets. */
                if (json_object_has_member(node, fields[j]) || !value)
                    continue;
                g_autoptr(JsonParser) parser = json_parser_new();
                if (!json_parser_load_from_data(parser, value, -1, error)) {
                    json_object_unref(o);
                    return NULL;
                }
                json_object_set_member(node, fields[j],
                                       json_node_copy(json_parser_get_root(parser)));
            }
        }
    }
    /* Editors may load a connection whose agent has not supplied secrets yet. */
    if (!require)
        for (guint i = 0; i < 2; i++) {
            JsonObject *n = object_member(o, i == 0 ? "tcp" : "udp");
            if (n && !*ss_string(n, "password"))
                json_object_set_string_member(n, "password", "__missing__");
        }
    g_autofree char *full = serialize(o);
    json_object_unref(o);
    SsProfile *p = ss_profile_parse(full, error);
    if (p && !require)
        for (guint i = 0; i < 2; i++) {
            const char *pw =
                nm_setting_vpn_get_secret(vpn, i == 0 ? "tcp-password" : "udp-password");
            if (!pw)
                json_object_set_string_member(ss_node(p, i), "password", "");
        }
    return p;
}

char *ss_backend_json(SsProfile *p) {
    JsonObject *o = json_object_new();
    JsonArray *servers = json_array_new();
    for (guint i = 0; i < 2; i++) {
        JsonObject *node = clean_node(ss_node(p, i));
        json_object_set_string_member(node, "mode", i == 0 ? "tcp_only" : "udp_only");
        json_array_add_object_element(servers, node);
    }
    json_object_set_array_member(o, "servers", servers);
    json_object_set_string_member(o, "mode", "tcp_and_udp");
    json_object_set_int_member(o, "outbound_fwmark", 0x5353);
    JsonArray *locals = json_array_new();
    JsonObject *local = json_object_new();
    json_object_set_string_member(local, "protocol", "tun");
    json_object_set_string_member(local, "tun_interface_name", "nmss-tun");
    json_object_set_string_member(local, "tun_interface_address", "198.18.0.1/30");
    json_object_set_string_member(local, "mode", "tcp_and_udp");
    json_array_add_object_element(locals, local);
    json_object_set_array_member(o, "locals", locals);
    char *text = serialize(o);
    json_object_unref(o);
    return text;
}
