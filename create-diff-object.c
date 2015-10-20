/*
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2013-2014 Josh Poimboeuf <jpoimboe@redhat.com>
 * Copyright (C) 2015 Ross Lagerwall <ross.lagerwall@citrix.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Large amounts of this code have been copied from kpatch.
 * The insn directory is copied from kpatch, which was in turn copied
 * from the Linux kernel's arch/x86/lib.
 */

/*
 * This file contains the heart of the ELF object differencing engine.
 *
 * The tool takes two ELF objects from two versions of the same source
 * file; a "base" object and a "patched" object.  These object need to have
 * been compiled with the -ffunction-sections and -fdata-sections GCC options.
 *
 * The tool compares the objects at a section level to determine what
 * sections have changed.  Once a list of changed sections has been generated,
 * various rules are applied to determine any object local sections that
 * are dependencies of the changed section and also need to be included in
 * the output object.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <argp.h>
#include <error.h>
#include <unistd.h>
#include <gelf.h>

#include "list.h"
#include "lookup.h"
#include "asm/insn.h"

#define ERROR(format, ...) \
	error(1, 0, "ERROR: %s: %s: %d: " format, childobj, __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define DIFF_FATAL(format, ...) \
({ \
	fprintf(stderr, "ERROR: %s: " format "\n", childobj, ##__VA_ARGS__); \
	error(2, 0, "unreconcilable difference"); \
})

#define log_debug(format, ...) log(DEBUG, format, ##__VA_ARGS__)
#define log_normal(format, ...) log(NORMAL, "%s: " format, childobj, ##__VA_ARGS__)

#define log(level, format, ...) \
({ \
	if (loglevel <= (level)) \
		printf(format, ##__VA_ARGS__); \
})

char *childobj;

enum loglevel {
	DEBUG,
	NORMAL
};

static enum loglevel loglevel = NORMAL;

/*******************
 * Data structures
 * ****************/
struct section;
struct symbol;
struct rela;

enum status {
	NEW,
	CHANGED,
	SAME
};

struct section {
	struct list_head list;
	struct section *twin;
	GElf_Shdr sh;
	Elf_Data *data;
	char *name;
	int index;
	enum status status;
	int include;
	int ignore;
	int grouped;
	union {
		struct { /* if (is_rela_section()) */
			struct section *base;
			struct list_head relas;
		};
		struct { /* else */
			struct section *rela;
			struct symbol *secsym, *sym;
		};
	};
};

struct symbol {
	struct list_head list;
	struct symbol *twin;
	struct section *sec;
	GElf_Sym sym;
	char *name;
	int index;
	unsigned char bind, type;
	enum status status;
	union {
		int include; /* used in the patched elf */
		int strip; /* used in the output elf */
	};
};

struct rela {
	struct list_head list;
	GElf_Rela rela;
	struct symbol *sym;
	unsigned int type;
	int addend;
	int offset;
	char *string;
};

struct string {
	struct list_head list;
	char *name;
};

struct xsplice_elf {
	Elf *elf;
	struct list_head sections;
	struct list_head symbols;
	struct list_head strings;
	int fd;
};

#define PATCH_INSN_SIZE 5

struct xsplice_patch_func {
	unsigned long new_addr;
	unsigned long new_size;
	unsigned long old_addr;
	unsigned long old_size;
	char *name;
};

struct special_section {
	char *name;
	int (*group_size)(struct xsplice_elf *kelf, int offset);
};

/*******************
 * Helper functions
 ******************/

char *status_str(enum status status)
{
	switch(status) {
	case NEW:
		return "NEW";
	case CHANGED:
		return "CHANGED";
	case SAME:
		return "SAME";
	default:
		ERROR("status_str");
	}
	/* never reached */
	return NULL;
}

int is_rela_section(struct section *sec)
{
	return (sec->sh.sh_type == SHT_RELA);
}

int is_text_section(struct section *sec)
{
	return (sec->sh.sh_type == SHT_PROGBITS &&
		(sec->sh.sh_flags & SHF_EXECINSTR));
}

int is_debug_section(struct section *sec)
{
	char *name;
	if (is_rela_section(sec))
		name = sec->base->name;
	else
		name = sec->name;
	return !strncmp(name, ".debug_", 7);
}

struct section *find_section_by_index(struct list_head *list, unsigned int index)
{
	struct section *sec;

	list_for_each_entry(sec, list, list)
		if (sec->index == index)
			return sec;

	return NULL;
}

struct section *find_section_by_name(struct list_head *list, const char *name)
{
	struct section *sec;

	list_for_each_entry(sec, list, list)
		if (!strcmp(sec->name, name))
			return sec;

	return NULL;
}

struct symbol *find_symbol_by_index(struct list_head *list, size_t index)
{
	struct symbol *sym;

	list_for_each_entry(sym, list, list)
		if (sym->index == index)
			return sym;

	return NULL;
}

struct symbol *find_symbol_by_name(struct list_head *list, const char *name)
{
	struct symbol *sym;

	list_for_each_entry(sym, list, list)
		if (sym->name && !strcmp(sym->name, name))
			return sym;

	return NULL;
}

#define ALLOC_LINK(_new, _list) \
{ \
	(_new) = malloc(sizeof(*(_new))); \
	if (!(_new)) \
		ERROR("malloc"); \
	memset((_new), 0, sizeof(*(_new))); \
	INIT_LIST_HEAD(&(_new)->list); \
	list_add_tail(&(_new)->list, (_list)); \
}

/* returns the offset of the string in the string table */
int offset_of_string(struct list_head *list, char *name)
{
	struct string *string;
	int index = 0;

	/* try to find string in the string list */
	list_for_each_entry(string, list, list) {
		if (!strcmp(string->name, name))
			return index;
		index += strlen(string->name) + 1;
	}

	/* allocate a new string */
	ALLOC_LINK(string, list);
	string->name = name;
	return index;
}

void rela_insn(struct section *sec, struct rela *rela, struct insn *insn)
{
	unsigned long insn_addr, start, end, rela_addr;

	start = (unsigned long)sec->base->data->d_buf;
	end = start + sec->base->sh.sh_size;
	rela_addr = start + rela->offset;
	for (insn_addr = start; insn_addr < end; insn_addr += insn->length) {
		insn_init(insn, (void *)insn_addr, 1);
		insn_get_length(insn);
		if (!insn->length)
			ERROR("can't decode instruction in section %s at offset 0x%lx",
			      sec->base->name, insn_addr);
		if (rela_addr >= insn_addr &&
		    rela_addr < insn_addr + insn->length)
			return;
	}
}

/*************
 * Functions
 * **********/
void xsplice_create_rela_list(struct xsplice_elf *kelf, struct section *sec)
{
	int rela_nr, index = 0, skip = 0;
	struct rela *rela;
	unsigned int symndx;

	/* find matching base (text/data) section */
	sec->base = find_section_by_name(&kelf->sections, sec->name + 5);
	if (!sec->base)
		ERROR("can't find base section for rela section %s", sec->name);

	/* create reverse link from base section to this rela section */
	sec->base->rela = sec;
		
	rela_nr = sec->sh.sh_size / sec->sh.sh_entsize;

	log_debug("\n=== rela list for %s (%d entries) ===\n",
		sec->base->name, rela_nr);

	if (is_debug_section(sec)) {
		log_debug("skipping rela listing for .debug_* section\n");
		skip = 1;
	}

	/* read and store the rela entries */
	while (rela_nr--) {
		ALLOC_LINK(rela, &sec->relas);

		if (!gelf_getrela(sec->data, index, &rela->rela))
			ERROR("gelf_getrela");
		index++;

		rela->type = GELF_R_TYPE(rela->rela.r_info);
		rela->addend = rela->rela.r_addend;
		rela->offset = rela->rela.r_offset;
		symndx = GELF_R_SYM(rela->rela.r_info);
		rela->sym = find_symbol_by_index(&kelf->symbols, symndx);
		if (!rela->sym)
			ERROR("could not find rela entry symbol\n");
		if (rela->sym->sec &&
		    (rela->sym->sec->sh.sh_flags & SHF_STRINGS)) {
			/* XXX This differs from upstream. Send a pull request. */
			rela->string = rela->sym->sec->data->d_buf +
				       rela->sym->sym.st_value + rela->addend;

			/*
			 * XXX This is a bit of a hack. Investigate with
			 * upstream.
			 */
			/*
			 * If the relocation is relative to %rip, the addend
			 * includes the length of the instruction, so offset by
			 * the length of the instruction to get the actual
			 * string (if the relocation is to a .text section).
			 * If the relocation is to a debug frame, ignore the
			 * since it encodes the line number.
			 */
			if (rela->type == R_X86_64_PC32) {
				if (!strncmp(sec->base->name, ".text", 5)) {
					struct insn insn;
					rela_insn(sec, rela, &insn);
					rela->string += (long)insn.next_byte -
							(long)sec->base->data->d_buf -
							rela->offset;
				} else {
					rela->string -= rela->addend;
				}
			}
			if (!rela->string)
				ERROR("could not lookup rela string for %s+%d",
				      rela->sym->name, rela->addend);
		}

		if (skip)
			continue;
		log_debug("offset %d, type %d, %s %s %d", rela->offset,
			rela->type, rela->sym->name,
			(rela->addend < 0)?"-":"+", abs(rela->addend));
		if (rela->string)
			log_debug(" (string = %s)", rela->string);
		log_debug("\n");
	}
}

