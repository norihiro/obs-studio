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
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>

#include "obsconfig.h"
#include "base.h"
#include "bmem.h"

static bool check_elf(const char *name)
{
	FILE *tmp_file = NULL;
	tmp_file = fopen(name, "rb");

	fseek(tmp_file, 0, SEEK_END);
	size_t filesize = (size_t)ftell(tmp_file);
	fseek(tmp_file, 0, SEEK_SET);
	void *data = bmalloc(filesize);
	size_t bytes_read = fread(data, filesize, 1, tmp_file);
	fclose(tmp_file);

	ElfW(Ehdr) *ehdr = data;

	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
		blog(LOG_INFO, "wrong elf type '%s'", name);
		bfree(data);
		return false;
	}

	blog(LOG_INFO, "Ehdr for '%s'", name);
	blog(LOG_INFO, "e_entry=%d", (int)ehdr->e_entry);
	blog(LOG_INFO, "e_entry=%d", (int)ehdr->e_entry);
	blog(LOG_INFO, "e_phoff=%d", (int)ehdr->e_phoff);
	blog(LOG_INFO, "e_shoff=%d", (int)ehdr->e_shoff);
	blog(LOG_INFO, "e_flags=%d", (int)ehdr->e_flags);
	blog(LOG_INFO, "e_ehsize=%d", (int)ehdr->e_ehsize);
	blog(LOG_INFO, "e_phentsize=%d", (int)ehdr->e_phentsize);
	blog(LOG_INFO, "e_phnum=%d", (int)ehdr->e_phnum);
	blog(LOG_INFO, "e_shentsize=%d", (int)ehdr->e_shentsize);
	blog(LOG_INFO, "e_shnum=%d", (int)ehdr->e_shnum);
	blog(LOG_INFO, "e_shstrndx=%d", (int)ehdr->e_shstrndx);

	ElfW(Shdr) *shdr_ptr = data + ehdr->e_shoff;
	for (int i = 0; i < ehdr->e_shnum; i++) {
		blog(LOG_INFO, "shdr[%d].sh_name=%d", i, (int)shdr_ptr[i].sh_name);
		blog(LOG_INFO, "shdr[%d].sh_type=%#x", i, (int)shdr_ptr[i].sh_type);
		blog(LOG_INFO, "shdr[%d].sh_flags=%d", i, (int)shdr_ptr[i].sh_flags);
		blog(LOG_INFO, "shdr[%d].sh_addr=%#x", i, (int)shdr_ptr[i].sh_addr);
		blog(LOG_INFO, "shdr[%d].sh_offset=%#x", i, (int)shdr_ptr[i].sh_offset);
		blog(LOG_INFO, "shdr[%d].sh_size=%#x", i, (int)shdr_ptr[i].sh_size);
		blog(LOG_INFO, "shdr[%d].sh_link=%d", i, (int)shdr_ptr[i].sh_link);
		blog(LOG_INFO, "shdr[%d].sh_info=%d", i, (int)shdr_ptr[i].sh_info);
		blog(LOG_INFO, "shdr[%d].sh_addralign=%d", i, (int)shdr_ptr[i].sh_addralign);
		blog(LOG_INFO, "shdr[%d].sh_entsize=%d", i, (int)shdr_ptr[i].sh_entsize);
	}

	bfree(data);
	return true;
}

bool obs_plugin_check_qt_version(void *module, const char *name)
{
	if (!module)
		return false;

	bool ret = true;

	ret &= check_elf(name);

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
			ret = false;
		}
	}

	blog(LOG_INFO, "module '%s' qt_version_tag %p", name, dlsym(module, "qt_version_tag"));
	blog(LOG_INFO, "module '%s' qt_version_tag Qt5.15 %p", name, dlvsym(module, "qt_version_tag", "Qt5.15"));
	blog(LOG_INFO, "module '%s' qt_version_tag Qt_6 %p", name, dlvsym(module, "qt_version_tag", "Qt_6"));
	blog(LOG_INFO, "module '%s' qt_version_tag Qt_6.2 %p", name, dlvsym(module, "qt_version_tag", "Qt_6.2"));
	blog(LOG_INFO, "module '%s' qt_version_tag Qt_5.15 %p", name, dlvsym(module, "qt_version_tag", "Qt_5.15"));
	blog(LOG_INFO, "module '%s' qt_version_tag Qt_5 %p", name, dlvsym(module, "qt_version_tag", "Qt_5"));

	return ret;
}

#ifdef QT_VERSION_MAIN

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *name = argv[i];
		void *module = dlopen(name, RTLD_LAZY);

		bool ok = obs_plugin_check_qt_version(module, name);
	}
	return 0;
}

#endif // QT_VERSION_MAIN
