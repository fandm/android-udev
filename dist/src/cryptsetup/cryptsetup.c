/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <mntent.h>

#include <libcryptsetup.h>
#include <libudev.h>

#include "fileio.h"
#include "log.h"
#include "util.h"
#include "path-util.h"
#include "strv.h"
#include "ask-password-api.h"
#include "def.h"

static const char *opt_type = NULL; /* CRYPT_LUKS1, CRYPT_TCRYPT or CRYPT_PLAIN */
static char *opt_cipher = NULL;
static unsigned opt_key_size = 0;
static unsigned opt_keyfile_size = 0;
static unsigned opt_keyfile_offset = 0;
static char *opt_hash = NULL;
static unsigned opt_tries = 0;
static bool opt_readonly = false;
static bool opt_verify = false;
static bool opt_discards = false;
static bool opt_tcrypt_hidden = false;
static bool opt_tcrypt_system = false;
static char **opt_tcrypt_keyfiles = NULL;
static usec_t opt_timeout = 0;

/* Options Debian's crypttab knows we don't:

    offset=
    skip=
    precheck=
    check=
    checkargs=
    noearly=
    loud=
    keyscript=
*/

static int parse_one_option(const char *option) {
        assert(option);

        /* Handled outside of this tool */
        if (streq(option, "noauto") || streq(option, "nofail"))
                return 0;

        if (startswith(option, "cipher=")) {
                char *t;

                t = strdup(option+7);
                if (!t)
                        return -ENOMEM;

                free(opt_cipher);
                opt_cipher = t;

        } else if (startswith(option, "size=")) {

                if (safe_atou(option+5, &opt_key_size) < 0) {
                        log_error("size= parse failure, ignoring.");
                        return 0;
                }

        } else if (startswith(option, "tcrypt-keyfile=")) {

                opt_type = CRYPT_TCRYPT;
                if (path_is_absolute(option+15))
                        opt_tcrypt_keyfiles = strv_append(opt_tcrypt_keyfiles, strdup(option+15));
                else
                        log_error("Key file path '%s' is not absolute. Ignoring.", option+15);

        } else if (startswith(option, "keyfile-size=")) {

                if (safe_atou(option+13, &opt_keyfile_size) < 0) {
                        log_error("keyfile-size= parse failure, ignoring.");
                        return 0;
                }

        } else if (startswith(option, "keyfile-offset=")) {

                if (safe_atou(option+15, &opt_keyfile_offset) < 0) {
                        log_error("keyfile-offset= parse failure, ignoring.");
                        return 0;
                }

        } else if (startswith(option, "hash=")) {
                char *t;

                t = strdup(option+5);
                if (!t)
                        return -ENOMEM;

                free(opt_hash);
                opt_hash = t;

        } else if (startswith(option, "tries=")) {

                if (safe_atou(option+6, &opt_tries) < 0) {
                        log_error("tries= parse failure, ignoring.");
                        return 0;
                }

        } else if (streq(option, "readonly") || streq(option, "read-only"))
                opt_readonly = true;
        else if (streq(option, "verify"))
                opt_verify = true;
        else if (streq(option, "allow-discards"))
                opt_discards = true;
        else if (streq(option, "luks"))
                opt_type = CRYPT_LUKS1;
        else if (streq(option, "tcrypt"))
                opt_type = CRYPT_TCRYPT;
        else if (streq(option, "tcrypt-hidden")) {
                opt_type = CRYPT_TCRYPT;
                opt_tcrypt_hidden = true;
        } else if (streq(option, "tcrypt-system")) {
                opt_type = CRYPT_TCRYPT;
                opt_tcrypt_system = true;
        } else if (streq(option, "plain") ||
                 streq(option, "swap") ||
                 streq(option, "tmp"))
                opt_type = CRYPT_PLAIN;
        else if (startswith(option, "timeout=")) {

                if (parse_sec(option+8, &opt_timeout) < 0) {
                        log_error("timeout= parse failure, ignoring.");
                        return 0;
                }

        } else if (!streq(option, "none"))
                log_error("Encountered unknown /etc/crypttab option '%s', ignoring.", option);

        return 0;
}

static int parse_options(const char *options) {
        char *state, *w;
        size_t l;
        int r;

        assert(options);

        FOREACH_WORD_SEPARATOR(w, l, options, ",", state) {
                _cleanup_free_ char *o;

                o = strndup(w, l);
                if (!o)
                        return -ENOMEM;
                r = parse_one_option(o);
                if (r < 0)
                        return r;
        }

        return 0;
}