void xsplice_create_section_list(struct xsplice_elf *kelf)
{
	Elf_Scn *scn = NULL;
	struct section *sec;
	size_t shstrndx, sections_nr;

	if (elf_getshdrnum(kelf->elf, &sections_nr))
		ERROR("elf_getshdrnum");

	/*
	 * elf_getshdrnum() includes section index 0 but elf_nextscn
	 * doesn't return that section so subtract one.
	 */
	sections_nr--;

	if (elf_getshdrstrndx(kelf->elf, &shstrndx))
		ERROR("elf_getshdrstrndx");

	log_debug("=== section list (%zu) ===\n", sections_nr);

	while (sections_nr--) {
		ALLOC_LINK(sec, &kelf->sections);

		scn = elf_nextscn(kelf->elf, scn);
		if (!scn)
			ERROR("scn NULL");

		if (!gelf_getshdr(scn, &sec->sh))
			ERROR("gelf_getshdr");

		sec->name = elf_strptr(kelf->elf, shstrndx, sec->sh.sh_name);
		if (!sec->name)
			ERROR("elf_strptr");

		sec->data = elf_getdata(scn, NULL);
		if (!sec->data)
			ERROR("elf_getdata");

		sec->index = elf_ndxscn(scn);

		log_debug("ndx %02d, data %p, size %zu, name %s\n",
			sec->index, sec->data->d_buf, sec->data->d_size,
			sec->name);
	}

	/* Sanity check, one more call to elf_nextscn() should return NULL */
	if (elf_nextscn(kelf->elf, scn))
		ERROR("expected NULL");
}

int is_bundleable(struct symbol *sym)
{
	if (sym->type == STT_FUNC &&
	    !strncmp(sym->sec->name, ".text.",6) &&
	    !strcmp(sym->sec->name + 6, sym->name))
		return 1;

	if (sym->type == STT_FUNC &&
	    !strncmp(sym->sec->name, ".text.unlikely.",15) &&
	    !strcmp(sym->sec->name + 15, sym->name))
		return 1;

	if (sym->type == STT_OBJECT &&
	   !strncmp(sym->sec->name, ".data.",6) &&
	   !strcmp(sym->sec->name + 6, sym->name))
		return 1;

	if (sym->type == STT_OBJECT &&
	   !strncmp(sym->sec->name, ".rodata.",8) &&
	   !strcmp(sym->sec->name + 8, sym->name))
		return 1;

	if (sym->type == STT_OBJECT &&
	   !strncmp(sym->sec->name, ".bss.",5) &&
	   !strcmp(sym->sec->name + 5, sym->name))
		return 1;

	return 0;
}

void xsplice_create_symbol_list(struct xsplice_elf *kelf)
{
	struct section *symtab;
	struct symbol *sym;
	int symbols_nr, index = 0;

	symtab = find_section_by_name(&kelf->sections, ".symtab");
	if (!symtab)
		ERROR("missing symbol table");

	symbols_nr = symtab->sh.sh_size / symtab->sh.sh_entsize;

	log_debug("\n=== symbol list (%d entries) ===\n", symbols_nr);

	while (symbols_nr--) {
		ALLOC_LINK(sym, &kelf->symbols);

		sym->index = index;
		if (!gelf_getsym(symtab->data, index, &sym->sym))
			ERROR("gelf_getsym");
		index++;

		sym->name = elf_strptr(kelf->elf, symtab->sh.sh_link,
				       sym->sym.st_name);
		if (!sym->name)
			ERROR("elf_strptr");

		sym->type = GELF_ST_TYPE(sym->sym.st_info);
		sym->bind = GELF_ST_BIND(sym->sym.st_info);

		if (sym->sym.st_shndx > SHN_UNDEF &&
		    sym->sym.st_shndx < SHN_LORESERVE) {
			sym->sec = find_section_by_index(&kelf->sections,
					sym->sym.st_shndx);
			if (!sym->sec)
				ERROR("couldn't find section for symbol %s\n",
					sym->name);

			if (is_bundleable(sym)) {
				if (sym->sym.st_value != 0)
					ERROR("symbol %s at offset %lu within section %s, expected 0",
					      sym->name, sym->sym.st_value, sym->sec->name);
				sym->sec->sym = sym;
			} else if (sym->type == STT_SECTION) {
				sym->sec->secsym = sym;
				/* use the section name as the symbol name */
				sym->name = sym->sec->name;
			}
		}

		log_debug("sym %02d, type %d, bind %d, ndx %02d, name %s",
			sym->index, sym->type, sym->bind, sym->sym.st_shndx,
			sym->name);
		if (sym->sec)
			log_debug(" -> %s", sym->sec->name);
		log_debug("\n");
	}

}

struct xsplice_elf *xsplice_elf_open(const char *name)
{
	Elf *elf;
	int fd;
	struct xsplice_elf *kelf;
	struct section *sec;

	fd = open(name, O_RDONLY);
	if (fd == -1)
		ERROR("open");

	elf = elf_begin(fd, ELF_C_READ_MMAP, NULL);
	if (!elf)
		ERROR("elf_begin");

	kelf = malloc(sizeof(*kelf));
	if (!kelf)
		ERROR("malloc");
	memset(kelf, 0, sizeof(*kelf));
	INIT_LIST_HEAD(&kelf->sections);
	INIT_LIST_HEAD(&kelf->symbols);
	INIT_LIST_HEAD(&kelf->strings);

	/* read and store section, symbol entries from file */
	kelf->elf = elf;
	kelf->fd = fd;
	xsplice_create_section_list(kelf);
	xsplice_create_symbol_list(kelf);

	/* for each rela section, read and store the rela entries */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec))
			continue;
		INIT_LIST_HEAD(&sec->relas);
		xsplice_create_rela_list(kelf, sec);
	}

	return kelf;
}

void xsplice_compare_elf_headers(Elf *elf1, Elf *elf2)
{
	GElf_Ehdr eh1, eh2;

	if (!gelf_getehdr(elf1, &eh1))
		ERROR("gelf_getehdr");

	if (!gelf_getehdr(elf2, &eh2))
		ERROR("gelf_getehdr");

	if (memcmp(eh1.e_ident, eh2.e_ident, EI_NIDENT) ||
	    eh1.e_type != eh2.e_type ||
	    eh1.e_machine != eh2.e_machine ||
	    eh1.e_version != eh2.e_version ||
	    eh1.e_entry != eh2.e_entry ||
	    eh1.e_phoff != eh2.e_phoff ||
	    eh1.e_flags != eh2.e_flags ||
	    eh1.e_ehsize != eh2.e_ehsize ||
	    eh1.e_phentsize != eh2.e_phentsize ||
	    eh1.e_shentsize != eh2.e_shentsize)
		DIFF_FATAL("ELF headers differ");
}

void xsplice_check_program_headers(Elf *elf)
{
	size_t ph_nr;

	if (elf_getphdrnum(elf, &ph_nr))
		ERROR("elf_getphdrnum");

	if (ph_nr != 0)
		DIFF_FATAL("ELF contains program header");
}

void xsplice_mark_grouped_sections(struct xsplice_elf *kelf)
{
	struct section *groupsec, *sec;
	unsigned int *data, *end;

	list_for_each_entry(groupsec, &kelf->sections, list) {
		if (groupsec->sh.sh_type != SHT_GROUP)
			continue;
		data = groupsec->data->d_buf;
		end = groupsec->data->d_buf + groupsec->data->d_size;
		data++; /* skip first flag word (e.g. GRP_COMDAT) */
		while (data < end) {
			sec = find_section_by_index(&kelf->sections, *data);
			if (!sec)
				ERROR("group section not found");
			sec->grouped = 1;
			log_debug("marking section %s (%d) as grouped\n",
			          sec->name, sec->index);
			data++;
		}
	}
}

/*
 * Mangle the relas a little.  The compiler will sometimes use section symbols
 * to reference local objects and functions rather than the object or function
 * symbols themselves.  We substitute the object/function symbols for the
 * section symbol in this case so that the relas can be properly correlated and
 * so that the existing object/function in vmlinux can be linked to.
 */
void xsplice_replace_sections_syms(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct rela *rela;
	struct symbol *sym;
	int add_off;

	log_debug("\n");

	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec) ||
		    is_debug_section(sec))
			continue;

		list_for_each_entry(rela, &sec->relas, list) {

			if (rela->sym->type != STT_SECTION)
				continue;

			/*
			 * Replace references to bundled sections with their
			 * symbols.
			 */
			if (rela->sym->sec && rela->sym->sec->sym) {
				rela->sym = rela->sym->sec->sym;
				continue;
			}

			if (rela->type == R_X86_64_PC32) {
				struct insn insn;
				rela_insn(sec, rela, &insn);
				add_off = (long)insn.next_byte -
					  (long)sec->base->data->d_buf -
					  rela->offset;
			} else if (rela->type == R_X86_64_64 ||
				   rela->type == R_X86_64_32S)
				add_off = 0;
			else
				continue;

			/*
			 * Attempt to replace references to unbundled sections
			 * with their symbols.
			 */
			list_for_each_entry(sym, &kelf->symbols, list) {
				int start, end;

				if (sym->type == STT_SECTION ||
				    sym->sec != rela->sym->sec)
					continue;

				start = sym->sym.st_value;
				end = sym->sym.st_value + sym->sym.st_size;

				if (rela->addend + add_off < start ||
				    rela->addend + add_off >= end)
					continue;

				log_debug("%s: replacing %s+%d reference with %s+%d\n",
					  sec->name,
					  rela->sym->name, rela->addend,
					  sym->name, rela->addend - start);

				rela->sym = sym;
				rela->addend -= start;
				break;
			}
		}
	}
	log_debug("\n");
}

