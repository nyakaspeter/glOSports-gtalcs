/* so_util.c -- map an Android .so into this process, relocate and hook it.
 * After the Vita loaders' so_util (Andy Nguyen, MIT): the segments go into one
 * anonymous mapping, the dynamic section gives the tables, relocations are
 * the three kinds an NDK r8 .so has. */
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "so_util.h"
#include "util.h"

uintptr_t so_base;
size_t so_span;
uintptr_t so_exidx;
size_t so_exidx_count;

uintptr_t so_text_end; /* end of the executable segment, page-aligned */
static Elf32_Sym *syms;
static const char *strtab;
static const uint32_t *hashtab;
static Elf32_Rel *rel, *jmprel;
static size_t relsz, jmprelsz;
static uintptr_t *init_array;
static size_t init_array_count;

#define PAGE_ALIGN(x) (((x) + 0xfff) & ~(uintptr_t)0xfff)

int so_load(const char *filename)
{
	int fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -1;
	struct stat st;
	fstat(fd, &st);
	uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file == MAP_FAILED)
		return -1;

	Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_ARM)
		return -2;
	Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff);

	uintptr_t span = 0;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span)
			span = ph[i].p_vaddr + ph[i].p_memsz;
	so_span = PAGE_ALIGN(span);
	void *base = mmap(NULL, so_span, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return -3;
	so_base = (uintptr_t)base;

	Elf32_Dyn *dyn = NULL;
	for (int i = 0; i < eh->e_phnum; i++) {
		switch (ph[i].p_type) {
		case PT_LOAD:
			memcpy((void *)(so_base + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
			if (ph[i].p_flags & PF_X)
				so_text_end = PAGE_ALIGN(so_base + ph[i].p_vaddr + ph[i].p_memsz);
			break;
		case PT_DYNAMIC:
			dyn = (Elf32_Dyn *)(so_base + ph[i].p_vaddr);
			break;
		case PT_ARM_EXIDX:
			so_exidx = so_base + ph[i].p_vaddr;
			so_exidx_count = ph[i].p_memsz / 8;
			break;
		}
	}
	munmap(file, st.st_size);
	if (!dyn)
		return -4;

	for (; dyn->d_tag != DT_NULL; dyn++) {
		uintptr_t p = so_base + dyn->d_un.d_ptr;
		switch (dyn->d_tag) {
		case DT_SYMTAB: syms = (Elf32_Sym *)p; break;
		case DT_STRTAB: strtab = (const char *)p; break;
		case DT_HASH: hashtab = (const uint32_t *)p; break;
		case DT_REL: rel = (Elf32_Rel *)p; break;
		case DT_RELSZ: relsz = dyn->d_un.d_val / sizeof(Elf32_Rel); break;
		case DT_JMPREL: jmprel = (Elf32_Rel *)p; break;
		case DT_PLTRELSZ: jmprelsz = dyn->d_un.d_val / sizeof(Elf32_Rel); break;
		case DT_INIT_ARRAY: init_array = (uintptr_t *)p; break;
		case DT_INIT_ARRAYSZ: init_array_count = dyn->d_un.d_val / 4; break;
		}
	}
	if (!syms || !strtab || !hashtab)
		return -5;
	return 0;
}

static int relocate(Elf32_Rel *rels, size_t count, uintptr_t (*resolve)(const char *))
{
	int missing = 0;
	for (size_t i = 0; i < count; i++) {
		uintptr_t *ptr = (uintptr_t *)(so_base + rels[i].r_offset);
		Elf32_Sym *sym = &syms[ELF32_R_SYM(rels[i].r_info)];
		switch (ELF32_R_TYPE(rels[i].r_info)) {
		case R_ARM_RELATIVE:
			*ptr += so_base;
			break;
		case R_ARM_ABS32:
			if (sym->st_shndx != SHN_UNDEF) {
				*ptr += so_base + sym->st_value;
				break;
			}
			/* fall through: an imported address */
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT: {
			if (sym->st_shndx != SHN_UNDEF) {
				*ptr = so_base + sym->st_value;
				break;
			}
			const char *name = strtab + sym->st_name;
			uintptr_t f = resolve(name);
			if (f) {
				*ptr = f;
			} else {
				/* imports from the Android libraries the folder has no
				 * counterpart for (Social Club, Rockstar cloud, OpenSL):
				 * a variable reads zero, a function returns 0 */
				if (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT)
					*ptr = (uintptr_t)calloc(1, sym->st_size > 64 ? sym->st_size : 64);
				else
					*ptr = (uintptr_t)ret0;
				log_printf("so: %s not provided, %s\n", name,
					ELF32_ST_TYPE(sym->st_info) == STT_OBJECT ? "zeroed" : "returns 0");
				missing++;
			}
			break;
		}
		default:
			log_printf("so: unknown relocation type %d\n", ELF32_R_TYPE(rels[i].r_info));
			break;
		}
	}
	return missing;
}

int so_resolve(uintptr_t (*resolve)(const char *name))
{
	int missing = relocate(rel, relsz, resolve);
	missing += relocate(jmprel, jmprelsz, resolve);
	return missing;
}

int so_symbol_count(void)
{
	return hashtab[1];
}

const char *so_function_name(int i, uintptr_t *addr)
{
	if (syms[i].st_shndx == SHN_UNDEF || syms[i].st_name == 0 || ELF32_ST_TYPE(syms[i].st_info) != STT_FUNC)
		return NULL;
	*addr = so_base + syms[i].st_value;
	return strtab + syms[i].st_name;
}

void so_make_text_writable(int writable)
{
	mprotect((void *)so_base, so_text_end - so_base,
		PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0));
}

void so_flush_caches(void)
{
	__builtin___clear_cache((char *)so_base, (char *)so_text_end);
}

void so_execute_init_array(void)
{
	for (size_t i = 0; i < init_array_count; i++)
		if (init_array[i] != 0 && init_array[i] != (uintptr_t)-1)
			((void (*)(void))init_array[i])();
}

static uint32_t elf_hash(const char *name)
{
	uint32_t h = 0, g;
	while (*name) {
		h = (h << 4) + (uint8_t)*name++;
		g = h & 0xf0000000;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

uintptr_t so_symbol(const char *name)
{
	uint32_t nbucket = hashtab[0];
	const uint32_t *bucket = &hashtab[2], *chain = &hashtab[2 + nbucket];
	for (uint32_t i = bucket[elf_hash(name) % nbucket]; i != 0; i = chain[i])
		if (syms[i].st_shndx != SHN_UNDEF && strcmp(strtab + syms[i].st_name, name) == 0)
			return so_base + syms[i].st_value;
	return 0;
}

uintptr_t so_find_addr(const char *name)
{
	uintptr_t a = so_symbol(name);
	if (!a)
		fatal_error("%s has no symbol %s: it must be the supported version (README.md)", game_so_name, name);
	return a;
}

void hook_thumb(uintptr_t addr, uintptr_t dst)
{
	if (addr == 0)
		return;
	addr &= ~1;
	if (addr & 2) {
		*(uint16_t *)addr = 0xbf00; /* NOP, so the LDR below is word-aligned */
		addr += 2;
	}
	uint32_t *p = (uint32_t *)addr;
	p[0] = 0xf000f8df; /* LDR PC, [PC] */
	p[1] = dst;
}

void hook_addr(uintptr_t addr, uintptr_t dst)
{
	if (addr & 1)
		hook_thumb(addr, dst);
	else
		hook_arm(addr, dst);
}

void so_patch(uintptr_t off, const void *data, size_t len)
{
	memcpy((void *)(so_base + off), data, len);
}

void hook_arm(uintptr_t addr, uintptr_t dst)
{
	if (addr == 0)
		return;
	uint32_t *p = (uint32_t *)(addr & ~3);
	p[0] = 0xe51ff004; /* LDR PC, [PC, #-4] */
	p[1] = dst;
}
