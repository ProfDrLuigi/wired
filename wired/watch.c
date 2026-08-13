/* $Id$ */

/*
 *  Copyright (c) 2003-2009 Axel Andersson
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wired/wired.h>

#include "chats.h"
#include "files.h"
#include "server.h"
#include "settings.h"
#include "watch.h"

#define WD_WATCH_INTERVAL               2.0
#define WD_WATCH_USER_ID                0
#define WD_WATCH_USER_COLOR             4
#define WD_WATCH_DEFAULT_MESSAGE        "New file available: $FILE ($SIZE)"
#define WD_WATCH_DEFAULT_NAME           "Wired Server"
#define WD_WATCH_DEFAULT_STATUS         "File notifications"
#define WD_WATCH_PIPE_BUFFER_SIZE       65536


static void                             wd_watch_scan(wi_timer_t *);
static void                             wd_watch_announce_path(wi_string_t *, wi_string_t *);
static wi_boolean_t                     wd_watch_path_is_descendant_of_path(wi_string_t *, wi_string_t *);
static int                              wd_watch_open_pipe(wi_string_t *);
static void                             wd_watch_drain_pipe(void);
static wi_string_t *                    wd_watch_normalize_pipe_markup(wi_string_t *);
static void                             wd_watch_broadcast_text(wi_string_t *);
static void                             wd_watch_broadcast_user_join(void);
static void                             wd_watch_broadcast_user_leave(void);
static void                             wd_watch_broadcast_user_status(void);
static void                             wd_watch_broadcast_user_icon(void);
static void                             wd_watch_set_user_fields(wi_p7_message_t *, wd_chat_t *);


static wi_lock_t                        *wd_watch_lock;
static wi_timer_t                       *wd_watch_timer;
static wi_string_t                      *wd_watch_path;
static wi_string_t                      *wd_watch_message;
static wi_string_t                      *wd_watch_name;
static wi_string_t                      *wd_watch_status;
static wi_data_t                        *wd_watch_icon;
static wi_mutable_dictionary_t          *wd_watch_known_files;
static wi_mutable_dictionary_t          *wd_watch_candidates;
static char                             wd_watch_pipe_buffer[WD_WATCH_PIPE_BUFFER_SIZE];
static wi_uinteger_t                    wd_watch_pipe_buffer_length;
static int                              wd_watch_pipe_fd;
static wi_boolean_t                     wd_watch_pipe_overflow;
static wi_boolean_t                     wd_watch_enabled;
static wi_boolean_t                     wd_watch_baseline_pending;



void wd_watch_initialize(void) {
    wd_watch_lock               = wi_lock_init(wi_lock_alloc());
    wd_watch_timer              = wi_timer_init_with_function(wi_timer_alloc(), wd_watch_scan, 0.0, true);
    wd_watch_known_files        = wi_dictionary_init(wi_mutable_dictionary_alloc());
    wd_watch_candidates         = wi_dictionary_init(wi_mutable_dictionary_alloc());
    wd_watch_pipe_fd            = -1;
}



void wd_watch_apply_settings(wi_set_t *changes) {
    wi_string_t     *configured_path, *configured_message, *configured_name;
    wi_string_t     *configured_status, *configured_icon, *configured_pipe, *realpath;
    wi_string_t     *name, *status;
    wi_data_t       *icon;
    wi_boolean_t    configured_enabled, directory, enabled, changed, was_enabled;
    wi_boolean_t    status_changed, icon_changed;
    int             pipe_fd, old_pipe_fd;

    configured_path     = wi_config_path_for_name(wd_config, WI_STR("watch path"));
    configured_message  = wi_config_string_for_name(wd_config, WI_STR("watch message"));
    configured_name     = wi_config_string_for_name(wd_config, WI_STR("watch name"));
    configured_status   = wi_config_string_for_name(wd_config, WI_STR("watch status"));
    configured_icon     = wi_config_path_for_name(wd_config, WI_STR("watch icon"));
    configured_pipe     = wi_config_path_for_name(wd_config, WI_STR("watch pipe"));
    configured_enabled  = wi_config_bool_for_name(wd_config, WI_STR("watch enabled"));
    realpath            = NULL;
    icon                = wi_data();
    enabled             = configured_enabled;
    pipe_fd             = -1;

    name = configured_name && wi_string_length(configured_name) > 0
        ? configured_name
        : WI_STR(WD_WATCH_DEFAULT_NAME);
    status = configured_status ? configured_status : WI_STR(WD_WATCH_DEFAULT_STATUS);

    if(configured_icon && wi_string_length(configured_icon) > 0) {
        icon = wi_data_with_contents_of_file(configured_icon);

        if(!icon) {
            wi_log_warn(WI_STR("Could not read watch icon \"%@\": %m"), configured_icon);
            icon = wi_data();
        }
    }

    if(configured_enabled && configured_path && wi_string_length(configured_path) > 0) {
        realpath = wi_fs_real_path_for_path(configured_path);

        if(!realpath || !wi_fs_path_exists(realpath, &directory) || !directory) {
            wi_log_warn(WI_STR("Could not watch \"%@\": path is not a directory"), configured_path);
            realpath = NULL;
        }
    }

    if(configured_enabled && configured_pipe && wi_string_length(configured_pipe) > 0)
        pipe_fd = wd_watch_open_pipe(configured_pipe);

    wi_lock_lock(wd_watch_lock);

    wi_release(wd_watch_message);
    wd_watch_message = wi_retain(configured_message && wi_string_length(configured_message) > 0
        ? configured_message
        : WI_STR(WD_WATCH_DEFAULT_MESSAGE));

    status_changed = !wd_watch_name || !wi_is_equal(wd_watch_name, name) ||
        !wd_watch_status || !wi_is_equal(wd_watch_status, status);
    icon_changed = !wd_watch_icon || !wi_is_equal(wd_watch_icon, icon);

    wi_release(wd_watch_name);
    wd_watch_name = wi_retain(name);

    wi_release(wd_watch_status);
    wd_watch_status = wi_retain(status);

    wi_release(wd_watch_icon);
    wd_watch_icon = wi_retain(icon);

    old_pipe_fd = wd_watch_pipe_fd;
    wd_watch_pipe_fd = pipe_fd;

    wd_watch_pipe_buffer_length = 0;
    wd_watch_pipe_overflow = false;

    was_enabled = wd_watch_enabled;
    changed     = (enabled != wd_watch_enabled);

    if(!changed && enabled) {
        if(wd_watch_path || realpath)
            changed = !wd_watch_path || !realpath || !wi_is_equal(wd_watch_path, realpath);
    }

    if(changed) {
        wi_release(wd_watch_path);
        wd_watch_path = enabled && realpath ? wi_retain(realpath) : NULL;
        wd_watch_enabled = enabled;
        wd_watch_baseline_pending = enabled && realpath;

        wi_mutable_dictionary_remove_all_data(wd_watch_known_files);
        wi_mutable_dictionary_remove_all_data(wd_watch_candidates);
    }

    wi_lock_unlock(wd_watch_lock);

    if(old_pipe_fd >= 0)
        close(old_pipe_fd);

    if(!was_enabled && enabled) {
        if(realpath)
            wi_log_info(WI_STR("Watching \"%@\" for new items"), realpath);

        wd_watch_broadcast_user_join();
    } else if(was_enabled && !enabled) {
        wi_log_info(WI_STR("Stopped watching for new items"));
        wd_watch_broadcast_user_leave();
    } else if(changed && enabled && realpath) {
        wi_log_info(WI_STR("Now watching \"%@\" for new items"), realpath);
    }

    if(pipe_fd >= 0)
        wi_log_info(WI_STR("Accepting chat messages from pipe \"%@\""), configured_pipe);

    if(was_enabled && enabled) {
        if(status_changed)
            wd_watch_broadcast_user_status();

        if(icon_changed)
            wd_watch_broadcast_user_icon();
    }
}



void wd_watch_schedule(void) {
    wi_boolean_t enabled;

    wi_lock_lock(wd_watch_lock);
    enabled = wd_watch_enabled;
    wi_lock_unlock(wd_watch_lock);

    if(enabled)
        wi_timer_reschedule(wd_watch_timer, WD_WATCH_INTERVAL);
    else
        wi_timer_invalidate(wd_watch_timer);
}



#pragma mark -

static void wd_watch_scan(wi_timer_t *timer) {
    wi_pool_t                   *pool;
    wi_fsenumerator_t           *fsenumerator;
    wi_fsenumerator_status_t    status;
    wi_mutable_dictionary_t     *current_inodes, *current_signatures, *current_directories;
    wi_mutable_array_t          *announcements, *unknown_directories, *new_directories;
    wi_array_t                  *keys;
    wi_enumerator_t             *enumerator;
    wi_string_t                 *path, *filepath, *key, *signature, *candidate, *directorypath;
    wi_mutable_string_t         *directory_signature;
    wi_number_t                 *inode, *known_inode;
    wi_fs_stat_t                sb;
    wi_uinteger_t               i, j, count, directory_count;
    wi_boolean_t                enabled, baseline, has_new_parent, suppress_announcement;

    pool = wi_pool_init(wi_pool_alloc());

    wi_lock_lock(wd_watch_lock);
    enabled = wd_watch_enabled;
    path = enabled ? wi_retain(wd_watch_path) : NULL;
    wi_lock_unlock(wd_watch_lock);

    if(!enabled) {
        wi_release(path);
        wi_release(pool);
        return;
    }

    wd_watch_drain_pipe();

    if(!path) {
        wi_release(pool);
        return;
    }

    current_inodes     = wi_mutable_dictionary();
    current_signatures = wi_mutable_dictionary();
    current_directories = wi_mutable_dictionary();
    announcements      = wi_mutable_array();
    unknown_directories = wi_mutable_array();
    new_directories    = wi_mutable_array();
    fsenumerator       = wi_fs_enumerator_at_path(path);

    if(!fsenumerator) {
        wi_log_error(WI_STR("Could not scan watched path \"%@\": %m"), path);
        wi_release(path);
        wi_release(pool);
        return;
    }

    while((status = wi_fsenumerator_get_next_path(fsenumerator, &filepath)) != WI_FSENUMERATOR_EOF) {
        if(status == WI_FSENUMERATOR_ERROR) {
            wi_log_error(WI_STR("Could not scan watched path \"%@\": %m"), filepath);
            continue;
        }

        if(wi_fs_path_is_invisible(filepath)) {
            wi_fsenumerator_skip_descendents(fsenumerator);
            continue;
        }

        if(!wi_fs_lstat_path(filepath, &sb))
            continue;

        if(!S_ISREG(sb.mode) && !S_ISDIR(sb.mode))
            continue;

        inode = wi_number_with_int64((int64_t) sb.ino);
        signature = wi_string_with_format(WI_STR("%c:%llu:%llu:%u"),
            S_ISDIR(sb.mode) ? 'd' : 'f', sb.ino, sb.size, sb.mtime);

        wi_mutable_dictionary_set_data_for_key(current_inodes, inode, filepath);

        if(S_ISDIR(sb.mode)) {
            directory_signature = wi_mutable_copy(signature);
            wi_mutable_dictionary_set_data_for_key(current_signatures, directory_signature, filepath);
            wi_mutable_dictionary_set_data_for_key(current_directories, wi_number_with_bool(true), filepath);
            wi_release(directory_signature);
        } else {
            wi_mutable_dictionary_set_data_for_key(current_signatures, signature, filepath);
        }

        /*
         * A directory is stable only when its complete subtree is stable. This
         * keeps a directory pending while files are still being copied into it.
         */
        enumerator = wi_dictionary_key_enumerator(current_directories);

        while((directorypath = wi_enumerator_next_data(enumerator))) {
            if(wd_watch_path_is_descendant_of_path(filepath, directorypath)) {
                directory_signature = wi_dictionary_data_for_key(current_signatures, directorypath);
                wi_mutable_string_append_format(directory_signature, WI_STR("|%@"), signature);
            }
        }
    }

    wi_lock_lock(wd_watch_lock);

    if(!wd_watch_enabled || !wd_watch_path || !wi_is_equal(wd_watch_path, path)) {
        wi_lock_unlock(wd_watch_lock);
        wi_release(path);
        wi_release(pool);
        return;
    }

    baseline = wd_watch_baseline_pending;

    if(baseline) {
        wi_mutable_dictionary_set_dictionary(wd_watch_known_files, current_inodes);
        wi_mutable_dictionary_remove_all_data(wd_watch_candidates);
        wd_watch_baseline_pending = false;

        wi_log_info(WI_STR("Initialized watched path with %u existing items"), wi_dictionary_count(current_inodes));
    } else {
        keys = wi_dictionary_all_keys(wd_watch_known_files);
        count = wi_array_count(keys);

        for(i = 0; i < count; i++) {
            key = WI_ARRAY(keys, i);

            if(!wi_dictionary_contains_key(current_inodes, key))
                wi_mutable_dictionary_remove_data_for_key(wd_watch_known_files, key);
        }

        keys = wi_dictionary_all_keys(wd_watch_candidates);
        count = wi_array_count(keys);

        for(i = 0; i < count; i++) {
            key = WI_ARRAY(keys, i);

            if(!wi_dictionary_contains_key(current_signatures, key))
                wi_mutable_dictionary_remove_data_for_key(wd_watch_candidates, key);
        }

        /*
         * Find the outermost newly-created directories. Their complete current
         * contents are adopted as known below so copying a populated directory
         * results in one announcement for the directory, not one per child.
         */
        enumerator = wi_dictionary_key_enumerator(current_directories);

        while((filepath = wi_enumerator_next_data(enumerator))) {
            inode = wi_dictionary_data_for_key(current_inodes, filepath);
            known_inode = wi_dictionary_data_for_key(wd_watch_known_files, filepath);

            if(!known_inode || !wi_is_equal(known_inode, inode))
                wi_mutable_array_add_data(unknown_directories, filepath);
        }

        directory_count = wi_array_count(unknown_directories);

        for(i = 0; i < directory_count; i++) {
            filepath = WI_ARRAY(unknown_directories, i);
            has_new_parent = false;

            for(j = 0; j < directory_count; j++) {
                directorypath = WI_ARRAY(unknown_directories, j);

                if(i != j && wd_watch_path_is_descendant_of_path(filepath, directorypath)) {
                    has_new_parent = true;
                    break;
                }
            }

            if(!has_new_parent)
                wi_mutable_array_add_data(new_directories, filepath);
        }

        enumerator = wi_dictionary_key_enumerator(current_inodes);

        while((filepath = wi_enumerator_next_data(enumerator))) {
            inode = wi_dictionary_data_for_key(current_inodes, filepath);
            known_inode = wi_dictionary_data_for_key(wd_watch_known_files, filepath);

            if(known_inode && wi_is_equal(known_inode, inode))
                continue;

            suppress_announcement = false;
            directory_count = wi_array_count(new_directories);

            for(i = 0; i < directory_count; i++) {
                directorypath = WI_ARRAY(new_directories, i);

                if(wd_watch_path_is_descendant_of_path(filepath, directorypath)) {
                    suppress_announcement = true;
                    break;
                }
            }

            if(suppress_announcement) {
                wi_mutable_dictionary_set_data_for_key(wd_watch_known_files, inode, filepath);
                wi_mutable_dictionary_remove_data_for_key(wd_watch_candidates, filepath);
                continue;
            }

            signature = wi_dictionary_data_for_key(current_signatures, filepath);
            candidate = wi_dictionary_data_for_key(wd_watch_candidates, filepath);

            if(candidate && wi_is_equal(candidate, signature)) {
                wi_mutable_dictionary_set_data_for_key(wd_watch_known_files, inode, filepath);
                wi_mutable_dictionary_remove_data_for_key(wd_watch_candidates, filepath);
                wi_mutable_array_add_data(announcements, filepath);
            } else {
                wi_mutable_dictionary_set_data_for_key(wd_watch_candidates, signature, filepath);
            }
        }
    }

    wi_lock_unlock(wd_watch_lock);

    count = wi_array_count(announcements);

    for(i = 0; i < count; i++)
        wd_watch_announce_path(WI_ARRAY(announcements, i), path);

    wi_release(path);
    wi_pool_drain(pool);
    wi_release(pool);
}



