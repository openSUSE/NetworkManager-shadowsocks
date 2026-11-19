/*
 * nm-config.c
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
#include <arpa/inet.h>
#include <string.h>

static guint32 ip4_number(const char *address) {
    struct in_addr ip;
    inet_pton(AF_INET, address, &ip);
    return ip.s_addr;
}

GVariant *ss_ip_config(SsProfile *p, guint family) {
    GVariantBuilder ip, dns, domains;
    g_variant_builder_init(&ip, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_init(&dns, family == 4 ? G_VARIANT_TYPE("au") : G_VARIANT_TYPE("aay"));
    g_variant_builder_init(&domains, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&domains, "s", "~.");
    if (family == 4) {
        g_variant_builder_add(&ip, "{sv}", "address",
                              g_variant_new_uint32(ip4_number("198.18.0.1")));
    } else {
        g_autoptr(GInetAddress) address = g_inet_address_new_from_string("fd53:5353::1");
        g_variant_builder_add(&ip, "{sv}", "address",
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                        g_inet_address_to_bytes(address), 16, 1));
    }
    g_variant_builder_add(&ip, "{sv}", "prefix", g_variant_new_uint32(family == 4 ? 30 : 126));
    g_variant_builder_add(&ip, "{sv}", "never-default", g_variant_new_boolean(TRUE));
    JsonArray *servers = json_object_get_array_member(p->obj, "dns");
    for (guint i = 0; i < json_array_get_length(servers); i++) {
        const char *text = json_array_get_string_element(servers, i);
        g_autoptr(GInetAddress) address = g_inet_address_new_from_string(text);
        if (g_inet_address_get_family(address) !=
            (family == 4 ? G_SOCKET_FAMILY_IPV4 : G_SOCKET_FAMILY_IPV6))
            continue;
        if (family == 4)
            g_variant_builder_add(&dns, "u", ip4_number(text));
        else
            g_variant_builder_add_value(
                &dns, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                g_inet_address_to_bytes(address), 16, 1));
    }
    g_variant_builder_add(&ip, "{sv}", "dns", g_variant_builder_end(&dns));
    g_variant_builder_add(&ip, "{sv}", "domains", g_variant_builder_end(&domains));
    return g_variant_ref_sink(g_variant_builder_end(&ip));
}

GVariant *ss_general_config(SsProfile *p, GInetAddress *gateway) {
    g_return_val_if_fail(G_IS_INET_ADDRESS(gateway), NULL);
    GVariantBuilder config;
    g_variant_builder_init(&config, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_TUNDEV,
                          g_variant_new_string("nmss-tun"));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP4,
                          g_variant_new_boolean(ss_enabled(p, 4)));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP6,
                          g_variant_new_boolean(ss_enabled(p, 6)));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_MTU, g_variant_new_uint32(1400));
    const guint8 *bytes = g_inet_address_to_bytes(gateway);
    GVariant *value;
    if (g_inet_address_get_family(gateway) == G_SOCKET_FAMILY_IPV4) {
        guint32 address;
        memcpy(&address, bytes, sizeof(address));
        value = g_variant_new_uint32(address);
    } else {
        value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, 16, 1);
    }
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, value);
    return g_variant_ref_sink(g_variant_builder_end(&config));
}
