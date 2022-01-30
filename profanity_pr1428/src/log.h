/*
 * log.h
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2012 - 2019 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#ifndef LOG_H
#define LOG_H

#include <glib.h>

#include "xmpp/message.h"

// log levels
typedef enum {
    PROF_LEVEL_DEBUG,
    PROF_LEVEL_INFO,
    PROF_LEVEL_WARN,
    PROF_LEVEL_ERROR
} log_level_t;

typedef enum {
    PROF_IN_LOG,
    PROF_OUT_LOG
} chat_log_direction_t;

void log_init(log_level_t filter, char* log_file);
log_level_t log_get_filter(void);
void log_close(void);
void log_reinit(void);
const char* get_log_file_location(void);
void log_debug(const char* const msg, ...);
void log_info(const char* const msg, ...);
void log_warning(const char* const msg, ...);
void log_error(const char* const msg, ...);
void log_msg(log_level_t level, const char* const area, const char* const msg);
log_level_t log_level_from_string(char* log_level);

void log_stderr_init(log_level_t level);
void log_stderr_close(void);
void log_stderr_handler(void);

void chat_log_init(void);

void chat_log_msg_out(const char* const barejid, const char* const msg, const char* resource);
void chat_log_otr_msg_out(const char* const barejid, const char* const msg, const char* resource);
void chat_log_pgp_msg_out(const char* const barejid, const char* const msg, const char* resource);
void chat_log_omemo_msg_out(const char* const barejid, const char* const msg, const char* resource);

void chat_log_msg_in(ProfMessage* message);
void chat_log_otr_msg_in(ProfMessage* message);
void chat_log_pgp_msg_in(ProfMessage* message);
void chat_log_omemo_msg_in(ProfMessage* message);

void chat_log_close(void);

void groupchat_log_init(void);

void groupchat_log_msg_out(const gchar* const room, const gchar* const msg);
void groupchat_log_msg_in(const gchar* const room, const gchar* const nick, const gchar* const msg);
void groupchat_log_omemo_msg_out(const gchar* const room, const gchar* const msg);
void groupchat_log_omemo_msg_in(const gchar* const room, const gchar* const nick, const gchar* const msg);

#endif