static int wd_watch_open_pipe(wi_string_t *path) {
    struct stat sb;
    const char  *cstring;
    int         fd;

    cstring = wi_string_cstring(path);

    if(lstat(cstring, &sb) < 0) {
        if(errno != ENOENT) {
            wi_log_warn(WI_STR("Could not inspect watch pipe \"%@\": %m"), path);
            return -1;
        }

        if(mkfifo(cstring, 0660) < 0) {
            wi_log_warn(WI_STR("Could not create watch pipe \"%@\": %m"), path);
            return -1;
        }

        if(chmod(cstring, 0660) < 0)
            wi_log_warn(WI_STR("Could not set permissions on watch pipe \"%@\": %m"), path);
    } else if(!S_ISFIFO(sb.st_mode)) {
        wi_log_warn(WI_STR("Could not use watch pipe \"%@\": path is not a named pipe"), path);
        return -1;
    }

    fd = open(cstring, O_RDONLY | O_NONBLOCK);

    if(fd < 0) {
        wi_log_warn(WI_STR("Could not open watch pipe \"%@\": %m"), path);
        return -1;
    }

    return fd;
}



static void wd_watch_drain_pipe(void) {
    wi_mutable_array_t  *messages;
    wi_enumerator_t     *enumerator;
    wi_string_t         *message, *formatted_message;
    char                buffer[4096];
    ssize_t             bytes;
    wi_uinteger_t       i, length;
    int                 error;
    wi_boolean_t        overflowed;

    messages = wi_mutable_array();
    error = 0;
    overflowed = false;

    wi_lock_lock(wd_watch_lock);

    while(wd_watch_pipe_fd >= 0) {
        bytes = read(wd_watch_pipe_fd, buffer, sizeof(buffer));

        if(bytes < 0) {
            if(errno == EINTR)
                continue;

            if(errno != EAGAIN && errno != EWOULDBLOCK)
                error = errno;

            break;
        }

        if(bytes == 0)
            break;

        for(i = 0; i < (wi_uinteger_t) bytes; i++) {
            if(buffer[i] == '\n') {
                if(wd_watch_pipe_overflow) {
                    wd_watch_pipe_overflow = false;
                    wd_watch_pipe_buffer_length = 0;
                    continue;
                }

                length = wd_watch_pipe_buffer_length;

                if(length > 0 && wd_watch_pipe_buffer[length - 1] == '\r')
                    length--;

                if(length > 0) {
                    message = wi_string_with_bytes(wd_watch_pipe_buffer, length);
                    wi_mutable_array_add_data(messages, message);
                }

                wd_watch_pipe_buffer_length = 0;
            } else if(!wd_watch_pipe_overflow) {
                if(wd_watch_pipe_buffer_length < WD_WATCH_PIPE_BUFFER_SIZE) {
                    wd_watch_pipe_buffer[wd_watch_pipe_buffer_length++] = buffer[i];
                } else {
                    wd_watch_pipe_buffer_length = 0;
                    wd_watch_pipe_overflow = true;
                    overflowed = true;
                }
            }
        }
    }

    wi_lock_unlock(wd_watch_lock);

    if(error != 0) {
        errno = error;
        wi_log_warn(WI_STR("Could not read watch pipe: %m"));
    }

    if(overflowed)
        wi_log_warn(WI_STR("Discarded watch pipe message larger than %u bytes"), WD_WATCH_PIPE_BUFFER_SIZE);

    enumerator = wi_array_data_enumerator(messages);

    while((message = wi_enumerator_next_data(enumerator))) {
        formatted_message = wd_watch_normalize_pipe_markup(message);
        wd_watch_broadcast_text(formatted_message);
        wi_log_info(WI_STR("Announced watch pipe message \"%@\""), formatted_message);
    }
}



