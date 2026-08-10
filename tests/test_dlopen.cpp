// Verifies the built .wfx loads with dlopen and exposes every export from adbfsplugin.def
#include <dlfcn.h>
#include <cstdio>

static const char* kExports[] = {
    "FsInit", "FsInitW", "FsFindFirst", "FsFindFirstW", "FsFindNext", "FsFindNextW",
    "FsFindClose", "FsMkDir", "FsMkDirW", "FsExecuteFile", "FsExecuteFileW",
    "FsRenMovFile", "FsRenMovFileW", "FsGetFile", "FsGetFileW", "FsPutFile",
    "FsPutFileW", "FsDeleteFile", "FsDeleteFileW", "FsRemoveDir", "FsRemoveDirW",
    "FsSetAttr", "FsSetAttrW", "FsSetTime", "FsSetTimeW", "FsStatusInfo",
    "FsGetDefRootName", "FsExtractCustomIcon", "FsExtractCustomIconW",
    "FsSetDefaultParams", "FsGetPreviewBitmap", "FsGetPreviewBitmapW",
    "FsContentGetSupportedField", "FsContentGetValue", "FsContentGetValueW",
    "FsContentGetSupportedFieldFlags", "FsContentGetDefaultSortOrder",
    "FsContentGetDefaultView", "FsContentSetValue", "FsContentSetValueW",
    "FsContentPluginUnloading", "FsDisconnect", "FsDisconnectW",
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build/adbfsplugin.wfx";
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
        return 1;
    }
    int missing = 0;
    for (const char* name : kExports) {
        if (!dlsym(h, name)) {
            std::fprintf(stderr, "missing export: %s\n", name);
            missing++;
        }
    }
    if (missing) {
        std::fprintf(stderr, "%d export(s) missing from %s\n", missing, path);
        return 1;
    }
    std::printf("OK: %zu exports resolved in %s\n", sizeof(kExports) / sizeof(kExports[0]), path);
    return 0;
}
