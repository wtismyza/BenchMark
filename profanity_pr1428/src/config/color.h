/*
 * color.h
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2019 Aurelien Aptel <aurelien.aptel@gmail.com>
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

#ifndef _COLOR_H_
#define _COLOR_H_

/* to access color names */
#define COLOR_NAME_SIZE 256

#include <stdint.h>

typedef enum {
    COLOR_PROFILE_DEFAULT,
    COLOR_PROFILE_REDGREEN_BLINDNESS,
    COLOR_PROFILE_BLUE_BLINDNESS,
} color_profile;

struct color_def
{
    uint16_t h;
    uint8_t s, l;
    const char* name;
};
extern const struct color_def color_names[];

/* hash string to color pair */
int color_pair_cache_hash_str(const char* str, color_profile profile);
/* parse fg_bg string to color pair */
int color_pair_cache_get(const char* pair_name);
/* clear cache */
void color_pair_cache_reset(void);

#endif