/*
 * This is like strcmp, but for gcc-mangled symbols.  It skips the comparison
 * of any substring which consists of '.' followed by any number of digits.
 */
static int xsplice_mangled_strcmp(char *s1, char *s2)
{
	while (*s1 == *s2) {
		if (!*s1)
			return 0;
		if (*s1 == '.' && isdigit(s1[1])) {
			if (!isdigit(s2[1]))
				return 1;
			while (isdigit(*++s1))
				;
			while (isdigit(*++s2))
				;
		} else {
			s1++;
			s2++;
		}
	}
	return 1;
}

/*
 * When gcc makes compiler optimizations which affect a function's calling
 * interface, it mangles the function's name.  For example, sysctl_print_dir is
 * renamed to sysctl_print_dir.isra.2.  The problem is that the trailing number
 * is chosen arbitrarily, and the patched version of the function may end up
 * with a different trailing number.  Rename any mangled patched functions to
 * match their base counterparts.
 */
void xsplice_rename_mangled_functions(struct xsplice_elf *base,
				     struct xsplice_elf *patched)
{
	struct symbol *sym, *basesym;
	char name[256], *origname;
	struct section *sec, *basesec;
	int found;

	list_for_each_entry(sym, &patched->symbols, list) {
		if (sym->type != STT_FUNC)
			continue;

		if (!strstr(sym->name, ".isra.") &&
		    !strstr(sym->name, ".constprop.") &&
		    !strstr(sym->name, ".part."))
			continue;

		found = 0;
		list_for_each_entry(basesym, &base->symbols, list) {
			if (!xsplice_mangled_strcmp(basesym->name, sym->name)) {
				found = 1;
				break;
			}
		}

		if (!found)
			continue;

		if (!strcmp(sym->name, basesym->name))
			continue;

		log_debug("renaming %s to %s\n", sym->name, basesym->name);
		origname = sym->name;
		sym->name = strdup(basesym->name);

		if (sym != sym->sec->sym)
			continue;

		sym->sec->name = strdup(basesym->sec->name);
		if (sym->sec->rela)
			sym->sec->rela->name = strdup(basesym->sec->rela->name);

		/*
		 * When function foo.isra.1 has a switch statement, it might
		 * have a corresponding bundled .rodata.foo.isra.1 section (in
		 * addition to .text.foo.isra.1 which we renamed above).
		 */
		sprintf(name, ".rodata.%s", origname);
		sec = find_section_by_name(&patched->sections, name);
		if (!sec)
			continue;
		sprintf(name, ".rodata.%s", basesym->name);
		basesec = find_section_by_name(&base->sections, name);
		if (!basesec)
			continue;
		sec->name = strdup(basesec->name);
		sec->secsym->name = sec->name;
		if (sec->rela)
			sec->rela->name = strdup(basesec->rela->name);
	}
}

/*
 * This function detects whether the given symbol is a "special" static local
 * variable (for lack of a better term).
 *
 * Special static local variables should never be correlated and should always
 * be included if they are referenced by an included function.
 */
static int is_special_static(struct symbol *sym)
{
	static char *prefixes[] = {
		"__key.",
		"__warned.",
		"descriptor.",
		"__func__.",
		"_rs.",
		NULL,
	};
	char **prefix;

	if (!sym)
		return 0;

	if (sym->type == STT_SECTION) {
		/* __verbose section contains the descriptor variables */
		if (!strcmp(sym->name, "__verbose"))
			return 1;

		/* otherwise make sure section is bundled */
		if (!sym->sec->sym)
			return 0;

		/* use bundled object/function symbol for matching */
		sym = sym->sec->sym;
	}

	if (sym->type != STT_OBJECT || sym->bind != STB_LOCAL)
		return 0;

	for (prefix = prefixes; *prefix; prefix++)
		if (!strncmp(sym->name, *prefix, strlen(*prefix)))
			return 1;

	return 0;
}

static int is_constant_label(struct symbol *sym)
{
	const char *str;

	if (sym->bind != STB_LOCAL)
		return 0;

	if (strncmp(sym->name, ".LC", 3))
		return 0;

	str = sym->name + 3;
	do {
		if (!isdigit(*str))
			return 0;
	} while (*++str);

	return 1;
}

void xsplice_correlate_sections(struct list_head *seclist1, struct list_head *seclist2)
{
	struct section *sec1, *sec2;

	list_for_each_entry(sec1, seclist1, list) {
		list_for_each_entry(sec2, seclist2, list) {
			if (strcmp(sec1->name, sec2->name))
				continue;

			if (is_special_static(is_rela_section(sec1) ?
					      sec1->base->secsym :
					      sec1->secsym))
				continue;

			/*
			 * Group sections must match exactly to be correlated.
			 * Changed group sections are currently not supported.
			 */
			if (sec1->sh.sh_type == SHT_GROUP) {
				if (sec1->data->d_size != sec2->data->d_size)
					continue;
				if (memcmp(sec1->data->d_buf, sec2->data->d_buf,
				           sec1->data->d_size))
					continue;
			}
			log_debug("Found section twins: %s\n", sec1->name);
			sec1->twin = sec2;
			sec2->twin = sec1;
			/* set initial status, might change */
			sec1->status = sec2->status = SAME;
			break;
		}
	}
}

void xsplice_correlate_symbols(struct list_head *symlist1, struct list_head *symlist2)
{
	struct symbol *sym1, *sym2;

	list_for_each_entry(sym1, symlist1, list) {
		list_for_each_entry(sym2, symlist2, list) {
			if (strcmp(sym1->name, sym2->name) ||
			    sym1->type != sym2->type)
				continue;

			if (is_special_static(sym1))
				continue;

			if (is_constant_label(sym1))
				continue;

			/* group section symbols must have correlated sections */
			if (sym1->sec &&
			    sym1->sec->sh.sh_type == SHT_GROUP &&
			    sym1->sec->twin != sym2->sec)
				continue;

			log_debug("Found symbol twins: %s\n", sym1->name);
			sym1->twin = sym2;
			sym2->twin = sym1;
			/* set initial status, might change */
			sym1->status = sym2->status = SAME;
			break;
		}
	}
}

void xsplice_correlate_elfs(struct xsplice_elf *kelf1, struct xsplice_elf *kelf2)
{
	xsplice_correlate_sections(&kelf1->sections, &kelf2->sections);
	xsplice_correlate_symbols(&kelf1->symbols, &kelf2->symbols);
}

static char *xsplice_section_function_name(struct section *sec)
{
	if (is_rela_section(sec))
		sec = sec->base;
	return sec->sym ? sec->sym->name : sec->name;
}

/*
 * Given a static local variable symbol and a section which references it in
 * the patched object, find a corresponding usage of a similarly named symbol
 * in the base object.
 */
static struct symbol *xsplice_find_static_twin(struct section *sec,
					      struct symbol *sym)
{
	struct rela *rela;
	struct symbol *basesym;

	if (!sec->twin)
		return NULL;

	/*
	 * Ensure there are no other orphaned static variables with the
	 * same name in the function.  This is possible if the
	 * variables are in different scopes or if one of them is part of an
	 * inlined function.
	 */
	list_for_each_entry(rela, &sec->relas, list) {
		if (rela->sym == sym || rela->sym->twin)
			continue;
		if (!xsplice_mangled_strcmp(rela->sym->name, sym->name))
			ERROR("found another static local variable matching %s in patched %s",
			      sym->name, xsplice_section_function_name(sec));
	}

	/* find the base object's corresponding variable */
	basesym = NULL;
	list_for_each_entry(rela, &sec->twin->relas, list) {
		if (rela->sym->twin)
			continue;
		if (xsplice_mangled_strcmp(rela->sym->name, sym->name))
			continue;
		if (basesym && basesym != rela->sym)
			ERROR("found two static local variables matching %s in orig %s",
			      sym->name, xsplice_section_function_name(sec));

		basesym = rela->sym;
	}

	return basesym;
}

/*
 * gcc renames static local variables by appending a period and a number.  For
 * example, __foo could be renamed to __foo.31452.  Unfortunately this number
 * can arbitrarily change.  Try to rename the patched version of the symbol to
 * match the base version and then correlate them.
 */
void xsplice_correlate_static_local_variables(struct xsplice_elf *base,
					     struct xsplice_elf *patched)
{
	struct symbol *sym, *basesym, *tmpsym;
	struct section *tmpsec, *sec;
	struct rela *rela;
	int bundled, basebundled;