static void log_glue(int level, const char *msg, void *usrptr) {
        log_debug("%s", msg);
}

static char *disk_description(const char *path) {

        static const char name_fields[] = {
                "ID_PART_ENTRY_NAME\0"
                "DM_NAME\0"
                "ID_MODEL_FROM_DATABASE\0"
                "ID_MODEL\0"
        };

        struct udev *udev = NULL;
        struct udev_device *device = NULL;
        struct stat st;
        char *description = NULL;
        const char *i;

        assert(path);

        if (stat(path, &st) < 0)
                return NULL;

        if (!S_ISBLK(st.st_mode))
                return NULL;

        udev = udev_new();
        if (!udev)
                return NULL;

        device = udev_device_new_from_devnum(udev, 'b', st.st_rdev);
        if (!device)
                goto finish;

        NULSTR_FOREACH(i, name_fields) {
                const char *name;

                name = udev_device_get_property_value(device, i);
                if (!isempty(name)) {
                        description = strdup(name);
                        break;
                }
        }

finish:
        if (device)
                udev_device_unref(device);

        if (udev)
                udev_unref(udev);

        return description;
}

static char *disk_mount_point(const char *label) {
        char *mp = NULL;
        _cleanup_free_ char *device = NULL;
        FILE *f = NULL;
        struct mntent *m;

        /* Yeah, we don't support native systemd unit files here for now */

        if (asprintf(&device, "/dev/mapper/%s", label) < 0)
                goto finish;

        f = setmntent("/etc/fstab", "r");
        if (!f)
                goto finish;

        while ((m = getmntent(f)))
                if (path_equal(m->mnt_fsname, device)) {
                        mp = strdup(m->mnt_dir);
                        break;
                }

finish:
        if (f)
                endmntent(f);

        return mp;
}

static int get_password(const char *name, usec_t until, bool accept_cached, char ***passwords) {
        int r;
        char **p;
        _cleanup_free_ char *text = NULL;

        assert(name);
        assert(passwords);

        if (asprintf(&text, "Please enter passphrase for disk %s!", name) < 0)
                return log_oom();

        r = ask_password_auto(text, "drive-harddisk", until, accept_cached, passwords);
        if (r < 0) {
                log_error("Failed to query password: %s", strerror(-r));
                return r;
        }

        if (opt_verify) {
                _cleanup_strv_free_ char **passwords2 = NULL;

                assert(strv_length(*passwords) == 1);

                if (asprintf(&text, "Please enter passphrase for disk %s! (verification)", name) < 0)
                        return log_oom();

                r = ask_password_auto(text, "drive-harddisk", until, false, &passwords2);
                if (r < 0) {
                        log_error("Failed to query verification password: %s", strerror(-r));
                        return r;
                }

                assert(strv_length(passwords2) == 1);

                if (!streq(*passwords[0], passwords2[0])) {
                        log_warning("Passwords did not match, retrying.");
                        return -EAGAIN;
                }
        }

        strv_uniq(*passwords);

        STRV_FOREACH(p, *passwords) {
                char *c;

                if (strlen(*p)+1 >= opt_key_size)
                        continue;

                /* Pad password if necessary */
                if (!(c = new(char, opt_key_size)))
                        return log_oom();

                strncpy(c, *p, opt_key_size);
                free(*p);
                *p = c;
        }

        return 0;
}

static int attach_tcrypt(struct crypt_device *cd,
                                const char *name,
                                const char *key_file,
                                char **passwords,
                                uint32_t flags) {
        int r = 0;
        _cleanup_free_ char *passphrase = NULL;
        struct crypt_params_tcrypt params = {
                .flags = CRYPT_TCRYPT_LEGACY_MODES,
                .keyfiles = (const char **)opt_tcrypt_keyfiles,
                .keyfiles_count = strv_length(opt_tcrypt_keyfiles)
        };

        assert(cd);
        assert(name);
        assert(key_file || passwords);

        if (opt_tcrypt_hidden)
                params.flags |= CRYPT_TCRYPT_HIDDEN_HEADER;

        if (opt_tcrypt_system)
                params.flags |= CRYPT_TCRYPT_SYSTEM_HEADER;

        if (key_file) {
                r = read_one_line_file(key_file, &passphrase);
                if (r < 0) {
                        log_error("Failed to read password file '%s': %s", key_file, strerror(-r));
                        return -EAGAIN;
                }

                params.passphrase = passphrase;
        } else
                params.passphrase = passwords[0];
        params.passphrase_size = strlen(params.passphrase);

        r = crypt_load(cd, CRYPT_TCRYPT, &params);
        if (r < 0) {
                if (key_file && r == -EPERM) {
                        log_error("Failed to activate using password file '%s'.", key_file);
                        return -EAGAIN;
                }
                return r;
        }

        return crypt_activate_by_volume_key(cd, name, NULL, 0, flags);;
}

