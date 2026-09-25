// Stand-in libvulkan.so for Dawn: forwards vkGetInstanceProcAddr to the libadrenotools-loaded
// loader, which lives in a linker namespace Dawn's own dlopen cannot reach.

#include <stddef.h>

typedef void (*DuskVkVoidFunction)(void);
typedef DuskVkVoidFunction (*DuskVkGetInstanceProcAddr)(void* instance, const char* name);

static DuskVkGetInstanceProcAddr g_getInstanceProcAddr = NULL;

__attribute__((visibility("default"))) void dusk_vulkan_shim_set_proc_addr(
    DuskVkGetInstanceProcAddr proc) {
    g_getInstanceProcAddr = proc;
}

__attribute__((visibility("default"))) DuskVkVoidFunction vkGetInstanceProcAddr(
    void* instance, const char* name) {
    if (g_getInstanceProcAddr == NULL) {
        return NULL;
    }
    return g_getInstanceProcAddr(instance, name);
}
