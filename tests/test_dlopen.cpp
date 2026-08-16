// Verifies the built .wfx passes Double Commander's plugin validity check
// (thin Mach-O magic — DC's GetPluginBinaryType rejects universal/fat binaries
// as "This is not a valid plugin!"), loads with dlopen, and exposes every
// WFX entry point listed in kExports below.
// Usage: test_dlopen [--magic-only] <path.wfx>
#include <dlfcn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

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

// Double Commander (udefaultplugins.pas GetPluginBinaryType) accepts ONLY thin
// Mach-O magics; a fat/universal binary (0xCAFEBABE big-endian) is btUnknown.
static bool checkThinMacho64(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    uint32_t magic = 0;
    size_t n = std::fread(&magic, 1, sizeof(magic), f);
    std::fclose(f);
    if (n != sizeof(magic)) {
        std::fprintf(stderr, "cannot read magic from %s\n", path);
        return false;
    }
    if (magic == 0xBEBAFECAu || magic == 0xCAFEBABEu) {
        std::fprintf(stderr, "%s is a universal (fat) binary - Double Commander rejects these\n", path);
        return false;
    }
    if (magic != 0xFEEDFACFu && magic != 0xCFFAEDFEu) {
        std::fprintf(stderr, "%s is not a thin 64-bit Mach-O (magic 0x%08X)\n", path, magic);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    bool magicOnly = false;
    const char* path = "build/adb-fs-tc-plugin.wfx";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--magic-only") == 0) magicOnly = true;
        else path = argv[i];
    }
    if (!checkThinMacho64(path)) return 1;
    if (magicOnly) {
        std::printf("OK: %s is a thin 64-bit Mach-O\n", path);
        return 0;
    }
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