static wi_string_t * wd_watch_normalize_pipe_markup(wi_string_t *message) {
    wi_mutable_string_t *formatted_message;

    formatted_message = wi_mutable_copy(message);
    wi_mutable_string_replace_string_with_string(formatted_message, WI_STR("<n>"), WI_STR("<span>"), 0);
    wi_mutable_string_replace_string_with_string(formatted_message, WI_STR("</n>"), WI_STR("</span>"), 0);
    wi_mutable_string_replace_string_with_string(formatted_message, WI_STR("<bold>"), WI_STR("<strong>"), 0);
    wi_mutable_string_replace_string_with_string(formatted_message, WI_STR("</bold>"), WI_STR("</strong>"), 0);

    return wi_autorelease(formatted_message);
}



static wi_boolean_t wd_watch_path_is_descendant_of_path(wi_string_t *path, wi_string_t *directory) {
    wi_string_t *prefix;

    prefix = wi_string_by_appending_string(directory, WI_STR("/"));

    return wi_string_has_prefix(path, prefix);
}



static void wd_watch_announce_path(wi_string_t *filepath, wi_string_t *root) {
    wi_mutable_string_t *text;
    wi_string_t     *relativepath, *size, *message_template;
    wi_fs_stat_t    sb;

    relativepath = wi_string_substring_from_index(filepath, wi_string_length(root) + 1);

    if(wi_fs_stat_path(filepath, &sb)) {
        if(S_ISDIR(sb.mode))
            size = WI_STR("folder");
        else
            size = wd_files_string_for_bytes(sb.size);
    } else {
        size = WI_STR("unknown size");
    }

    wi_lock_lock(wd_watch_lock);
    message_template = wi_retain(wd_watch_message ? wd_watch_message : WI_STR(WD_WATCH_DEFAULT_MESSAGE));
    wi_lock_unlock(wd_watch_lock);

    text = wi_mutable_copy(message_template);
    wi_mutable_string_replace_string_with_string(text, WI_STR("$SIZE"), size, 0);
    wi_mutable_string_replace_string_with_string(text, WI_STR("$FILE"), relativepath, 0);
    wi_release(message_template);

    wd_watch_broadcast_text(text);

    wi_log_info(WI_STR("Announced new item \"%@\" as \"%@\""), filepath, text);

    wi_release(text);
}



