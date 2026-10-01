#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OMNIMESH_PLUGIN_API_VERSION 1u
typedef enum omni_status_t { OMNI_STATUS_OK=0, OMNI_STATUS_INVALID_ARGUMENT=1, OMNI_STATUS_UNAVAILABLE=2, OMNI_STATUS_NOT_IMPLEMENTED=3, OMNI_STATUS_INTERNAL=4 } omni_status_t;
typedef void* (*omni_alloc_fn)(void* user, uint64_t size);
typedef void (*omni_free_fn)(void* user, void* ptr);
typedef struct omni_plugin_context { uint32_t api_version; void* user; omni_alloc_fn allocate; omni_free_fn deallocate; } omni_plugin_context;
typedef struct omni_plugin_descriptor { uint32_t api_version; const char* name; const char* version; uint64_t capabilities; } omni_plugin_descriptor;
/* Plugin owns returned strings; host must use context deallocate for buffers it receives. No exceptions cross this ABI. */
omni_status_t omni_plugin_init(const omni_plugin_context* context, omni_plugin_descriptor* descriptor);
#ifdef __cplusplus
}
#endif
