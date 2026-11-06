/* so_util.h -- map an Android .so into this process, relocate and hook it.
 * After the Vita loaders' so_util (Andy Nguyen, MIT), with mmap/mprotect instead of
 * kubridge. */
#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *symbol;
	uintptr_t func;
} DynLibFunction;

extern uintptr_t so_base;
extern size_t so_span;
extern uintptr_t so_text_end; /* end of the executable segment */
extern uintptr_t so_exidx;
extern size_t so_exidx_count;

int so_load(const char *filename);
/* Fills the .so's GOT through resolve(name); names it returns 0 for are
 * listed on stderr and point to an address that faults with the GOT slot as
 * PC. */
int so_resolve(uintptr_t (*resolve)(const char *name));
/* The .so's own functions, for walking its symbols: name, or NULL for a
 * symbol that is not a function the .so defines. */
int so_symbol_count(void);
const char *so_function_name(int i, uintptr_t *addr);
void so_make_text_writable(int writable);
void so_flush_caches(void);
void so_execute_init_array(void);
/* Address of a symbol the .so defines, 0 if there is none. */
uintptr_t so_symbol(const char *name);
/* Same, but a missing symbol is fatal (wrong libMaxPayne.so version). */
uintptr_t so_find_addr(const char *name);

/* Overwrite the start of a Thumb / ARM function with a jump to dst. */
void hook_thumb(uintptr_t addr, uintptr_t dst);
void hook_arm(uintptr_t addr, uintptr_t dst);
/* Either, by the address's low bit (set: Thumb), as symbols carry it. */
void hook_addr(uintptr_t addr, uintptr_t dst);
#define hook_sym(name, dst) hook_addr(so_find_addr(name), (uintptr_t)(dst))
/* By offset in the .so, for code at an address known only for the one
 * supported version of the game: odd = Thumb, even = ARM, as above. */
#define hook_off(off, dst) hook_addr(so_base + (off), (uintptr_t)(dst))
/* Overwrite len bytes at an offset in the .so. */
void so_patch(uintptr_t off, const void *data, size_t len);

#endif