static void wd_watch_broadcast_text(wi_string_t *text) {
    wi_p7_message_t *message;

    message = wi_p7_message_with_name(WI_STR("wired.chat.say"), wd_p7_spec);
    wi_p7_message_set_uint32_for_name(message, wd_chat_id(wd_public_chat), WI_STR("wired.chat.id"));
    wi_p7_message_set_uint32_for_name(message, WD_WATCH_USER_ID, WI_STR("wired.user.id"));
    wi_p7_message_set_string_for_name(message, text, WI_STR("wired.chat.say"));
    wd_chat_broadcast_message(wd_public_chat, message);
}



#pragma mark -

static void wd_watch_set_user_fields(wi_p7_message_t *message, wd_chat_t *chat) {
    wi_string_t *name, *status;
    wi_data_t   *icon;

    wi_lock_lock(wd_watch_lock);
    name = wi_retain(wd_watch_name ? wd_watch_name : WI_STR(WD_WATCH_DEFAULT_NAME));
    status = wi_retain(wd_watch_status ? wd_watch_status : WI_STR(WD_WATCH_DEFAULT_STATUS));
    icon = wi_retain(wd_watch_icon ? wd_watch_icon : wi_data());
    wi_lock_unlock(wd_watch_lock);

    wi_p7_message_set_uint32_for_name(message, wd_chat_id(chat), WI_STR("wired.chat.id"));
    wi_p7_message_set_uint32_for_name(message, WD_WATCH_USER_ID, WI_STR("wired.user.id"));
    wi_p7_message_set_bool_for_name(message, false, WI_STR("wired.user.idle"));
    wi_p7_message_set_string_for_name(message, name, WI_STR("wired.user.nick"));
    wi_p7_message_set_string_for_name(message, status, WI_STR("wired.user.status"));
    wi_p7_message_set_data_for_name(message, icon, WI_STR("wired.user.icon"));
    wi_p7_message_set_enum_for_name(message, WD_WATCH_USER_COLOR, WI_STR("wired.account.color"));

    wi_release(name);
    wi_release(status);
    wi_release(icon);
}