static int attach_luks_or_plain(struct crypt_device *cd,
                                const char *name,
                                const char *key_file,
                                char **passwords,
                                uint32_t flags) {
        int r = 0;
        bool pass_volume_key = false;

        assert(cd);
        assert(name);
        assert(key_file || passwords);

        if (!opt_type || streq(opt_type, CRYPT_LUKS1))
                r = crypt_load(cd, CRYPT_LUKS1, NULL);

        if ((!opt_type && r < 0) || streq_ptr(opt_type, CRYPT_PLAIN)) {
                struct crypt_params_plain params = {};
                const char *cipher, *cipher_mode;
                _cleanup_free_ char *truncated_cipher = NULL;

                if (opt_hash) {
                        /* plain isn't a real hash type. it just means "use no hash" */
                        if (!streq(opt_hash, "plain"))
                                params.hash = opt_hash;
                } else
                        params.hash = "ripemd160";

                if (opt_cipher) {
                        size_t l;

                        l = strcspn(opt_cipher, "-");
                        truncated_cipher = strndup(opt_cipher, l);
                        if (!truncated_cipher)
                                return log_oom();

                        cipher = truncated_cipher;
                        cipher_mode = opt_cipher[l] ? opt_cipher+l+1 : "plain";
                } else {
                        cipher = "aes";
                        cipher_mode = "cbc-essiv:sha256";
                }

                /* for CRYPT_PLAIN limit reads
                 * from keyfile to key length, and
                 * ignore keyfile-size */
                opt_keyfile_size = opt_key_size / 8;

                /* In contrast to what the name
                 * crypt_setup() might suggest this
                 * doesn't actually format anything,
                 * it just configures encryption
                 * parameters when used for plain
                 * mode. */
                r = crypt_format(cd, CRYPT_PLAIN, cipher, cipher_mode,
                                 NULL, NULL, opt_keyfile_size, &params);

                /* hash == NULL implies the user passed "plain" */
                pass_volume_key = (params.hash == NULL);
        }

        if (r < 0) {
                log_error("Loading of cryptographic parameters failed: %s", strerror(-r));
                return r;
        }

        log_info("Set cipher %s, mode %s, key size %i bits for device %s.",
                 crypt_get_cipher(cd),
                 crypt_get_cipher_mode(cd),
                 crypt_get_volume_key_size(cd)*8,
                 crypt_get_device_name(cd));

        if (key_file) {
                r = crypt_activate_by_keyfile_offset(cd, name, CRYPT_ANY_SLOT,
                                                     key_file, opt_keyfile_size,
                                                     opt_keyfile_offset, flags);
                if (r < 0) {
                        log_error("Failed to activate with key file '%s': %s", key_file, strerror(-r));
                        return -EAGAIN;
                }
        } else {
                char **p;

                STRV_FOREACH(p, passwords) {
                        if (pass_volume_key)
                                r = crypt_activate_by_volume_key(cd, name, *p, opt_key_size, flags);
                        else
                                r = crypt_activate_by_passphrase(cd, name, CRYPT_ANY_SLOT, *p, strlen(*p), flags);

                        if (r >= 0)
                                break;
                }
        }

        return r;
}

static int help(void) {

        printf("%s attach VOLUME SOURCEDEVICE [PASSWORD] [OPTIONS]\n"
               "%s detach VOLUME\n\n"
               "Attaches or detaches an encrypted block device.\n",
               program_invocation_short_name,
               program_invocation_short_name);

        return 0;
}

