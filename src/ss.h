/*
 * ss.h
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
#include <NetworkManager.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "config.h"
#include "i18n.h"

#define SS_SERVICE "org.freedesktop.NetworkManager.shadowsocks"
#define SS_ERROR G_IO_ERROR
#define SS_MAX_CONFIG (4 * 1024 * 1024)

typedef struct { JsonObject *obj; } SsProfile;
SsProfile *ss_profile_new(void);
SsProfile *ss_profile_parse(const char *text, GError **error);
SsProfile *ss_profile_load(const char *path, GError **error);
void ss_profile_free(SsProfile *p);
gboolean ss_profile_validate(SsProfile *p, GError **error);
char *ss_profile_json(SsProfile *p, gboolean secrets);
JsonObject *ss_import_node(const char *path, guint index, GError **error);
gboolean ss_profile_export(SsProfile *p, const char *path, GError **error);
JsonArray *ss_cidrs_load(const char *path, GError **error);
JsonObject *ss_node(SsProfile *p, guint protocol);
const char *ss_string(JsonObject *o, const char *key);
gboolean ss_enabled(SsProfile *p, guint family);
NMConnection *ss_connection_new(SsProfile *p, GError **error);
gboolean ss_connection_update(SsProfile *p, NMConnection *conn, GError **error);
SsProfile *ss_profile_from_connection(NMConnection *conn, gboolean require_secrets, GError **error);
char *ss_backend_json(SsProfile *p);
GVariant *ss_ip_config(SsProfile *p, guint family);
GVariant *ss_general_config(SsProfile *p, GInetAddress *gateway);
char *ss_plugin_resolve(const char *program, GError **error);
GPtrArray *ss_route_plan(SsProfile *p);
gboolean ss_routes_preflight(GError **error);
gboolean ss_route_tables_apply(GPtrArray *plan, GCancellable *cancel, GError **error);
gboolean ss_command(const char *command, char **output, GError **error);
char *ss_route_undo(const char *command);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SsProfile, ss_profile_free)

typedef struct _SsRuntime SsRuntime;
SsRuntime *ss_runtime_new(SsProfile *p);
gboolean ss_runtime_start(SsRuntime *r, GCancellable *cancel, GError **error);
void ss_runtime_free(SsRuntime *r);
GSubprocess *ss_runtime_child(SsRuntime *r);
SsProfile *ss_runtime_profile(SsRuntime *r);
GInetAddress *ss_runtime_gateway(SsRuntime *r);