void wd_watch_reply_user_list(wd_chat_t *chat, wd_user_t *user, wi_p7_message_t *message) {
    wi_p7_message_t *reply;
    wi_boolean_t    enabled;

    if(chat != wd_public_chat)
        return;

    wi_lock_lock(wd_watch_lock);
    enabled = wd_watch_enabled;
    wi_lock_unlock(wd_watch_lock);

    if(!enabled)
        return;

    reply = wi_p7_message_with_name(WI_STR("wired.chat.user_list"), wd_p7_spec);
    wd_watch_set_user_fields(reply, chat);
    wd_user_reply_message(user, reply, message);
}



static void wd_watch_broadcast_user_join(void) {
    wi_p7_message_t *message;

    message = wi_p7_message_with_name(WI_STR("wired.chat.user_join"), wd_p7_spec);
    wd_watch_set_user_fields(message, wd_public_chat);
    wd_chat_broadcast_message(wd_public_chat, message);
}



static void wd_watch_broadcast_user_leave(void) {
    wi_p7_message_t *message;

    message = wi_p7_message_with_name(WI_STR("wired.chat.user_leave"), wd_p7_spec);
    wi_p7_message_set_uint32_for_name(message, wd_chat_id(wd_public_chat), WI_STR("wired.chat.id"));
    wi_p7_message_set_uint32_for_name(message, WD_WATCH_USER_ID, WI_STR("wired.user.id"));
    wd_chat_broadcast_message(wd_public_chat, message);
}



