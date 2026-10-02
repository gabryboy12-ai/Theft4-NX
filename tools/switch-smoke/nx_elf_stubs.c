/*
 * libelf for Mesa's libvulkan.a (mesa-switch): nv_cubin.c parses NVIDIA CUBIN
 * files with libelf, which devkitPro does not ship and which never runs on
 * Switch. These definitions only satisfy the linker; elf_version returning 0
 * ("unsupported version") makes a caller give up cleanly if one ever runs.
 * nfsmw-nx does the same (sdk/src/ui/switch_elf_stubs.c).
 */
#include <stddef.h>

unsigned int elf_version(unsigned int v) { (void)v; return 0; }
void* elf_memory(char* image, size_t size) { (void)image; (void)size; return NULL; }
int elf_end(void* elf) { (void)elf; return 0; }
int elf_kind(void* elf) { (void)elf; return 0; }
void* elf64_getehdr(void* elf) { (void)elf; return NULL; }
void* elf64_getshdr(void* scn) { (void)scn; return NULL; }
void* elf_getscn(void* elf, size_t index) { (void)elf; (void)index; return NULL; }
void* elf_nextscn(void* elf, void* scn) { (void)elf; (void)scn; return NULL; }
void* elf_getdata(void* scn, void* data) { (void)scn; (void)data; return NULL; }
int elf_getshdrstrndx(void* elf, size_t* out) { (void)elf; if (out) *out = 0; return -1; }
char* elf_strptr(void* elf, size_t section, size_t offset) {
  (void)elf; (void)section; (void)offset; return NULL;
}
int elf_errno(void) { return 0; }
const char* elf_errmsg(int error) { (void)error; return "libelf is not available on Switch"; }
