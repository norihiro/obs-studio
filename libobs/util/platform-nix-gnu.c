/*
 * Copyright (c) 2022 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE // for dlinfo
#include <link.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

#include "obsconfig.h"
#include "base.h"

bool obs_plugin_check_qt_version(void *module, const char *name)
{
	if (!module)
		return false;

	struct link_map *list = NULL;
	if (dlinfo(module, RTLD_DI_LINKMAP, &list) == 0) {
		for (struct link_map *ptr = list; ptr; ptr = ptr->l_next) {
			char *slash = strrchr(ptr->l_name, '/');
			if (!slash)
				continue;
			char *base = slash + 1;
			if (strncmp(base, "libQt", 5) != 0)
				continue;
			int qtversion = atoi(base + 5);
			if (qtversion == 0 || qtversion == OBS_QT_VERSION)
				continue;

			blog(LOG_ERROR,
			     "module '%s' links wrong Qt library '%s',"
			     " expected Qt%d links Qt%d.",
			     name, ptr->l_name, OBS_QT_VERSION, qtversion);
			return false;
		}
	}

	return true;
}
