/* MIT license. Small llvm-dis-compatible AIR reader for macOS.
 * Uses the system LLVM C API, following Cobalt64's documented GPUCompiler
 * framework discovery approach. No replacement shader/IR is generated. */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    void *library;
    void *(*context_create)(void);
    void (*context_dispose)(void *);
    void (*diagnostic_handler)(void *, void (*)(void *, void *), void *);
    char *(*diagnostic_description)(void *);
    void *(*buffer_copy)(const char *, size_t, const char *);
    void (*buffer_dispose)(void *);
    int (*parse)(void *, void *, void **);
    char *(*print)(void *);
    void (*module_dispose)(void *);
    void (*message_dispose)(char *);
    void (*opaque_pointers)(void *, bool);
    const char *path;
} Reader;

static bool bind_reader(Reader *reader, const char *path) {
    memset(reader, 0, sizeof(*reader));
    reader->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!reader->library) return false;
#define BIND(field, symbol) do { \
    *(void **)(&reader->field) = dlsym(reader->library, symbol); \
    if (!reader->field) { dlclose(reader->library); reader->library = NULL; return false; } \
} while (0)
    BIND(context_create, "LLVMContextCreate");
    BIND(context_dispose, "LLVMContextDispose");
    BIND(diagnostic_handler, "LLVMContextSetDiagnosticHandler");
    BIND(diagnostic_description, "LLVMGetDiagInfoDescription");
    BIND(buffer_copy, "LLVMCreateMemoryBufferWithMemoryRangeCopy");
    BIND(buffer_dispose, "LLVMDisposeMemoryBuffer");
    BIND(parse, "LLVMParseBitcodeInContext2");
    BIND(print, "LLVMPrintModuleToString");
    BIND(module_dispose, "LLVMDisposeModule");
    BIND(message_dispose, "LLVMDisposeMessage");
#undef BIND
    /* Older Apple LLVM prints typed pointers unless its context is explicitly
     * switched. Newer opaque-only LLVM need not export this optional method. */
    *(void **)(&reader->opaque_pointers) = dlsym(reader->library,
        "_ZNK4llvm11LLVMContext17setOpaquePointersEb");
    reader->path = path;
    return true;
}

static bool load_reader(Reader *reader) {
    /* The helper may run inside an arbitrary application's environment.
     * Select only the installed system reader; never accept a library path
     * from process-global environment or a developer toolchain. */
    /* These may live only in the dyld shared cache: do not require a disk file. */
    const char *paths[] = {
        "/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/Current/Libraries/libLLVM.dylib",
        "/System/Library/PrivateFrameworks/GPUCompiler.framework/GPUCompiler",
        "/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        if (bind_reader(reader, paths[i])) return true;
    return false;
}

static void diagnostic(void *info, void *opaque) {
    Reader *reader = opaque;
    char *description = reader->diagnostic_description(info);
    if (description) {
        fprintf(stderr, "Apple LLVM: %s\n", description);
        reader->message_dispose(description);
    }
}

int main(int argc, char **argv) {
    const bool version = argc == 2 && strcmp(argv[1], "--version") == 0;
    if (!version && (argc != 4 || strcmp(argv[2], "-o") != 0)) {
        fprintf(stderr, "usage: apple-air-dis INPUT.air -o OUTPUT.ll | --version\n");
        return 2;
    }
    Reader reader;
    if (!load_reader(&reader)) {
        fprintf(stderr, "No system Apple LLVM library with the required AIR reader API is available\n");
        return 1;
    }
    if (version) {
        printf("HackyNavi Apple AIR reader v1: %s\n", reader.path);
        dlclose(reader.library);
        return 0;
    }
    int result = 1;
    struct stat status;
    if (stat(argv[1], &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 4 ||
        (uint64_t)status.st_size > 64u * 1024u * 1024u) {
        fprintf(stderr, "AIR must be a regular file of 4 bytes through 64 MiB\n");
        dlclose(reader.library);
        return 1;
    }
    const size_t size = (size_t)status.st_size;
    char *data = malloc(size);
    FILE *input = fopen(argv[1], "rb");
    if (!data || !input) {
        fprintf(stderr, "Cannot allocate or open AIR input\n");
        if (input) fclose(input);
        free(data);
        dlclose(reader.library);
        return 1;
    }
    const bool read_ok = fread(data, 1, size, input) == size && !ferror(input);
    fclose(input);
    void *context = reader.context_create();
    void *buffer = NULL;
    void *module = NULL;
    if (!read_ok || !context) goto cleanup;
    reader.diagnostic_handler(context, diagnostic, &reader);
    if (reader.opaque_pointers) reader.opaque_pointers(context, true);
    buffer = reader.buffer_copy(data, size, "air");
    if (!buffer || reader.parse(context, buffer, &module) != 0 || !module) {
        fprintf(stderr, "System Apple LLVM rejected AIR bitcode\n");
        goto cleanup;
    }
    char *text = reader.print(module);
    if (!text || !*text) {
        if (text) reader.message_dispose(text);
        goto cleanup;
    }
    FILE *output = fopen(argv[3], "wb");
    if (output) {
        const size_t length = strlen(text);
        const bool written = fwrite(text, 1, length, output) == length && !ferror(output);
        const int closed = fclose(output);
        if (written && closed == 0) result = 0;
        else remove(argv[3]);
    }
    reader.message_dispose(text);
cleanup:
    if (module) reader.module_dispose(module);
    if (buffer) reader.buffer_dispose(buffer);
    if (context) reader.context_dispose(context);
    free(data);
    dlclose(reader.library);
    return result;
}
