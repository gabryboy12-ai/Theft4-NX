#include <rex/ppc/function.h>
#include <cstring>
#include <cstdlib>

// Define missing CRT hooks
extern "C" {

// ---------------------------------------------------------
// String & Memory hooks
// ---------------------------------------------------------

static void* native_memcpy(void* dst, const void* src, size_t n) { return std::memcpy(dst, src, n); }
static void* native_memset(void* dst, int val, size_t n) { return std::memset(dst, val, n); }
static void* native_XMemCpy(void* dst, const void* src, size_t n) { return std::memcpy(dst, src, n); }
static char* native_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
static char* native_strstr(const char* s, const char* find) { return const_cast<char*>(std::strstr(s, find)); }
static char* native_strtok(char* s, const char* delim) { return std::strtok(s, delim); }
static int native_strncmp(const char* s1, const char* s2, size_t n) { return std::strncmp(s1, s2, n); }
static char* native_strncpy(char* dest, const char* src, size_t n) { return std::strncpy(dest, src, n); }
static char* native_strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
static int native__stricmp(const char* s1, const char* s2) { return strcasecmp(s1, s2); }

REXCRT_EXPORT(rexcrt_memcpy, native_memcpy)
REXCRT_EXPORT(rexcrt_memset, native_memset)
REXCRT_EXPORT(rexcrt_XMemCpy, native_XMemCpy)
REXCRT_EXPORT(rexcrt_strchr, native_strchr)
REXCRT_EXPORT(rexcrt_strstr, native_strstr)
REXCRT_EXPORT(rexcrt_strtok, native_strtok)
REXCRT_EXPORT(rexcrt_strncmp, native_strncmp)
REXCRT_EXPORT(rexcrt_strncpy, native_strncpy)
REXCRT_EXPORT(rexcrt_strrchr, native_strrchr)
REXCRT_EXPORT(rexcrt__stricmp, native__stricmp)

// ---------------------------------------------------------
// Heap hooks
// ---------------------------------------------------------

static void* native_RtlAllocateHeap(void* heap, uint32_t flags, size_t size) { return std::malloc(size); }
static int native_RtlFreeHeap(void* heap, uint32_t flags, void* ptr) { std::free(ptr); return 1; }
static void* native_RtlReAllocateHeap(void* heap, uint32_t flags, void* ptr, size_t size) { return std::realloc(ptr, size); }
static size_t native_RtlSizeHeap(void* heap, uint32_t flags, const void* ptr) { return 0; } // no-op

REXCRT_EXPORT(rexcrt_RtlAllocateHeap, native_RtlAllocateHeap)
REXCRT_EXPORT(rexcrt_RtlFreeHeap, native_RtlFreeHeap)
REXCRT_EXPORT(rexcrt_RtlReAllocateHeap, native_RtlReAllocateHeap)
REXCRT_EXPORT(rexcrt_RtlSizeHeap, native_RtlSizeHeap)

// ---------------------------------------------------------
// FS Stub hooks
// ---------------------------------------------------------

REXCRT_EXPORT_STUB(rexcrt_CloseHandle)
REXCRT_EXPORT_STUB(rexcrt_CreateDirectoryA)
REXCRT_EXPORT_STUB(rexcrt_CreateFileA)
REXCRT_EXPORT_STUB(rexcrt_DeleteFileA)
REXCRT_EXPORT_STUB(rexcrt_FindFirstFileA)
REXCRT_EXPORT_STUB(rexcrt_FindNextFileA)
REXCRT_EXPORT_STUB(rexcrt_FlushFileBuffers)
REXCRT_EXPORT_STUB(rexcrt_GetFileAttributesA)
REXCRT_EXPORT_STUB(rexcrt_GetFileSize)
REXCRT_EXPORT_STUB(rexcrt_GetFileSizeEx)
REXCRT_EXPORT_STUB(rexcrt_MoveFileA)
REXCRT_EXPORT_STUB(rexcrt_ReadFile)
REXCRT_EXPORT_STUB(rexcrt_RemoveDirectoryA)
REXCRT_EXPORT_STUB(rexcrt_SetEndOfFile)
REXCRT_EXPORT_STUB(rexcrt_SetFileAttributesA)
REXCRT_EXPORT_STUB(rexcrt_SetFilePointer)
REXCRT_EXPORT_STUB(rexcrt_SetFileTime)
REXCRT_EXPORT_STUB(rexcrt_WriteFile)

} // extern "C"
