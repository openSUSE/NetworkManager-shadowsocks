#include "ss.h"
#include "backend.h"
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    gid_t gid;
    const char *config;
    const char *directory;
} BackendSetup;

static void backend_session(gpointer data) {
    const BackendSetup *setup = data;
    /* Only the forked child changes credentials. NetworkManager does not grant
     * CAP_CHOWN: a file owner may change its group to its own effective GID. */
    if (setsid() < 0 || setgid(setup->gid) < 0 ||
        chown(setup->config, (uid_t) -1, setup->gid) < 0 ||
        chmod(setup->config, 0640) < 0 ||
        chown(setup->directory, (uid_t) -1, setup->gid) < 0 ||
        chmod(setup->directory, 0710) < 0) {
        static const char message[] = "Cannot prepare backend configuration permissions\n";
        (void) write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(127);
    }
}

GSubprocess *ss_backend_spawn(const char *executable, const char *config,
                             GError **error) {
    struct passwd entry, *account = NULL;
    gsize size = 16384;
    g_autofree char *buffer = NULL;
    int result;
    do {
        buffer = g_realloc(buffer, size);
        result = getpwnam_r(SS_BACKEND_USER, &entry, buffer, size, &account);
        size *= 2;
    } while (result == ERANGE && size <= 1024 * 1024);
    if (result || !account || account->pw_uid == 0 || account->pw_gid == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
            _("Backend account '%s' must exist and have a non-root UID and GID"),
            SS_BACKEND_USER);
        return NULL;
    }
    /* Root retains ownership: the backend can read, but cannot replace its
     * configuration or add files to the runtime directory. */
    g_autofree char *directory = g_path_get_dirname(config);
    g_autofree char *uid = g_strdup_printf("%lu", (unsigned long) account->pw_uid);
    g_autofree char *gid = g_strdup_printf("%lu", (unsigned long) account->pw_gid);
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    BackendSetup setup = {account->pw_gid, config, directory};
    g_subprocess_launcher_set_child_setup(launcher, backend_session, &setup, NULL);
    /* TUN creation and SO_MARK need NET_ADMIN. Ambient capabilities survive
     * exec into sslocal and its SIP003 children; all other capabilities and
     * supplementary groups are removed. Leave the inherited bounding ceiling alone:
     * changing it requires CAP_SETPCAP, which NetworkManager does not grant.
     * no_new_privs prevents gaining file capabilities or setuid privileges.
     * Never retry an unsuccessful drop as root. */
    return g_subprocess_launcher_spawn(launcher, error, SS_SETPRIV,
        "--reuid", uid, "--regid", gid, "--clear-groups",
        "--inh-caps=-all,+net_admin",
        "--ambient-caps=-all,+net_admin", "--no-new-privs", "--reset-env",
        "--", executable, "-c", config, NULL);
}