	list_for_each_entry(sym, &patched->symbols, list) {
		if (sym->type != STT_OBJECT || sym->bind != STB_LOCAL ||
		    sym->twin)
			continue;

		if (is_special_static(sym))
			continue;

		if (!strchr(sym->name, '.'))
			continue;

		/*
		 * For each function which uses the variable in the patched
		 * object, look for a corresponding use in the function's twin
		 * in the base object.
		 *
		 * It's possible for multiple functions to use the same static
		 * local variable if the variable is defined in an inlined
		 * function.
		 */
		sec = NULL;
		basesym = NULL;
		list_for_each_entry(tmpsec, &patched->sections, list) {
			if (!is_rela_section(tmpsec) ||
			    !is_text_section(tmpsec->base) ||
			    is_debug_section(tmpsec))
				continue;
			list_for_each_entry(rela, &tmpsec->relas, list) {
				if (rela->sym != sym)
					continue;

				tmpsym = xsplice_find_static_twin(tmpsec, sym);
				if (basesym && tmpsym && basesym != tmpsym)
					ERROR("found two twins for static local variable %s: %s and %s",
					      sym->name, basesym->name,
					      tmpsym->name);
				if (tmpsym && !basesym)
					basesym = tmpsym;

				sec = tmpsec;
				break;
			}
		}

		if (!sec)
			ERROR("static local variable %s not used", sym->name);

		if (!basesym) {
			log_normal("WARNING: unable to correlate static local variable %s used by %s, assuming variable is new\n",
				   sym->name,
				   xsplice_section_function_name(sec));
			continue;
		}


		bundled = sym == sym->sec->sym;
		basebundled = basesym == basesym->sec->sym;
		if (bundled != basebundled)
			ERROR("bundle mismatch for symbol %s", sym->name);
		if (!bundled && sym->sec->twin != basesym->sec)
			ERROR("sections %s and %s aren't correlated",
			      sym->sec->name, basesym->sec->name);

		log_debug("renaming and correlating %s to %s\n",
			  sym->name, basesym->name);
		sym->name = strdup(basesym->name);
		sym->twin = basesym;
		basesym->twin = sym;
		sym->status = basesym->status = SAME;

		if (bundled) {
			sym->sec->twin = basesym->sec;
			basesym->sec->twin = sym->sec;
		}
	}
}

int rela_equal(struct rela *rela1, struct rela *rela2)
{
	log_debug("Comparing rela %s with %s\n", rela1->sym->name, rela2->sym->name);
	if (rela1->type != rela2->type ||
	    rela1->offset != rela2->offset)
		return 0;

	if (rela1->string)
		return rela2->string && !strcmp(rela1->string, rela2->string);

	if (rela1->addend != rela2->addend)
		return 0;

	if (is_constant_label(rela1->sym) && is_constant_label(rela2->sym))
		return 1;

	if (is_special_static(rela1->sym))
		return !xsplice_mangled_strcmp(rela1->sym->name,
					      rela2->sym->name);

	return !strcmp(rela1->sym->name, rela2->sym->name);
}

void xsplice_compare_correlated_rela_section(struct section *sec)
{
	struct rela *rela1, *rela2 = NULL;

	rela2 = list_entry(sec->twin->relas.next, struct rela, list);
	list_for_each_entry(rela1, &sec->relas, list) {
		if (rela_equal(rela1, rela2)) {
			rela2 = list_entry(rela2->list.next, struct rela, list);
			continue;
		}
		sec->status = CHANGED;
		return;
	}

	sec->status = SAME;
}

void xsplice_compare_correlated_nonrela_section(struct section *sec)
{
	struct section *sec1 = sec, *sec2 = sec->twin;

	if (sec1->sh.sh_type != SHT_NOBITS &&
	    memcmp(sec1->data->d_buf, sec2->data->d_buf, sec1->data->d_size))
		sec->status = CHANGED;
	else
		sec->status = SAME;
}

void xsplice_compare_correlated_section(struct section *sec)
{
	struct section *sec1 = sec, *sec2 = sec->twin;

	log_debug("Compare correlated section: %s\n", sec->name);

	/* Compare section headers (must match or fatal) */
	if (sec1->sh.sh_type != sec2->sh.sh_type ||
	    sec1->sh.sh_flags != sec2->sh.sh_flags ||
	    sec1->sh.sh_addr != sec2->sh.sh_addr ||
	    sec1->sh.sh_addralign != sec2->sh.sh_addralign ||
	    sec1->sh.sh_entsize != sec2->sh.sh_entsize)
		DIFF_FATAL("%s section header details differ", sec1->name);

	if (sec1->sh.sh_size != sec2->sh.sh_size ||
	    sec1->data->d_size != sec2->data->d_size) {
		sec->status = CHANGED;
		goto out;
	}

	if (is_rela_section(sec))
		xsplice_compare_correlated_rela_section(sec);
	else
		xsplice_compare_correlated_nonrela_section(sec);
out:
	if (sec->status == CHANGED)
		log_debug("section %s has changed\n", sec->name);
}

void xsplice_compare_sections(struct list_head *seclist)
{
	struct section *sec;

	/* compare all sections */
	list_for_each_entry(sec, seclist, list) {
		if (sec->twin)
			xsplice_compare_correlated_section(sec);
		else
			sec->status = NEW;
	}

	/* sync symbol status */
	list_for_each_entry(sec, seclist, list) {
		if (is_rela_section(sec)) {
			if (sec->base->sym && sec->base->sym->status != CHANGED)
				sec->base->sym->status = sec->status;
		} else {
			if (sec->sym && sec->sym->status != CHANGED)
				sec->sym->status = sec->status;
		}
	}
}

void xsplice_compare_correlated_symbol(struct symbol *sym)
{
	struct symbol *sym1 = sym, *sym2 = sym->twin;

	if (sym1->sym.st_info != sym2->sym.st_info ||
	    sym1->sym.st_other != sym2->sym.st_other ||
	    (sym1->sec && !sym2->sec) ||
	    (sym2->sec && !sym1->sec))
		DIFF_FATAL("symbol info mismatch: %s", sym1->name);

	/*
	 * If two symbols are correlated but their sections are not, then the
	 * symbol has changed sections.  This is only allowed if the symbol is
	 * moving out of an ignored section.
	 */
	if (sym1->sec && sym2->sec && sym1->sec->twin != sym2->sec) {
		if (sym2->sec->twin && sym2->sec->twin->ignore)
			sym->status = CHANGED;
		else
			DIFF_FATAL("symbol changed sections: %s, %s, %s, %s", sym1->name, sym2->name, sym1->sec->name, sym2->sec->name);
	}

	if (sym1->type == STT_OBJECT &&
	    sym1->sym.st_size != sym2->sym.st_size)
		DIFF_FATAL("object size mismatch: %s", sym1->name);

	if (sym1->sym.st_shndx == SHN_UNDEF ||
	     sym1->sym.st_shndx == SHN_ABS)
		sym1->status = SAME;

	/*
	 * The status of LOCAL symbols is dependent on the status of their
	 * matching section and is set during section comparison.
	 */
}

void xsplice_compare_symbols(struct list_head *symlist)
{
	struct symbol *sym;

	list_for_each_entry(sym, symlist, list) {
		if (sym->twin)
			xsplice_compare_correlated_symbol(sym);
		else
			sym->status = NEW;

		log_debug("symbol %s is %s\n", sym->name, status_str(sym->status));
	}
}

void xsplice_compare_correlated_elements(struct xsplice_elf *kelf)
{
	/* lists are already correlated at this point */
	log_debug("Compare sections\n");
	xsplice_compare_sections(&kelf->sections);
	log_debug("Compare symbols\n");
	xsplice_compare_symbols(&kelf->symbols);
}

/*
 * While this is a one-shot program without a lot of proper cleanup in case
 * of an error, this function serves a debugging purpose: to break down and
 * zero data structures we shouldn't be accessing anymore.  This should
 * help cause an immediate and obvious issue when a logic error leads to
 * accessing data that is not intended to be accessed past a particular point.
 */
void xsplice_elf_teardown(struct xsplice_elf *kelf)
{
	struct section *sec, *safesec;
	struct symbol *sym, *safesym;
	struct rela *rela, *saferela;

	list_for_each_entry_safe(sec, safesec, &kelf->sections, list) {
		if (is_rela_section(sec)) {
			list_for_each_entry_safe(rela, saferela, &sec->relas, list) {
				memset(rela, 0, sizeof(*rela));
				free(rela);
			}
			memset(sec, 0, sizeof(*sec));
			free(sec);
		}
	}

	list_for_each_entry_safe(sym, safesym, &kelf->symbols, list) {
		memset(sym, 0, sizeof(*sym));
		free(sym);
	}

	INIT_LIST_HEAD(&kelf->sections);
	INIT_LIST_HEAD(&kelf->symbols);
}

void xsplice_elf_free(struct xsplice_elf *kelf)
{
	elf_end(kelf->elf);
	close(kelf->fd);
	memset(kelf, 0, sizeof(*kelf));
	free(kelf);
}

void xsplice_mark_ignored_sections_same(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;

	list_for_each_entry(sec, &kelf->sections, list) {
		if (!sec->ignore)
			continue;
		sec->status = SAME;
		if (sec->secsym)
			sec->secsym->status = SAME;
		if (sec->rela)
			sec->rela->status = SAME;
		list_for_each_entry(sym, &kelf->symbols, list) {
			if (sym->sec != sec)
				continue;
			sym->status = SAME;
		}
	}
}

void xsplice_mark_constant_labels_same(struct xsplice_elf *kelf)
{
	struct symbol *sym;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (is_constant_label(sym))
			sym->status = SAME;
	}
}

