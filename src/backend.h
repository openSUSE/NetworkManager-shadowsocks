#pragma once
#include <gio/gio.h>

/* Launch an executable accepting "-c CONFIG" as the configured service user.
 * CONFIG must be in a private root-owned runtime directory. */
GSubprocess *ss_backend_spawn(const char *executable, const char *config,
                             GError **error);
