/*
 * SSH server for Unix: main program.
 *
 * ======================================================================
 *
 * This server is NOT SECURE!
 *
 * DO NOT DEPLOY IT IN A HOSTILE-FACING ENVIRONMENT!
 *
 * Its purpose is to speak the server end of everything PuTTY speaks
 * on the client side, so that I can test that I haven't broken PuTTY
 * when I reorganise its code, even things like RSA key exchange or
 * chained auth methods which it's hard to find a server that speaks
 * at all.
 *
 * It has no interaction with the OS's authentication system: the
 * authentications it will accept are configurable by command-line
 * option, and once you authenticate, it will run the connection
 * protocol - including all subprocesses and shells - under the same
 * Unix user id you started it under.
 *
 * It really is only suitable for testing the actual SSH protocol.
 * Don't use it for anything more serious!
 *
 * ======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#ifndef HAVE_NO_SYS_SELECT_H
#include <sys/select.h>
#endif

#define PUTTY_DO_GLOBALS	       /* actually _define_ globals */
#include "putty.h"
#include "ssh.h"
#include "sshserver.h"

const char *const appname = "uppity";

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
void nonfatal(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
}

char *platform_default_s(const char *name)
{
    return NULL;
}

int platform_default_i(const char *name, int def)
{
    return def;
}

FontSpec *platform_default_fontspec(const char *name)
{
    return fontspec_new("");
}

Filename *platform_default_filename(const char *name)
{
    return filename_from_str("");
}

char *x_get_default(const char *key)
{
    return NULL;		       /* this is a stub */
}

/*
 * Our selects are synchronous, so these functions are empty stubs.
 */
uxsel_id *uxsel_input_add(int fd, int rwx) { return NULL; }
void uxsel_input_remove(uxsel_id *id) { }

void old_keyfile_warning(void) { }

void timer_change_notify(unsigned long next)
{
}

char *platform_get_x_display(void) { return NULL; }

static int verbose;