int bug_frames_0_group_size(struct xsplice_elf *kelf, int offset) { return 8; }
int bug_frames_1_group_size(struct xsplice_elf *kelf, int offset) { return 8; }
int bug_frames_2_group_size(struct xsplice_elf *kelf, int offset) { return 8; }
int bug_frames_3_group_size(struct xsplice_elf *kelf, int offset) { return 16; }
int ex_table_group_size(struct xsplice_elf *kelf, int offset) { return 8; }
int altinstructions_group_size(struct xsplice_elf *kelf, int offset) { return 12; }

/*
 * The rela groups in the .fixup section vary in size.  The beginning of each
 * .fixup rela group is referenced by the .ex_table section. To find the size
 * of a .fixup rela group, we have to traverse the .ex_table relas.
 */
int fixup_group_size(struct xsplice_elf *kelf, int offset)
{
	struct section *sec;
	struct rela *rela;
	int found;

	sec = find_section_by_name(&kelf->sections, ".rela.ex_table");
	if (!sec)
		ERROR("missing .rela.ex_table section");

	/* find beginning of this group */
	found = 0;
	list_for_each_entry(rela, &sec->relas, list) {
		if (!strcmp(rela->sym->name, ".fixup") &&
		    rela->addend == offset) {
				found = 1;
				break;
		}
	}

	if (!found)
		ERROR("can't find .fixup rela group at offset %d\n", offset);

	/* find beginning of next group */
	found = 0;
	list_for_each_entry_continue(rela, &sec->relas, list) {
		if (!strcmp(rela->sym->name, ".fixup") &&
		    rela->addend > offset) {
			found = 1;
			break;
		}
	}

	if (!found) {
		/* last group */
		struct section *fixupsec;
		fixupsec = find_section_by_name(&kelf->sections, ".fixup");
		return fixupsec->sh.sh_size - offset;
	}

	return rela->addend - offset;
}

struct special_section special_sections[] = {
	{
		.name		= ".bug_frames.0",
		.group_size	= bug_frames_0_group_size,
	},
	{
		.name		= ".bug_frames.1",
		.group_size	= bug_frames_1_group_size,
	},
	{
		.name		= ".bug_frames.2",
		.group_size	= bug_frames_2_group_size,
	},
	{
		.name		= ".bug_frames.3",
		.group_size	= bug_frames_3_group_size,
	},
	{
		.name		= ".fixup",
		.group_size	= fixup_group_size,
	},
	{
		.name		= ".ex_table",
		.group_size	= ex_table_group_size,
	},
	{
		.name		= ".altinstructions",
		.group_size	= altinstructions_group_size,
	},
	{},
};

int should_keep_rela_group(struct section *sec, int start, int size)
{
	struct rela *rela;
	int found = 0;

	/* check if any relas in the group reference any changed functions */
	list_for_each_entry(rela, &sec->relas, list) {
		if (rela->offset >= start &&
		    rela->offset < start + size &&
		    rela->sym->type == STT_FUNC &&
		    rela->sym->sec->include) {
			found = 1;
			log_debug("new/changed symbol %s found in special section %s\n",
				  rela->sym->name, sec->name);
		}
	}

	return found;
}

void xsplice_regenerate_special_section(struct xsplice_elf *kelf,
				       struct special_section *special,
				       struct section *sec)
{
	struct rela *rela, *safe;
	char *src, *dest;
	int group_size, src_offset, dest_offset, include, align, aligned_size;

	LIST_HEAD(newrelas);

	src = sec->base->data->d_buf;
	/* alloc buffer for new base section */
	dest = malloc(sec->base->sh.sh_size);
	if (!dest)
		ERROR("malloc");

	group_size = 0;
	src_offset = 0;
	dest_offset = 0;
	for ( ; src_offset < sec->base->sh.sh_size; src_offset += group_size) {

		group_size = special->group_size(kelf, src_offset);
		include = should_keep_rela_group(sec, src_offset, group_size);

		if (!include)
			continue;

		/*
		 * Copy all relas in the group.  It's possible that the relas
		 * aren't sorted (e.g. .rela.fixup), so go through the entire
		 * rela list each time.
		 */
		list_for_each_entry_safe(rela, safe, &sec->relas, list) {
			if (rela->offset >= src_offset &&
			    rela->offset < src_offset + group_size) {
				/* copy rela entry */
				list_del(&rela->list);
				list_add_tail(&rela->list, &newrelas);

				rela->offset -= src_offset - dest_offset;
				rela->rela.r_offset = rela->offset;

				rela->sym->include = 1;
			}
		}

		/* copy base section group */
		memcpy(dest + dest_offset, src + src_offset, group_size);
		dest_offset += group_size;
	}

	/* verify that group_size is a divisor of aligned section size */
	align = sec->base->sh.sh_addralign;
	aligned_size = ((sec->base->sh.sh_size + align - 1) / align) * align;
	if (src_offset != aligned_size)
		ERROR("group size mismatch for section %s\n", sec->base->name);

	if (!dest_offset) {
		/* no changed or global functions referenced */
		sec->status = sec->base->status = SAME;
		sec->include = sec->base->include = 0;
		free(dest);
		return;
	}

	/* overwrite with new relas list */
	list_replace(&newrelas, &sec->relas);

	/* include both rela and base sections */
	sec->include = 1;
	sec->base->include = 1;

	/*
	 * Update text section data buf and size.
	 *
	 * The rela section's data buf and size will be regenerated in
	 * xsplice_rebuild_rela_section_data().
	 */
	sec->base->data->d_buf = dest;
	sec->base->data->d_size = dest_offset;
}

void xsplice_process_special_sections(struct xsplice_elf *kelf)
{
	struct special_section *special;
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;

	for (special = special_sections; special->name; special++) {
		sec = find_section_by_name(&kelf->sections, special->name);
		if (!sec)
			continue;

		sec = sec->rela;
		if (!sec)
			continue;

		xsplice_regenerate_special_section(kelf, special, sec);
	}

	/*
	 * The following special sections don't have relas which reference
	 * non-included symbols, so their entire rela section can be included.
	 */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (strcmp(sec->name, ".altinstr_replacement"))
			continue;

		/* include base section */
		sec->include = 1;

		/* include all symbols in the section */
		list_for_each_entry(sym, &kelf->symbols, list)
			if (sym->sec == sec)
				sym->include = 1;

		/* include rela section */
		if (sec->rela) {
			sec->rela->include = 1;
			/* include all symbols referenced by relas */
			list_for_each_entry(rela, &sec->rela->relas, list)
				rela->sym->include = 1;
		}
	}
}

void xsplice_include_standard_elements(struct xsplice_elf *kelf)
{
	struct section *sec;

	list_for_each_entry(sec, &kelf->sections, list) {
		/* include these sections even if they haven't changed */
		if (!strcmp(sec->name, ".shstrtab") ||
		    !strcmp(sec->name, ".strtab") ||
		    !strcmp(sec->name, ".symtab") ||
		    !strncmp(sec->name, ".rodata.str1.", 13)) {
			sec->include = 1;
			if (sec->secsym)
				sec->secsym->include = 1;
		}
	}

	/* include the NULL symbol */
	list_entry(kelf->symbols.next, struct symbol, list)->include = 1;
}

#define inc_printf(fmt, ...) \
	log_debug("%*s" fmt, recurselevel, "", ##__VA_ARGS__);

void xsplice_include_symbol(struct symbol *sym, int recurselevel)
{
	struct rela *rela;
	struct section *sec;

	inc_printf("start include_symbol(%s)\n", sym->name);
	sym->include = 1;
	inc_printf("symbol %s is included\n", sym->name);
	/*
	 * Check if sym is a non-local symbol (sym->sec is NULL) or
	 * if an unchanged local symbol.  This a base case for the
	 * inclusion recursion.
	 */
	if (!sym->sec || sym->sec->include ||
	    (sym->type != STT_SECTION && sym->status == SAME))
		goto out;
	sec = sym->sec;
	sec->include = 1;
	inc_printf("section %s is included\n", sec->name);
	if (sec->secsym && sec->secsym != sym) {
		sec->secsym->include = 1;
		inc_printf("section symbol %s is included\n", sec->secsym->name);
	}
	if (!sec->rela)
		goto out;
	sec->rela->include = 1;
	inc_printf("section %s is included\n", sec->rela->name);
	list_for_each_entry(rela, &sec->rela->relas, list)
		xsplice_include_symbol(rela->sym, recurselevel+1);
out:
	inc_printf("end include_symbol(%s)\n", sym->name);
	return;
}

int xsplice_include_changed_functions(struct xsplice_elf *kelf)
{
	struct symbol *sym;
	int changed_nr = 0;

	log_debug("\n=== Inclusion Tree ===\n");

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->status == CHANGED &&
		    sym->type == STT_FUNC) {
			changed_nr++;
			xsplice_include_symbol(sym, 0);
		}

		if (sym->type == STT_FILE)
			sym->include = 1;
	}

	return changed_nr;
}

void xsplice_include_debug_sections(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct rela *rela, *saferela;

	/* include all .debug_* sections */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (is_debug_section(sec)) {
			sec->include = 1;
			if (!is_rela_section(sec))
				sec->secsym->include = 1;
		}
	}

	/*
	 * Go through the .rela.debug_ sections and strip entries
	 * referencing unchanged symbols
	 */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec) || !is_debug_section(sec))
			continue;
		list_for_each_entry_safe(rela, saferela, &sec->relas, list)
			if (!rela->sym->sec->include)
				list_del(&rela->list);
	}
}

