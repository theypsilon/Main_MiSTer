#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "profiling.h"
#include "scheduler.h"
#include "osd.h"
#include "file_io.h"
#include "user_io.h"
#include "menu.h"

#define MAX_PLUGINS 8
#define PLUGIN_API_VERSION 1

// Firmware API for Plugins, so that the plugins can use functions from the firmware declared here.

void firm_api_scheduler_yield() {
    #ifdef USE_SCHEDULER
        scheduler_yield();
    #endif
}

void firm_api_OsdSetTitle(const char *s, int a) {
    OsdSetTitle(s, a);
}

void firm_api_OsdClear() {
    OsdClear();
}

void firm_api_OsdEnable(unsigned char mode) {
    OsdEnable(mode);
}

int firm_api_OsdGetSize() {
    return OsdGetSize();
}

void firm_api_OsdWrite(unsigned char n, const char *s, unsigned char invert, unsigned char stipple, char usebg, int maxinv, int mininv) {
    OsdWrite(n, s, invert, stipple, usebg, maxinv, mininv);
}

void firm_api_OsdUpdate() {
    OsdUpdate();
}

void firm_api_SelectFile(const char* path, const char* pFileExt, int Options, unsigned char MenuSelect, unsigned char MenuCancel) {
    SelectFile(path, pFileExt, Options, MenuSelect, MenuCancel);
}

void firm_api_FileCreatePath(const char *dir) {
    FileCreatePath(dir);
}

void firm_api_MakeFile(const char *filename, const char *data) {
    MakeFile(filename, data);
}

struct FirmwareAPI {
    int version = PLUGIN_API_VERSION;
    void (*scheduler_yield)() = firm_api_scheduler_yield;
    void (*OsdSetTitle)(const char *s, int a) = firm_api_OsdSetTitle;
    void (*OsdClear)() = firm_api_OsdClear;
    void (*OsdEnable)(unsigned char mode) = firm_api_OsdEnable;
    int (*OsdGetSize)() = firm_api_OsdGetSize;
    void (*OsdWrite)(unsigned char n, const char *s, unsigned char invert, unsigned char stipple, char usebg, int maxinv, int mininv) = firm_api_OsdWrite;
    void (*OsdUpdate)() = firm_api_OsdUpdate;
    void (*SelectFile)(const char* path, const char* pFileExt, int Options, unsigned char MenuSelect, unsigned char MenuCancel) = firm_api_SelectFile;
    void (*FileCreatePath)(const char *dir) = firm_api_FileCreatePath;
    void (*MakeFile)(const char *filename, const char *data) = firm_api_MakeFile;
};

// Internals

struct PluginHookData {
    const char* name;
    int* count;
    void** arr;
    int limit;
};

#define MAX_PLUGIN_HOOKS 64
PluginHookData plugin_hooks[MAX_PLUGIN_HOOKS];

struct HookRegistrar {
    HookRegistrar(int id, const char* name, int* count, void** arr, int limit) {
        if (id < MAX_PLUGIN_HOOKS) {
            plugin_hooks[id] = {name, count, arr, limit};
        }
    }
};

#define DEFINE_PLUGIN_API_HOOK(id, hook_name, max_instances, param_decl, param_use) \
    typedef void (*__##hook_name##__func_t) param_decl; \
    static __##hook_name##__func_t __##hook_name##__arr[max_instances] = {nullptr}; \
    static int __##hook_name##__count = 0; \
    int hook_name param_decl { \
        for (int i = 0; i < __##hook_name##__count; ++i) { \
            __##hook_name##__arr[i] param_use; \
        } \
        return __##hook_name##__count; \
    } \
    static HookRegistrar __##hook_name##__registrar(id, #hook_name, &__##hook_name##__count, \
                                             reinterpret_cast<void**>(__##hook_name##__arr), max_instances);

struct HookPair {
    int id;
    void* ptr;
};

void load_hook(HookPair hook) {
    PluginHookData* hook_data = &plugin_hooks[hook.id];
    if (hook_data->name == nullptr) {
        fprintf(stderr, "hoook id# %d not registered.", hook.id);
        return;
    }
    if (*hook_data->count >= hook_data->limit) {
        fprintf(stderr, "%s() registered too many instances: %d\n", hook_data->name, *hook_data->count);
        return;
    }
    hook_data->arr[*hook_data->count] = hook.ptr;
    (*hook_data->count)++;
}

void load_single_plugin(const char* path) {
    void *plugin = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!plugin) {
        fprintf(stderr, "Failed to load plugin: %s\n", dlerror());
        return;
    }

    auto describe_hooks = reinterpret_cast<HookPair*(*)(FirmwareAPI*, int*)>(dlsym(plugin, "describe_hooks"));
    if (!describe_hooks) {
        fprintf(stderr, "Plugin %s| describe_hooks() not found: %s\n", path, dlerror());
        return;
    }

    int hooks_count = 0;
    auto api = FirmwareAPI{};
    HookPair* hooks = describe_hooks(&api, &hooks_count);
    if (hooks == nullptr || hooks_count <= 0 || hooks_count > MAX_PLUGIN_HOOKS) {
        fprintf(stderr, "Plugin %s| describe_hooks() did not return a valid hooks array: %d\n", path, hooks_count);
        return;
    }
    int loaded = 0;
    for (int i = 0; i < hooks_count; i++) {
        if (hooks[i].id < 0 || hooks[i].id >= MAX_PLUGIN_HOOKS) {
            fprintf(stderr, "Plugin %s| Hooks array entry '%d' did contain incorrect value: %d\n", path, i, hooks[i].id);
            continue;
        }
        load_hook(hooks[i]);
        loaded ++;
    }
    fprintf(stderr, "Plugin %s loaded with %d hooks.\n", path, loaded);
}

// Loads all the plugins during initialization

void load_plugins() {
    SPIKE_FUNCTION(16000);
    fprintf(stderr, "Loading plugins.\n");

    char dir_path[2100];
    sprintf(dir_path, "%s/linux/plugins", getRootDir());
    DIR *d = opendir(dir_path);
    if (!d)
    {
        fprintf(stderr, "Couldn't load plugins because couldn't open dir: %s\n", dir_path);
        return;
    }

    struct dirent *de;
    int loaded = 0;

    while ((de = readdir(d)) != nullptr && loaded < MAX_PLUGINS) {
        if (de->d_type != DT_REG ||
            de->d_name[0] == '.' ||
            strchr(de->d_name, '/')) {
            continue;
        }

        size_t len = strlen(de->d_name);
        if (len > 3 && strncasecmp(de->d_name + len - 3, ".so", 3) == 0) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
            
            load_single_plugin(full_path);
            loaded++;
        }
    }
    if (closedir(d) == -1) {
        perror("closedir");
    }
}

// Plugin API: Hooks to connect with the plugins. So these are the plugin functions we call from the firmware.

DEFINE_PLUGIN_API_HOOK(0, plugin_handle_mister_cmd, 5, (char* cmd), (cmd))
DEFINE_PLUGIN_API_HOOK(1, plugin_test, 1, (), ())