static void log_to_stderr(const char *msg)
{
    /*
     * FIXME: in multi-connection proper-socket mode, prefix this with
     * a connection id of some kind. We'll surely pass this in to
     * sshserver.c by way of constructing a distinct LogPolicy per
     * instance and making its containing structure contain the id -
     * but we'll also have to arrange for those LogPolicy structs to
     * be freed when the server instance terminates.
     *
     * For now, though, we only have one server instance, so no need.
     */

    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static void server_eventlog(LogPolicy *lp, const char *event)
{
    if (verbose)
        log_to_stderr(event);
}

static void server_logging_error(LogPolicy *lp, const char *event)
{
    log_to_stderr(event);              /* unconditional */
}

static int server_askappend(
    LogPolicy *lp, Filename *filename,
    void (*callback)(void *ctx, int result), void *ctx)
{
    return 2; /* always overwrite (FIXME: could make this a cmdline option) */
}

static const LogPolicyVtable server_logpolicy_vt = {
    server_eventlog,
    server_askappend,
    server_logging_error,
};
LogPolicy server_logpolicy[1] = {{ &server_logpolicy_vt }};

struct AuthPolicy_ssh1_pubkey {
    struct RSAKey key;
    struct AuthPolicy_ssh1_pubkey *next;
};
struct AuthPolicy_ssh2_pubkey {
    ptrlen public_blob;
    struct AuthPolicy_ssh2_pubkey *next;
};

struct AuthPolicy {
    struct AuthPolicy_ssh1_pubkey *ssh1keys;
    struct AuthPolicy_ssh2_pubkey *ssh2keys;
};
unsigned auth_methods(AuthPolicy *ap)
{
    return AUTHMETHOD_PUBLICKEY | AUTHMETHOD_PASSWORD;
}
int auth_none(AuthPolicy *ap, ptrlen username)
{
    return FALSE;
}
int auth_password(AuthPolicy *ap, ptrlen username, ptrlen password)
{
    return ptrlen_eq_string(password, "weasel");
}
int auth_publickey(AuthPolicy *ap, ptrlen username, ptrlen public_blob)
{
    struct AuthPolicy_ssh2_pubkey *iter;
    for (iter = ap->ssh2keys; iter; iter = iter->next) {
        if (ptrlen_eq_ptrlen(public_blob, iter->public_blob))
            return TRUE;
    }
    return FALSE;
}
struct RSAKey *auth_publickey_ssh1(
    AuthPolicy *ap, ptrlen username, Bignum rsa_modulus)
{
    struct AuthPolicy_ssh1_pubkey *iter;
    for (iter = ap->ssh1keys; iter; iter = iter->next) {
        if (!bignum_cmp(rsa_modulus, iter->key.modulus))
            return &iter->key;
    }
    return NULL;
}
int auth_successful(AuthPolicy *ap, ptrlen username, unsigned method)
{
    return TRUE;
}

static void safety_warning(FILE *fp)
{
    fputs("  =================================================\n"
          "     THIS SSH SERVER IS NOT WRITTEN TO BE SECURE!\n"
          "  DO NOT DEPLOY IT IN A HOSTILE-FACING ENVIRONMENT!\n"
          "  =================================================\n", fp);
}

static void show_help(FILE *fp)
{
    safety_warning(fp);
    fputs("\n"
          "usage:   uppity [options]\n"
          "options: --hostkey KEY        SSH host key (need at least one)\n"
          "         --userkey KEY        public key"
           " acceptable for user authentication\n"
          "also:    uppity --help        show this text\n"
          "         uppity --version     show version information\n"
          "\n", fp);
    safety_warning(fp);
}

static void show_version_and_exit(void)
{
    char *buildinfo_text = buildinfo("\n");
    printf("%s: %s\n%s\n", appname, ver, buildinfo_text);
    sfree(buildinfo_text);
    exit(0);
}

const int buildinfo_gtk_relevant = FALSE;

static int finished = FALSE;
void server_instance_terminated(void)
{
    /* FIXME: change policy here if we're running in a listening loop */
    finished = TRUE;
}

static int longoptarg(const char *arg, const char *expected,
                      const char **val, int *argcp, char ***argvp)
{
    int len = strlen(expected);
    if (memcmp(arg, expected, len))
        return FALSE;
    if (arg[len] == '=') {
        *val = arg + len + 1;
        return TRUE;
    } else if (arg[len] == '\0') {
        if (--*argcp > 0) {
            *val = *++*argvp;
            return TRUE;
        } else {
            fprintf(stderr, "%s: option %s expects an argument\n",
                    appname, expected);
            exit(1);
        }
    }
    return FALSE;
}

extern const SftpServerVtable unix_live_sftpserver_vt;

int main(int argc, char **argv)
{
    int *fdlist;
    int fd;
    int i, fdcount, fdsize, fdstate;
    unsigned long now;

    ssh_key **hostkeys = NULL;
    int nhostkeys = 0, hostkeysize = 0;
    struct RSAKey *hostkey1 = NULL;

    AuthPolicy ap;

    Conf *conf = conf_new();
    load_open_settings(NULL, conf);

    ap.ssh1keys = NULL;
    ap.ssh2keys = NULL;

    if (argc <= 1) {
        /*
         * We're going to terminate with an error message below,
         * because there are no host keys. But we'll display the help
         * as additional standard-error output, if nothing else so
         * that people see the giant safety warning.
         */
        show_help(stderr);
        fputc('\n', stderr);
    }

    while (--argc > 0) {
        const char *arg = *++argv;
        const char *val;

        if (!strcmp(arg, "--help")) {
            show_help(stdout);
            exit(0);
        } else if (!strcmp(arg, "--version")) {
            show_version_and_exit();
        } else if (!strcmp(arg, "--verbose") || !strcmp(arg, "-v")) {
            verbose = TRUE;
        } else if (longoptarg(arg, "--hostkey", &val, &argc, &argv)) {
            Filename *keyfile;
            int keytype;
            const char *error;

            keyfile = filename_from_str(val);
            keytype = key_type(keyfile);

            if (keytype == SSH_KEYTYPE_SSH2) {
                struct ssh2_userkey *uk;
                ssh_key *key;
                uk = ssh2_load_userkey(keyfile, NULL, &error);
                filename_free(keyfile);
                if (!uk || !uk->key) {
                    fprintf(stderr, "%s: unable to load host key '%s': "
                            "%s\n", appname, val, error);
                    exit(1);
                }
                key = uk->key;
                sfree(uk->comment);
                sfree(uk);

                for (i = 0; i < nhostkeys; i++)
                    if (ssh_key_alg(hostkeys[i]) == ssh_key_alg(key)) {
                        fprintf(stderr, "%s: host key '%s' duplicates key "
                                "type %s\n", appname, val,
                                ssh_key_alg(key)->ssh_id);
                        exit(1);
                    }

                if (nhostkeys >= hostkeysize) {
                    hostkeysize = nhostkeys * 5 / 4 + 16;
                    hostkeys = sresize(hostkeys, hostkeysize, ssh_key *);
                }
                hostkeys[nhostkeys++] = key;
            } else if (keytype == SSH_KEYTYPE_SSH1) {
                if (hostkey1) {
                    fprintf(stderr, "%s: host key '%s' is a redundant "
                            "SSH-1 host key\n", appname, val);
                    exit(1);
                }
                hostkey1 = snew(struct RSAKey);
                if (!rsa_ssh1_loadkey(keyfile, hostkey1, NULL, &error)) {
                    fprintf(stderr, "%s: unable to load host key '%s': "
                            "%s\n", appname, val, error);
                    exit(1);
                }
            } else {
                fprintf(stderr, "%s: '%s' is not loadable as a "
                        "private key (%s)", appname, val,
                        key_type_to_str(keytype));
                exit(1);
            }
        } else if (longoptarg(arg, "--userkey", &val, &argc, &argv)) {
            Filename *keyfile;
            int keytype;
            const char *error;

            keyfile = filename_from_str(val);
            keytype = key_type(keyfile);

            if (keytype == SSH_KEYTYPE_SSH2_PUBLIC_RFC4716 ||
                keytype == SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH) {
                strbuf *sb = strbuf_new();
                struct AuthPolicy_ssh2_pubkey *node;
                void *blob;

                if (!ssh2_userkey_loadpub(keyfile, NULL, BinarySink_UPCAST(sb),
                                          NULL, &error)) {
                    fprintf(stderr, "%s: unable to load user key '%s': "
                            "%s\n", appname, val, error);
                    exit(1);
                }

                node = snew_plus(struct AuthPolicy_ssh2_pubkey, sb->len);
                blob = snew_plus_get_aux(node);
                memcpy(blob, sb->u, sb->len);
                node->public_blob = make_ptrlen(blob, sb->len);

                node->next = ap.ssh2keys;
                ap.ssh2keys = node;

                strbuf_free(sb);
            } else if (keytype == SSH_KEYTYPE_SSH1_PUBLIC) {
                strbuf *sb = strbuf_new();
                BinarySource src[1];
                struct AuthPolicy_ssh1_pubkey *node;

                if (!rsa_ssh1_loadpub(keyfile, BinarySink_UPCAST(sb),
                                      NULL, &error)) {
                    fprintf(stderr, "%s: unable to load user key '%s': "
                            "%s\n", appname, val, error);
                    exit(1);
                }

                node = snew(struct AuthPolicy_ssh1_pubkey);
                BinarySource_BARE_INIT(src, sb->u, sb->len);
                get_rsa_ssh1_pub(src, &node->key, RSA_SSH1_EXPONENT_FIRST);

                node->next = ap.ssh1keys;
                ap.ssh1keys = node;

                strbuf_free(sb);
            } else {
                fprintf(stderr, "%s: '%s' is not loadable as a public key "
                        "(%s)\n", appname, val, key_type_to_str(keytype));
                exit(1);
            }
        } else if (longoptarg(arg, "--sshlog", &val, &argc, &argv) ||
                   longoptarg(arg, "-sshlog", &val, &argc, &argv)) {
            Filename *logfile = filename_from_str(val);
            conf_set_filename(conf, CONF_logfilename, logfile);
            filename_free(logfile);
            conf_set_int(conf, CONF_logtype, LGTYP_PACKETS);
            conf_set_int(conf, CONF_logxfovr, LGXF_OVR);
        } else if (longoptarg(arg, "--sshrawlog", &val, &argc, &argv) ||
                   longoptarg(arg, "-sshrawlog", &val, &argc, &argv)) {
            Filename *logfile = filename_from_str(val);
            conf_set_filename(conf, CONF_logfilename, logfile);
            filename_free(logfile);
            conf_set_int(conf, CONF_logtype, LGTYP_SSHRAW);
            conf_set_int(conf, CONF_logxfovr, LGXF_OVR);
        } else {
            fprintf(stderr, "%s: unrecognised option '%s'\n", appname, arg);
            exit(1);
        }
    }

    if (nhostkeys == 0 && !hostkey1) {
        fprintf(stderr, "%s: specify at least one host key\n", appname);
        exit(1);
    }

    fdlist = NULL;
    fdcount = fdsize = 0;

    random_ref();

    /*
     * Block SIGPIPE, so that we'll get EPIPE individually on
     * particular network connections that go wrong.
     */
    putty_signal(SIGPIPE, SIG_IGN);

    sk_init();
    uxsel_init();

    {
        Plug *plug = ssh_server_plug(
            conf, hostkeys, nhostkeys, hostkey1, &ap, server_logpolicy,
            &unix_live_sftpserver_vt);
        ssh_server_start(plug, make_fd_socket(0, 1, -1, plug));
    }

    now = GETTICKCOUNT();

    while (!finished) {
	fd_set rset, wset, xset;
	int maxfd;
	int rwx;
	int ret;
        unsigned long next;

	FD_ZERO(&rset);
	FD_ZERO(&wset);
	FD_ZERO(&xset);
	maxfd = 0;

	/* Count the currently active fds. */
	i = 0;
	for (fd = first_fd(&fdstate, &rwx); fd >= 0;
	     fd = next_fd(&fdstate, &rwx)) i++;

	/* Expand the fdlist buffer if necessary. */
	if (i > fdsize) {
	    fdsize = i + 16;
	    fdlist = sresize(fdlist, fdsize, int);
	}

	/*
	 * Add all currently open fds to the select sets, and store
	 * them in fdlist as well.
	 */
	fdcount = 0;
	for (fd = first_fd(&fdstate, &rwx); fd >= 0;
	     fd = next_fd(&fdstate, &rwx)) {
	    fdlist[fdcount++] = fd;
	    if (rwx & 1)
		FD_SET_MAX(fd, maxfd, rset);
	    if (rwx & 2)
		FD_SET_MAX(fd, maxfd, wset);
	    if (rwx & 4)
		FD_SET_MAX(fd, maxfd, xset);
	}

        if (toplevel_callback_pending()) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            ret = select(maxfd, &rset, &wset, &xset, &tv);
        } else if (run_timers(now, &next)) {
            do {
                unsigned long then;
                long ticks;
                struct timeval tv;

		then = now;
		now = GETTICKCOUNT();
		if (now - then > next - then)
		    ticks = 0;
		else
		    ticks = next - now;
		tv.tv_sec = ticks / 1000;
		tv.tv_usec = ticks % 1000 * 1000;
                ret = select(maxfd, &rset, &wset, &xset, &tv);
                if (ret == 0)
                    now = next;
                else
                    now = GETTICKCOUNT();
            } while (ret < 0 && errno == EINTR);
        } else {
            ret = select(maxfd, &rset, &wset, &xset, NULL);
        }

        if (ret < 0 && errno == EINTR)
            continue;

	if (ret < 0) {
	    perror("select");
	    exit(1);
	}

	for (i = 0; i < fdcount; i++) {
	    fd = fdlist[i];
            /*
             * We must process exceptional notifications before
             * ordinary readability ones, or we may go straight
             * past the urgent marker.
             */
	    if (FD_ISSET(fd, &xset))
		select_result(fd, 4);
	    if (FD_ISSET(fd, &rset))
		select_result(fd, 1);
	    if (FD_ISSET(fd, &wset))
		select_result(fd, 2);
	}

        run_toplevel_callbacks();
    }
    exit(0);
}