int xsplice_include_new_globals(struct xsplice_elf *kelf)
{
	struct symbol *sym;
	int nr = 0;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->bind == STB_GLOBAL && sym->sec &&
		    sym->status == NEW) {
			xsplice_include_symbol(sym, 0);
			nr++;
		}
	}

	return nr;
}

void xsplice_print_changes(struct xsplice_elf *kelf)
{
	struct symbol *sym;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (!sym->include || !sym->sec || sym->type != STT_FUNC)
			continue;
		if (sym->status == NEW)
			log_normal("new function: %s\n", sym->name);
		else if (sym->status == CHANGED)
			log_normal("changed function: %s\n", sym->name);
	}
}

void xsplice_dump_kelf(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;

	if (loglevel > DEBUG)
		return;

	printf("\n=== Sections ===\n");
	list_for_each_entry(sec, &kelf->sections, list) {
		printf("%02d %s (%s)", sec->index, sec->name, status_str(sec->status));
		if (is_rela_section(sec)) {
			printf(", base-> %s\n", sec->base->name);
			/* skip .debug_* sections */
			if (is_debug_section(sec))
				goto next;
			printf("rela section expansion\n");
			list_for_each_entry(rela, &sec->relas, list) {
				printf("sym %d, offset %d, type %d, %s %s %d\n",
				       rela->sym->index, rela->offset,
				       rela->type, rela->sym->name,
				       (rela->addend < 0)?"-":"+",
				       abs(rela->addend));
			}
		} else {
			if (sec->sym)
				printf(", sym-> %s", sec->sym->name);
			if (sec->secsym)
				printf(", secsym-> %s", sec->secsym->name);
			if (sec->rela)
				printf(", rela-> %s", sec->rela->name);
		}
next:
		printf("\n");
	}

	printf("\n=== Symbols ===\n");
	list_for_each_entry(sym, &kelf->symbols, list) {
		printf("sym %02d, type %d, bind %d, ndx %02d, name %s (%s)",
			sym->index, sym->type, sym->bind, sym->sym.st_shndx,
			sym->name, status_str(sym->status));
		if (sym->sec && (sym->type == STT_FUNC || sym->type == STT_OBJECT))
			printf(" -> %s", sym->sec->name);
		printf("\n");
	}
}

void xsplice_verify_patchability(struct xsplice_elf *kelf)
{
	struct section *sec;
	int errs = 0;

	list_for_each_entry(sec, &kelf->sections, list) {
		if (sec->status == CHANGED && !sec->include) {
			log_normal("changed section %s not selected for inclusion\n",
				   sec->name);
			errs++;
		}

		if (sec->status != SAME && sec->grouped) {
			log_normal("changed section %s is part of a section group\n",
				   sec->name);
			errs++;
		}

		if (sec->sh.sh_type == SHT_GROUP && sec->status == NEW) {
			log_normal("new/changed group sections are not supported\n");
			errs++;
		}

		/*
		 * ensure we aren't including .data.* or .bss.*
		 * (.data.unlikely is ok b/c it only has __warned vars)
		 */
		if (sec->include && sec->status != NEW &&
		    (!strncmp(sec->name, ".data", 5) ||
		     !strncmp(sec->name, ".bss", 4)) &&
		    strcmp(sec->name, ".data.unlikely")) {
			log_normal("data section %s selected for inclusion\n",
				   sec->name);
			errs++;
		}
	}

	if (errs)
		DIFF_FATAL("%d unsupported section change(s)", errs);
}

void xsplice_migrate_included_elements(struct xsplice_elf *kelf, struct xsplice_elf **kelfout)
{
	struct section *sec, *safesec;
	struct symbol *sym, *safesym;
	struct xsplice_elf *out;

	/* allocate output kelf */
	out = malloc(sizeof(*out));
	if (!out)
		ERROR("malloc");
	memset(out, 0, sizeof(*out));
	INIT_LIST_HEAD(&out->sections);
	INIT_LIST_HEAD(&out->symbols);
	INIT_LIST_HEAD(&out->strings);

	/* migrate included sections from kelf to out */
	list_for_each_entry_safe(sec, safesec, &kelf->sections, list) {
		if (!sec->include)
			continue;
		list_del(&sec->list);
		list_add_tail(&sec->list, &out->sections);
		sec->index = 0;
		if (!is_rela_section(sec) && sec->secsym && !sec->secsym->include)
			/* break link to non-included section symbol */
			sec->secsym = NULL;
	}

	/* migrate included symbols from kelf to out */
	list_for_each_entry_safe(sym, safesym, &kelf->symbols, list) {
		if (!sym->include)
			continue;
		list_del(&sym->list);
		list_add_tail(&sym->list, &out->symbols);
		sym->index = 0;
		sym->strip = 0;
		if (sym->sec && !sym->sec->include)
			/* break link to non-included section */
			sym->sec = NULL;
			
	}

	*kelfout = out;
}

void xsplice_migrate_symbols(struct list_head *src,
                                    struct list_head *dst,
                                    int (*select)(struct symbol *))
{
	struct symbol *sym, *safe;

	list_for_each_entry_safe(sym, safe, src, list) {
		if (select && !select(sym))
			continue;

		list_del(&sym->list);
		list_add_tail(&sym->list, dst);
	}
}

void xsplice_create_strings_elements(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;

	/* create .xsplice.strings */

	/* allocate section resources */
	ALLOC_LINK(sec, &kelf->sections);
	sec->name = ".xsplice.strings";

	/* set data */
	sec->data = malloc(sizeof(*sec->data));
	if (!sec->data)
		ERROR("malloc");
	sec->data->d_type = ELF_T_BYTE;

	/* set section header */
	sec->sh.sh_type = SHT_PROGBITS;
	sec->sh.sh_entsize = 1;
	sec->sh.sh_addralign = 1;
	sec->sh.sh_flags = SHF_ALLOC;

	/* create .xsplice.strings section symbol (reuse sym variable) */

	ALLOC_LINK(sym, &kelf->symbols);
	sym->sec = sec;
	sym->sym.st_info = GELF_ST_INFO(STB_LOCAL, STT_SECTION);
	sym->type = STT_SECTION;
	sym->bind = STB_LOCAL;
	sym->name = ".xsplice.strings";
}

void xsplice_build_strings_section_data(struct xsplice_elf *kelf)
{
	struct string *string;
	struct section *sec;
	int size;
	char *strtab;

	sec = find_section_by_name(&kelf->sections, ".xsplice.strings");
	if (!sec)
		ERROR("can't find .xsplice.strings");

	/* determine size */
	size = 0;
	list_for_each_entry(string, &kelf->strings, list)
		size += strlen(string->name) + 1;

	/* allocate section resources */
	strtab = malloc(size);
	if (!strtab)
		ERROR("malloc");
	sec->data->d_buf = strtab;
	sec->data->d_size = size;

	/* populate strings section data */
	list_for_each_entry(string, &kelf->strings, list) {
		strcpy(strtab, string->name);
		strtab += strlen(string->name) + 1;
	}
}

struct section *create_section_pair(struct xsplice_elf *kelf, char *name,
                                    int entsize, int nr)
{
	char *relaname;
	struct section *sec, *relasec;
	int size = entsize * nr;

	relaname = malloc(strlen(name) + strlen(".rela") + 1);
	if (!relaname)
		ERROR("malloc");
	strcpy(relaname, ".rela");
	strcat(relaname, name);

	/* allocate text section resources */
	ALLOC_LINK(sec, &kelf->sections);
	sec->name = name;

	/* set data */
	sec->data = malloc(sizeof(*sec->data));
	if (!sec->data)
		ERROR("malloc");
	sec->data->d_buf = malloc(size);
	if (!sec->data->d_buf)
		ERROR("malloc");
	sec->data->d_size = size;
	sec->data->d_type = ELF_T_BYTE;

	/* set section header */
	sec->sh.sh_type = SHT_PROGBITS;
	sec->sh.sh_entsize = entsize;
	sec->sh.sh_addralign = 8;
	sec->sh.sh_flags = SHF_ALLOC;
	sec->sh.sh_size = size;

	/* allocate rela section resources */
	ALLOC_LINK(relasec, &kelf->sections);
	relasec->name = relaname;
	relasec->base = sec;
	INIT_LIST_HEAD(&relasec->relas);

	/* set data, buffers generated by xsplice_rebuild_rela_section_data() */
	relasec->data = malloc(sizeof(*relasec->data));
	if (!relasec->data)
		ERROR("malloc");

	/* set section header */
	relasec->sh.sh_type = SHT_RELA;
	relasec->sh.sh_entsize = sizeof(GElf_Rela);
	relasec->sh.sh_addralign = 8;

	/* set text rela section pointer */
	sec->rela = relasec;

	return sec;
}

void xsplice_create_patches_sections(struct xsplice_elf *kelf,
                                    struct lookup_table *table, char *hint)
{
	int nr, index;
	struct section *sec, *relasec;
	struct symbol *sym, *strsym;
	struct rela *rela;
	struct lookup_result result;
	struct xsplice_patch_func *funcs;

	/* count patched functions */
	nr = 0;
	list_for_each_entry(sym, &kelf->symbols, list)
		if (sym->type == STT_FUNC && sym->status == CHANGED)
			nr++;

	/* create text/rela section pair */
	sec = create_section_pair(kelf, ".xsplice.funcs", sizeof(*funcs), nr);
	relasec = sec->rela;
	funcs = sec->data->d_buf;

