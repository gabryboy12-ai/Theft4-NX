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

/*
 * vkDestroyInstance for an NVK archive that exports only vkGetInstanceProcAddr
 * (jhuz0/nvk-switch localizes every other entry point). vulkan_instance.cpp and
 * vulkan_probe.cpp link vkDestroyInstance directly, as mesa-switch exports it.
 * Weak, so an archive that does export it wins; the pointer comes from the
 * driver's own dispatch, never from this function.
 */
typedef struct VkInstance_T* VkInstance;
typedef void (*PFN_vkVoidFunction)(void);
typedef void (*PFN_vkDestroyInstance)(VkInstance instance, const void* allocator);
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name);

__attribute__((weak)) void vkDestroyInstance(VkInstance instance, const void* allocator) {
  PFN_vkDestroyInstance destroy =
      (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
  if (destroy) destroy(instance, allocator);
}