static void wd_watch_broadcast_user_status(void) {
    wi_p7_message_t *message;
    wi_string_t     *name, *status;

    wi_lock_lock(wd_watch_lock);
    name = wi_retain(wd_watch_name ? wd_watch_name : WI_STR(WD_WATCH_DEFAULT_NAME));
    status = wi_retain(wd_watch_status ? wd_watch_status : WI_STR(WD_WATCH_DEFAULT_STATUS));
    wi_lock_unlock(wd_watch_lock);

    message = wi_p7_message_with_name(WI_STR("wired.chat.user_status"), wd_p7_spec);
    wi_p7_message_set_uint32_for_name(message, wd_chat_id(wd_public_chat), WI_STR("wired.chat.id"));
    wi_p7_message_set_uint32_for_name(message, WD_WATCH_USER_ID, WI_STR("wired.user.id"));
    wi_p7_message_set_bool_for_name(message, false, WI_STR("wired.user.idle"));
    wi_p7_message_set_string_for_name(message, name, WI_STR("wired.user.nick"));
    wi_p7_message_set_string_for_name(message, status, WI_STR("wired.user.status"));
    wi_p7_message_set_enum_for_name(message, WD_WATCH_USER_COLOR, WI_STR("wired.account.color"));
    wd_chat_broadcast_message(wd_public_chat, message);

    wi_release(name);
    wi_release(status);
}



static void wd_watch_broadcast_user_icon(void) {
    wi_p7_message_t *message;
    wi_data_t       *icon;

    wi_lock_lock(wd_watch_lock);
    icon = wi_retain(wd_watch_icon ? wd_watch_icon : wi_data());
    wi_lock_unlock(wd_watch_lock);

    message = wi_p7_message_with_name(WI_STR("wired.chat.user_icon"), wd_p7_spec);
    wi_p7_message_set_uint32_for_name(message, wd_chat_id(wd_public_chat), WI_STR("wired.chat.id"));
    wi_p7_message_set_uint32_for_name(message, WD_WATCH_USER_ID, WI_STR("wired.user.id"));
    wi_p7_message_set_data_for_name(message, icon, WI_STR("wired.user.icon"));
    wd_chat_broadcast_message(wd_public_chat, message);

    wi_release(icon);
}