	/* lookup strings symbol */
	strsym = find_symbol_by_name(&kelf->symbols, ".xsplice.strings");
	if (!strsym)
		ERROR("can't find .xsplice.strings symbol");

	/* populate sections */
	index = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type == STT_FUNC && sym->status == CHANGED) {
			if (sym->bind == STB_LOCAL) {
				if (lookup_local_symbol(table, sym->name,
				                        hint, &result))
					ERROR("lookup_local_symbol %s (%s)",
					      sym->name, hint);
			} else {
				if (lookup_global_symbol(table, sym->name,
				                        &result))
					ERROR("lookup_global_symbol %s",
					      sym->name);
			}
			log_debug("lookup for %s @ 0x%016lx len %lu\n",
			          sym->name, result.value, result.size);

			if (result.size < PATCH_INSN_SIZE)
				ERROR("%s too small to patch", sym->name);

			/* add entry in text section */
			funcs[index].old_addr = result.value;
			funcs[index].old_size = result.size;
			funcs[index].new_addr = 0;
			funcs[index].new_size = sym->sym.st_size;

			/*
			 * Add a relocation that will populate
			 * the funcs[index].new_addr field at
			 * module load time.
			 */
			ALLOC_LINK(rela, &relasec->relas);
			rela->sym = sym;
			rela->type = R_X86_64_64;
			rela->addend = 0;
			rela->offset = index * sizeof(*funcs);

			/*
			 * Add a relocation that will populate
			 * the funcs[index].name field.
			 */
			ALLOC_LINK(rela, &relasec->relas);
			rela->sym = strsym;
			rela->type = R_X86_64_64;
			rela->addend = offset_of_string(&kelf->strings, sym->name);
			rela->offset = index * sizeof(*funcs) +
			               offsetof(struct xsplice_patch_func, name);

			index++;
		}
	}

	/* sanity check, index should equal nr */
	if (index != nr)
		ERROR("size mismatch in funcs sections");

}

/* Resolve symbols using xen-syms */
void xsplice_resolve_symbols(struct xsplice_elf *kelf,
                            struct lookup_table *table, char *hint)
{
	struct symbol *sym;
	struct lookup_result result;

	list_for_each_entry(sym, &kelf->symbols, list) {
		/* ignore NULL symbol */
		if (!strlen(sym->name))
			continue;
		if (sym->sec)
			continue;
		if (sym->sym.st_shndx != SHN_UNDEF)
			continue;

		if (sym->bind == STB_LOCAL) {
			if (lookup_local_symbol(table, sym->name,
						hint, &result))
				ERROR("lookup_local_symbol %s (%s)",
				      sym->name, hint);
		} else {
			if (lookup_global_symbol(table, sym->name,
						&result))
				ERROR("lookup_global_symbol %s",
				      sym->name);
		}
		log_debug("lookup for %s @ 0x%016lx len %lu\n",
			  sym->name, result.value, result.size);
		sym->sym.st_value += result.value;
		sym->sym.st_shndx = SHN_ABS;
	}
}

int is_null_sym(struct symbol *sym)
{
	return !strlen(sym->name);
}

int is_file_sym(struct symbol *sym)
{
	return sym->type == STT_FILE;
}

int is_local_func_sym(struct symbol *sym)
{
	return sym->bind == STB_LOCAL && sym->type == STT_FUNC;
}

int is_local_sym(struct symbol *sym)
{
	return sym->bind == STB_LOCAL;
}

void xsplice_reorder_symbols(struct xsplice_elf *kelf)
{
	LIST_HEAD(symbols);

	/* migrate NULL sym */
	xsplice_migrate_symbols(&kelf->symbols, &symbols, is_null_sym);
	/* migrate LOCAL FILE sym */
	xsplice_migrate_symbols(&kelf->symbols, &symbols, is_file_sym);
	/* migrate LOCAL FUNC syms */
	xsplice_migrate_symbols(&kelf->symbols, &symbols, is_local_func_sym);
	/* migrate all other LOCAL syms */
	xsplice_migrate_symbols(&kelf->symbols, &symbols, is_local_sym);
	/* migrate all other (GLOBAL) syms */
	xsplice_migrate_symbols(&kelf->symbols, &symbols, NULL);

	list_replace(&symbols, &kelf->symbols);
}

void xsplice_reindex_elements(struct xsplice_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	int index;

	index = 1; /* elf write function handles NULL section 0 */
	list_for_each_entry(sec, &kelf->sections, list)
		sec->index = index++;

	index = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		sym->index = index++;
		if (sym->sec)
			sym->sym.st_shndx = sym->sec->index;
		else if (sym->sym.st_shndx != SHN_ABS)
			sym->sym.st_shndx = SHN_UNDEF;
	}
}

void xsplice_rebuild_rela_section_data(struct section *sec)
{
	struct rela *rela;
	int nr = 0, index = 0, size;
	GElf_Rela *relas;

	list_for_each_entry(rela, &sec->relas, list)
		nr++;

	size = nr * sizeof(*relas);
	relas = malloc(size);
	if (!relas)
		ERROR("malloc");

	sec->data->d_buf = relas;
	sec->data->d_size = size;
	/* d_type remains ELF_T_RELA */

	sec->sh.sh_size = size;

	list_for_each_entry(rela, &sec->relas, list) {
		relas[index].r_offset = rela->offset;
		relas[index].r_addend = rela->addend;
		relas[index].r_info = GELF_R_INFO(rela->sym->index, rela->type);
		index++;
	}

	/* sanity check, index should equal nr */
	if (index != nr)
		ERROR("size mismatch in rebuilt rela section");
}

void print_strtab(char *buf, size_t size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (buf[i] == 0)
			printf("\\0");
		else
			printf("%c",buf[i]);
	}
}

void xsplice_create_shstrtab(struct xsplice_elf *kelf)
{
	struct section *shstrtab, *sec;
	size_t size, offset, len;
	char *buf;

	shstrtab = find_section_by_name(&kelf->sections, ".shstrtab");
	if (!shstrtab)
		ERROR("find_section_by_name");

	/* determine size of string table */
	size = 1; /* for initial NULL terminator */
	list_for_each_entry(sec, &kelf->sections, list)
		size += strlen(sec->name) + 1; /* include NULL terminator */

	/* allocate data buffer */
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	/* populate string table and link with section header */
	offset = 1;
	list_for_each_entry(sec, &kelf->sections, list) {
		len = strlen(sec->name) + 1;
		sec->sh.sh_name = offset;
		memcpy(buf + offset, sec->name, len);
		offset += len;
	}

	if (offset != size)
		ERROR("shstrtab size mismatch");

	shstrtab->data->d_buf = buf;
	shstrtab->data->d_size = size;

	if (loglevel <= DEBUG) {
		printf("shstrtab: ");
		print_strtab(buf, size);
		printf("\n");

		list_for_each_entry(sec, &kelf->sections, list)
			printf("%s @ shstrtab offset %d\n",
			       sec->name, sec->sh.sh_name);
	}
}

void xsplice_create_strtab(struct xsplice_elf *kelf)
{
	struct section *strtab;
	struct symbol *sym;
	size_t size = 0, offset = 0, len;
	char *buf;

	strtab = find_section_by_name(&kelf->sections, ".strtab");
	if (!strtab)
		ERROR("find_section_by_name");

	/* determine size of string table */
	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type == STT_SECTION)
			continue;
		size += strlen(sym->name) + 1; /* include NULL terminator */
	}

	/* allocate data buffer */
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	/* populate string table and link with section header */
	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type == STT_SECTION) {
			sym->sym.st_name = 0;
			continue;
		}
		len = strlen(sym->name) + 1;
		sym->sym.st_name = offset;
		memcpy(buf + offset, sym->name, len);
		offset += len;
	}

	if (offset != size)
		ERROR("shstrtab size mismatch");

	strtab->data->d_buf = buf;
	strtab->data->d_size = size;

	if (loglevel <= DEBUG) {
		printf("strtab: ");
		print_strtab(buf, size);
		printf("\n");

		list_for_each_entry(sym, &kelf->symbols, list)
			printf("%s @ strtab offset %d\n",
			       sym->name, sym->sym.st_name);
	}
}

void xsplice_create_symtab(struct xsplice_elf *kelf)
{
	struct section *symtab;
	struct symbol *sym;
	char *buf;
	size_t size;
	int nr = 0, offset = 0, nr_local = 0;

	symtab = find_section_by_name(&kelf->sections, ".symtab");
	if (!symtab)
		ERROR("find_section_by_name");

	/* count symbols */
	list_for_each_entry(sym, &kelf->symbols, list)
		nr++;

	/* create new symtab buffer */
	size = nr * symtab->sh.sh_entsize;
	buf = malloc(size);
	if (!buf)
		ERROR("malloc");
	memset(buf, 0, size);

	offset = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		memcpy(buf + offset, &sym->sym, symtab->sh.sh_entsize);
		offset += symtab->sh.sh_entsize;

		if (is_local_sym(sym))
			nr_local++;
	}

	symtab->data->d_buf = buf;
	symtab->data->d_size = size;

	/* update symtab section header */
	symtab->sh.sh_link = find_section_by_name(&kelf->sections, ".strtab")->index;
	symtab->sh.sh_info = nr_local;
}