int main(int argc, char *argv[]) {
        int r = EXIT_FAILURE;
        struct crypt_device *cd = NULL;

        if (argc <= 1) {
                help();
                return EXIT_SUCCESS;
        }

        if (argc < 3) {
                log_error("This program requires at least two arguments.");
                return EXIT_FAILURE;
        }

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        if (streq(argv[1], "attach")) {
                uint32_t flags = 0;
                int k;
                unsigned tries;
                usec_t until;
                crypt_status_info status;
                const char *key_file = NULL, *name = NULL;
                _cleanup_free_ char *description = NULL, *name_buffer = NULL, *mount_point = NULL;

                /* Arguments: systemd-cryptsetup attach VOLUME SOURCE-DEVICE [PASSWORD] [OPTIONS] */

                if (argc < 4) {
                        log_error("attach requires at least two arguments.");
                        goto finish;
                }

                if (argc >= 5 &&
                    argv[4][0] &&
                    !streq(argv[4], "-") &&
                    !streq(argv[4], "none")) {

                        if (!path_is_absolute(argv[4]))
                                log_error("Password file path '%s' is not absolute. Ignoring.", argv[4]);
                        else
                                key_file = argv[4];
                }

                if (argc >= 6 && argv[5][0] && !streq(argv[5], "-")) {
                        if (parse_options(argv[5]) < 0)
                                goto finish;
                }

                /* A delicious drop of snake oil */
                mlockall(MCL_FUTURE);

                description = disk_description(argv[3]);
                mount_point = disk_mount_point(argv[2]);

                if (description && streq(argv[2], description)) {
                        /* If the description string is simply the
                         * volume name, then let's not show this
                         * twice */
                        free(description);
                        description = NULL;
                }

                if (mount_point && description)
                        asprintf(&name_buffer, "%s (%s) on %s", description, argv[2], mount_point);
                else if (mount_point)
                        asprintf(&name_buffer, "%s on %s", argv[2], mount_point);
                else if (description)
                        asprintf(&name_buffer, "%s (%s)", description, argv[2]);

                name = name_buffer ? name_buffer : argv[2];

                k = crypt_init(&cd, argv[3]);
                if (k) {
                        log_error("crypt_init() failed: %s", strerror(-k));
                        goto finish;
                }

                crypt_set_log_callback(cd, log_glue, NULL);

                status = crypt_status(cd, argv[2]);
                if (status == CRYPT_ACTIVE || status == CRYPT_BUSY) {
                        log_info("Volume %s already active.", argv[2]);
                        r = EXIT_SUCCESS;
                        goto finish;
                }

                if (opt_readonly)
                        flags |= CRYPT_ACTIVATE_READONLY;

                if (opt_discards)
                        flags |= CRYPT_ACTIVATE_ALLOW_DISCARDS;

                if (opt_timeout > 0)
                        until = now(CLOCK_MONOTONIC) + opt_timeout;
                else
                        until = 0;

                opt_tries = opt_tries > 0 ? opt_tries : 3;
                opt_key_size = (opt_key_size > 0 ? opt_key_size : 256);

                if (key_file) {
                        struct stat st;

                        /* Ideally we'd do this on the open fd, but since this is just a
                         * warning it's OK to do this in two steps. */
                        if (stat(key_file, &st) >= 0 && (st.st_mode & 0005))
                                log_warning("Key file %s is world-readable. This is not a good idea!", key_file);
                }

                for (tries = 0; tries < opt_tries; tries++) {
                        _cleanup_strv_free_ char **passwords = NULL;

                        if (!key_file) {
                                k = get_password(name, until, tries == 0 && !opt_verify, &passwords);
                                if (k == -EAGAIN)
                                        continue;
                                else if (k < 0)
                                        goto finish;
                        }

                        if (streq_ptr(opt_type, CRYPT_TCRYPT))
                                k = attach_tcrypt(cd, argv[2], key_file, passwords, flags);
                        else
                                k = attach_luks_or_plain(cd, argv[2], key_file, passwords, flags);
                        if (k >= 0)
                                break;
                        else if (k == -EAGAIN) {
                                key_file = NULL;
                                continue;
                        } else if (k != -EPERM) {
                                log_error("Failed to activate: %s", strerror(-k));
                                goto finish;
                        }

                        log_warning("Invalid passphrase.");
                }

                if (tries >= opt_tries) {
                        log_error("Too many attempts; giving up.");
                        r = EXIT_FAILURE;
                        goto finish;
                }

        } else if (streq(argv[1], "detach")) {
                int k;

                k = crypt_init_by_name(&cd, argv[2]);
                if (k) {
                        log_error("crypt_init() failed: %s", strerror(-k));
                        goto finish;
                }

                crypt_set_log_callback(cd, log_glue, NULL);

                k = crypt_deactivate(cd, argv[2]);
                if (k < 0) {
                        log_error("Failed to deactivate: %s", strerror(-k));
                        goto finish;
                }

        } else {
                log_error("Unknown verb %s.", argv[1]);
                goto finish;
        }

        r = EXIT_SUCCESS;

finish:

        if (cd)
                crypt_free(cd);

        free(opt_cipher);
        free(opt_hash);
        strv_free(opt_tcrypt_keyfiles);

        return r;
}