void xsplice_write_output_elf(struct xsplice_elf *kelf, Elf *elf, char *outfile)
{
	int fd;
	struct section *sec;
	Elf *elfout;
	GElf_Ehdr eh, ehout;
	Elf_Scn *scn;
	Elf_Data *data;
	GElf_Shdr sh;

	/* TODO make this argv */
	fd = creat(outfile, 0777);
	if (fd == -1)
		ERROR("creat");

	elfout = elf_begin(fd, ELF_C_WRITE, NULL);
	if (!elfout)
		ERROR("elf_begin");

	if (!gelf_newehdr(elfout, gelf_getclass(kelf->elf)))
		ERROR("gelf_newehdr");

	if (!gelf_getehdr(elfout, &ehout))
		ERROR("gelf_getehdr");

	if (!gelf_getehdr(elf, &eh))
		ERROR("gelf_getehdr");

	memset(&ehout, 0, sizeof(ehout));
	ehout.e_ident[EI_DATA] = eh.e_ident[EI_DATA];
	ehout.e_machine = eh.e_machine;
	ehout.e_type = eh.e_type;
	ehout.e_version = EV_CURRENT;
	ehout.e_shstrndx = find_section_by_name(&kelf->sections, ".shstrtab")->index;

	/* add changed sections */
	list_for_each_entry(sec, &kelf->sections, list) {
		scn = elf_newscn(elfout);
		if (!scn)
			ERROR("elf_newscn");

		data = elf_newdata(scn);
		if (!data)
			ERROR("elf_newdata");

		if (!elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY))
			ERROR("elf_flagdata");

		data->d_type = sec->data->d_type;
		data->d_buf = sec->data->d_buf;
		data->d_size = sec->data->d_size;

		if(!gelf_getshdr(scn, &sh))
			ERROR("gelf_getshdr");

		sh = sec->sh;

		if (!gelf_update_shdr(scn, &sh))
			ERROR("gelf_update_shdr");	
	}

	if (!gelf_update_ehdr(elfout, &ehout))
		ERROR("gelf_update_ehdr");

	if (elf_update(elfout, ELF_C_WRITE) < 0) {
		printf("%s\n",elf_errmsg(-1));
		ERROR("elf_update");
	}
}

struct arguments {
	char *args[4];
	int debug;
};

static char args_doc[] = "original.o patched.o kernel-object output.o";

static struct argp_option options[] = {
	{"debug", 'd', 0, 0, "Show debug output" },
	{ 0 }
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	/* Get the input argument from argp_parse, which we
	   know is a pointer to our arguments structure. */
	struct arguments *arguments = state->input;

	switch (key)
	{
		case 'd':
			arguments->debug = 1;
			break;
		case ARGP_KEY_ARG:
			if (state->arg_num >= 4)
				/* Too many arguments. */
				argp_usage (state);
			arguments->args[state->arg_num] = arg;
			break;
		case ARGP_KEY_END:
			if (state->arg_num < 4)
				/* Not enough arguments. */
				argp_usage (state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, 0 };

int main(int argc, char *argv[])
{
	struct xsplice_elf *kelf_base, *kelf_patched, *kelf_out;
	struct arguments arguments;
	int num_changed, new_globals_exist;
	struct lookup_table *lookup;
	struct section *sec, *symtab;
	struct symbol *sym;
	char *hint = NULL;

	arguments.debug = 0;
	argp_parse (&argp, argc, argv, 0, 0, &arguments);
	if (arguments.debug)
		loglevel = DEBUG;

	elf_version(EV_CURRENT);

	childobj = basename(arguments.args[0]);

	log_debug("Open base\n");
	kelf_base = xsplice_elf_open(arguments.args[0]);
	log_debug("Open patched\n");
	kelf_patched = xsplice_elf_open(arguments.args[1]);

	log_debug("Compare elf headers\n");
	xsplice_compare_elf_headers(kelf_base->elf, kelf_patched->elf);
	log_debug("Check program headers of base\n");
	xsplice_check_program_headers(kelf_base->elf);
	log_debug("Check program headers of patched\n");
	xsplice_check_program_headers(kelf_patched->elf);

	log_debug("Mark grouped sections\n");
	xsplice_mark_grouped_sections(kelf_patched);
	log_debug("Replace sections syms base\n");
	xsplice_replace_sections_syms(kelf_base);
	log_debug("Replace sections syms patched\n");
	xsplice_replace_sections_syms(kelf_patched);
	log_debug("Rename mangled functions\n");
	xsplice_rename_mangled_functions(kelf_base, kelf_patched);

	log_debug("Correlate elfs\n");
	xsplice_correlate_elfs(kelf_base, kelf_patched);
	log_debug("Correlate static local variables\n");
	xsplice_correlate_static_local_variables(kelf_base, kelf_patched);

	/*
	 * After this point, we don't care about kelf_base anymore.
	 * We access its sections via the twin pointers in the
	 * section, symbol, and rela lists of kelf_patched.
	 */
	log_debug("Compare correlated elements\n");
	xsplice_compare_correlated_elements(kelf_patched);
	log_debug("Elf teardown base\n");
	xsplice_elf_teardown(kelf_base);
	log_debug("Elf free base\n");
	xsplice_elf_free(kelf_base);

	log_debug("Mark ignored sections same\n");
	xsplice_mark_ignored_sections_same(kelf_patched);
	log_debug("Mark constant labels same\n");
	xsplice_mark_constant_labels_same(kelf_patched);

	log_debug("Include standard elements\n");
	xsplice_include_standard_elements(kelf_patched);
	log_debug("Include changed functions\n");
	num_changed = xsplice_include_changed_functions(kelf_patched);
	log_debug("num_changed = %d\n", num_changed);
	log_debug("Include debug sections\n");
	xsplice_include_debug_sections(kelf_patched);
	log_debug("Include new globals\n");
	new_globals_exist = xsplice_include_new_globals(kelf_patched);
	log_debug("new_globals_exist = %d\n", new_globals_exist);

	log_debug("Print changes\n");
	xsplice_print_changes(kelf_patched);
	log_debug("Dump patched elf status\n");
	xsplice_dump_kelf(kelf_patched);

	if (!num_changed && !new_globals_exist) {
		log_debug("no changed functions were found\n");
		return 3; /* 1 is ERROR, 2 is DIFF_FATAL */
	}

	log_debug("Process special sections\n");
	xsplice_process_special_sections(kelf_patched);
	log_debug("Verify patchability\n");
	xsplice_verify_patchability(kelf_patched);

	/* this is destructive to kelf_patched */
	log_debug("Migrate included elements\n");
	xsplice_migrate_included_elements(kelf_patched, &kelf_out);

	/*
	 * Teardown kelf_patched since we shouldn't access sections or symbols
	 * through it anymore.  Don't free however, since our section and symbol
	 * name fields still point to strings in the Elf object owned by
	 * xsplice_patched.
	 */
	log_debug("Elf teardown patched\n");
	xsplice_elf_teardown(kelf_patched);

	log_debug("Search for source file name\n");
	list_for_each_entry(sym, &kelf_out->symbols, list) {
		if (sym->type == STT_FILE) {
			hint = sym->name;
			break;
		}
	}
	if (!hint)
		ERROR("FILE symbol not found in output. Stripped?\n");
	log_debug("hint = %s\n", hint);

	/* create symbol lookup table */
	log_debug("Lookup xen-syms\n");
	lookup = lookup_open(arguments.args[2]);

	/* create strings, patches, and dynrelas sections */
	log_debug("Create strings elements\n");
	xsplice_create_strings_elements(kelf_out);
	log_debug("Create patches sections\n");
	xsplice_create_patches_sections(kelf_out, lookup, hint);
	/* log_debug("Resolve symbols\n"); */
	/* xsplice_resolve_symbols(kelf_out, lookup, hint); */
	xsplice_build_strings_section_data(kelf_out);

	/*
	 *  At this point, the set of output sections and symbols is
	 *  finalized.  Reorder the symbols into linker-compliant
	 *  order and index all the symbols and sections.  After the
	 *  indexes have been established, update index data
	 *  throughout the structure.
	 */
	log_debug("Reorder symbols\n");
	xsplice_reorder_symbols(kelf_out);
	log_debug("Reindex elements\n");
	xsplice_reindex_elements(kelf_out);

	/*
	 * Update rela section headers and rebuild the rela section data
	 * buffers from the relas lists.
	 */
	symtab = find_section_by_name(&kelf_out->sections, ".symtab");
	list_for_each_entry(sec, &kelf_out->sections, list) {
		if (!is_rela_section(sec))
			continue;
		sec->sh.sh_link = symtab->index;
		sec->sh.sh_info = sec->base->index;
		log_debug("Rebuild rela section data for %s\n", sec->name);
		xsplice_rebuild_rela_section_data(sec);
	}

	log_debug("Create shstrtab\n");
	xsplice_create_shstrtab(kelf_out);
	log_debug("Create strtab\n");
	xsplice_create_strtab(kelf_out);
	log_debug("Create symtab\n");
	xsplice_create_symtab(kelf_out);
	log_debug("Dump out elf status\n");
	xsplice_dump_kelf(kelf_out);
	log_debug("Write out elf\n");
	xsplice_write_output_elf(kelf_out, kelf_patched->elf, arguments.args[3]);

	log_debug("Elf free patched\n");
	xsplice_elf_free(kelf_patched);
	log_debug("Elf teardown out\n");
	xsplice_elf_teardown(kelf_out);
	log_debug("Elf free out\n");
	xsplice_elf_free(kelf_out);

	return 0;
}