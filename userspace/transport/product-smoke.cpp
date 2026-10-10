// SPDX-License-Identifier: MIT
// Target-only owned AIR/IR -> SPIR-V -> ACO -> native v1.3/v1.4 execution.
// No stored shader binaries, replacement kernels or software execution path.
#include "iokit_client.hpp"
#include "hackynavi_pipeline.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace t = hackynavi::transport;
namespace a = hackynavi::nativeabi;
namespace {
constexpr uint32_t kElements = 4099, kAllocationBytes = 20480, kWords = kAllocationBytes / 4;
constexpr uint32_t kGrid[3] = {kElements, 1, 1}, kLocal[3] = {64, 1, 1};
constexpr size_t kMaxInputBytes = 64U * 1024U * 1024U;
using Words = std::array<uint32_t, kWords>;
struct Options {
    const char *library = nullptr, *aco = nullptr, *dis = nullptr, *validator = nullptr;
    const char *input = nullptr, *entry = nullptr;
    uint32_t format = UINT32_MAX;
    bool affine = false, haveCheck = false, run = false, reuse = false;
    uint64_t registry = 0;
};
bool failed(const char* message) { std::fprintf(stderr, "%s\n", message); return false; }
bool checked(const char* operation, t::Result result) {
    if (result) return true;
    std::fprintf(stderr, "%s failed: status=%u IOReturn=0x%08x\n", operation,
        unsigned(result.status), unsigned(result.iokit)); return false;
}
bool path(const char* value, bool executable) {
    struct stat info{};
    if (!value || value[0] != '/' || stat(value, &info) || !S_ISREG(info.st_mode) ||
        access(value, executable ? X_OK : R_OK)) {
        std::fprintf(stderr, "Expected accessible absolute %s file path: %s\n",
            executable ? "executable" : "regular", value ? value : "(missing)"); return false;
    }
    return true;
}
bool options(int argc, char** argv, Options& out) {
    uint32_t seen = 0;
    for (int n = 1; n < argc; ++n) {
        if (!std::strcmp(argv[n], "--run")) { if (out.run) return false; out.run = true; continue; }
        if (!std::strcmp(argv[n], "--reuse")) { if (out.reuse) return false; out.reuse = true; continue; }
        const char* names[] = {"--library", "--aco", "--llvm-dis", "--spirv-val", "--input",
            "--format", "--check", "--entry", "--registry"};
        unsigned index = 0; while (index < 9 && std::strcmp(argv[n], names[index])) ++index;
        if (index == 9 || (seen & (1U << index)) || ++n == argc) return false;
        seen |= 1U << index; const char* value = argv[n];
        switch (index) {
        case 0: out.library = value; break;
        case 1: out.aco = value; break;
        case 2: out.dis = value; break;
        case 3: out.validator = value; break;
        case 4: out.input = value; break;
        case 5:
            if (!std::strcmp(value, "air")) out.format = HN_SHADER_AIR;
            else if (!std::strcmp(value, "llvm")) out.format = HN_SHADER_LLVM_TEXT;
            else return false;
            break;
        case 6:
            if (!std::strcmp(value, "add-one")) out.affine = false;
            else if (!std::strcmp(value, "affine-xor")) out.affine = true;
            else return false;
            out.haveCheck = true; break;
        case 7: if (!*value || std::strlen(value) > 1024) return false; out.entry = value; break;
        case 8: {
            char* end = nullptr; errno = 0; if (*value == '-' || *value == '+') return false;
            out.registry = std::strtoull(value, &end, 0);
            if (errno || !out.registry || end == value || *end) return false;
            break;
        }
        }
    }
    if (!out.entry) out.entry = out.affine ? "affine_xor" : "add_one";
    return out.run && out.haveCheck && out.format != UINT32_MAX &&
        path(out.library, false) && path(out.aco, true) && path(out.dis, true) &&
        path(out.validator, true) && path(out.input, false);
}
bool readInput(const char* name, std::vector<uint8_t>& out) {
    FILE* file = std::fopen(name, "rb");
    if (!file) return failed("Cannot open shader input");
    bool success = false;
    try {
        if (!std::fseek(file, 0, SEEK_END)) {
            const long bytes = std::ftell(file);
            if (bytes > 0 && uint64_t(bytes) <= kMaxInputBytes && !std::fseek(file, 0, SEEK_SET)) {
                out.resize(size_t(bytes));
                success = std::fread(out.data(), 1, out.size(), file) == out.size() &&
                    std::fgetc(file) == EOF && !std::ferror(file);
            }
        }
    } catch (...) { std::fclose(file); throw; }
    if (std::fclose(file)) success = false;
    return success || failed("Shader input is empty, exceeds 64MiB, changed size or could not be read");
}
class Runtime {
public:
#define HN_FUNCTION(name) decltype(&::name) name = nullptr
    HN_FUNCTION(hn_shader_abi_version); HN_FUNCTION(hn_shader_dispatch_abi_version);
    HN_FUNCTION(hn_pipeline_abi_version); HN_FUNCTION(hn_shader_compile);
    HN_FUNCTION(hn_shader_result_error); HN_FUNCTION(hn_shader_result_free);
    HN_FUNCTION(hn_pipeline_build); HN_FUNCTION(hn_pipeline_error); HN_FUNCTION(hn_pipeline_info);
    HN_FUNCTION(hn_pipeline_variant); HN_FUNCTION(hn_pipeline_region); HN_FUNCTION(hn_pipeline_resource);
    HN_FUNCTION(hn_pipeline_blob_size); HN_FUNCTION(hn_pipeline_copy_blob); HN_FUNCTION(hn_pipeline_free);
#undef HN_FUNCTION
    HNShaderResult* shader = nullptr;
    HNPipelineResult* product = nullptr;
    ~Runtime() {
        if (product && hn_pipeline_free) hn_pipeline_free(product);
        if (shader && hn_shader_result_free) hn_shader_result_free(shader);
        if (library_) dlclose(library_);
    }
    bool open(const char* name) {
        library_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!library_) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return false; }
#define HN_LOAD(name) if (!load(name, #name)) return false
        HN_LOAD(hn_shader_abi_version); HN_LOAD(hn_shader_dispatch_abi_version); HN_LOAD(hn_pipeline_abi_version);
        HN_LOAD(hn_shader_compile); HN_LOAD(hn_shader_result_error); HN_LOAD(hn_shader_result_free);
        HN_LOAD(hn_pipeline_build); HN_LOAD(hn_pipeline_error); HN_LOAD(hn_pipeline_info);
        HN_LOAD(hn_pipeline_variant); HN_LOAD(hn_pipeline_region); HN_LOAD(hn_pipeline_resource);
        HN_LOAD(hn_pipeline_blob_size); HN_LOAD(hn_pipeline_copy_blob); HN_LOAD(hn_pipeline_free);
#undef HN_LOAD
        return (hn_shader_abi_version() == 1 && hn_shader_dispatch_abi_version() == 1 &&
            hn_pipeline_abi_version() == 1) || failed("Unsupported shader/dispatch/pipeline C ABI version");
    }
private:
    template<class T> bool load(T& function, const char* name) {
        dlerror(); void* symbol = dlsym(library_, name); const char* error = dlerror();
        if (error || !symbol) { std::fprintf(stderr, "Missing runtime function %s: %s\n", name, error ? error : "null"); return false; }
        static_assert(sizeof(function) == sizeof(symbol), "POSIX function pointer representation");
        std::memcpy(&function, &symbol, sizeof(function)); return true;
    }
    void* library_ = nullptr;
};
struct Product {
    std::array<HNPipelineVariant, 2> metadata{};
    std::array<std::vector<uint8_t>, 2> code;
};
bool compile(const Options& o, Runtime& api, Product& out) {
    // Configure real tools before loading the runtime or starting any workers.
    if (setenv("METAL2VULKAN_LLVM_DIS", o.dis, 1) || setenv("METAL2VULKAN_SPIRV_VAL", o.validator, 1))
        return failed("Cannot configure compiler tool paths");
    if (!api.open(o.library)) return false;
    std::vector<uint8_t> input;
    if (!readInput(o.input, input)) return false;
    std::printf("Compiling supplied %s input %s, entry=%s, grid=4099,1,1 local=64,1,1\n",
        o.format == HN_SHADER_AIR ? "AIR" : "LLVM IR", o.input, o.entry);
    api.shader = api.hn_shader_compile(input.data(), input.size(), o.format, HN_SHADER_COMPUTE,
        o.entry, 64, 1, 1, HN_SHADER_EXACT_THREADS, 0);
    const char* error = api.shader ? api.hn_shader_result_error(api.shader) : nullptr;
    if (!api.shader || !error || *error) {
        std::fprintf(stderr, "Shader compile failed: %s\n", error ? error : "no valid result"); return false;
    }
    api.product = api.hn_pipeline_build(api.shader, HN_SHADER_EXACT_THREADS, kGrid, kLocal, o.aco);
    error = api.product ? api.hn_pipeline_error(api.product) : nullptr;
    if (!api.product || !error || *error) {
        std::fprintf(stderr, "Owned pipeline build failed: %s\n", error ? error : "no valid result"); return false;
    }
    // Read retained products only after releasing the original shader.
    api.hn_shader_result_free(api.shader); api.shader = nullptr;
    HNPipelineInfo info{};
    if (api.hn_pipeline_info(api.product, &info, sizeof(info)) != 1 || info.variant_count != 2 ||
        info.region_count != 2 || info.resource_count != 2 || info.descriptor_table_bytes != 32 ||
        info.push_constant_bytes != 48 || info.descriptor_set || info.descriptor_stride_bytes != 16 || info.target_gfx != 1100)
        return failed("Pipeline does not match the two-buffer GFX1100 smoke contract");
    uint32_t sizes = 0, resources = 0, regions = 0;
    for (uint32_t n = 0; n < 2; ++n) {
        auto& v = out.metadata[n];
        if (api.hn_pipeline_variant(api.product, n, &v, sizeof(v)) != 1 ||
            v.local_size[1] != 1 || v.local_size[2] != 1 ||
            (v.local_size[0] != 64 && v.local_size[0] != 3) || v.wave_size != 32 ||
            v.num_sgprs < 8 || v.num_sgprs > 128 || !v.num_vgprs || v.num_vgprs > 256 ||
            v.descriptor_binding_mask != 3 || v.descriptor_read_mask != 1 || v.descriptor_write_mask != 2 ||
            v.descriptor_table_bytes != 32 || v.push_constant_bytes != 48 ||
            !v.code_bytes || v.code_bytes > a::kMaxProductCodeBytes || (v.code_bytes & 3) ||
            !v.executable_bytes || v.executable_bytes > v.code_bytes || (v.executable_bytes & 3) ||
            !v.code_alignment || (v.code_alignment & (v.code_alignment - 1)) || v.code_alignment > 4096)
            return failed("Unsupported or invalid actual compiler variant metadata");
        const uint32_t bit = v.local_size[0] == 64 ? 1 : 2;
        if (sizes & bit) return failed("Duplicate actual compiler local-size variant");
        sizes |= bit;
        if (api.hn_pipeline_blob_size(api.product, HN_PIPELINE_CODE, n) != v.code_bytes)
            return failed("Actual compiler code size differs from its metadata");
        out.code[n].resize(v.code_bytes);
        if (api.hn_pipeline_copy_blob(api.product, HN_PIPELINE_CODE, n, out.code[n].data(), out.code[n].size()) != 1)
            return failed("Cannot copy owned compiler code");
        std::printf("Actual variant%u local=%u,1,1 code=%u executable=%u SGPR=%u VGPR=%u\n",
            n, v.local_size[0], v.code_bytes, v.executable_bytes, v.num_sgprs, v.num_vgprs);
        HNPipelineResource resource{};
        if (api.hn_pipeline_resource(api.product, n, &resource, sizeof(resource)) != 1 || resource.descriptor_set ||
            resource.binding > 1 || resource.metal_index != resource.binding || resource.declared_element_bytes != 4 ||
            resource.access != (resource.binding ? 2U : 1U) || resource.reflected_access != resource.access ||
            (resources & (1U << resource.binding))) return failed("Unexpected compiled input/output resource contract");
        resources |= 1U << resource.binding;
    }
    for (uint32_t n = 0; n < 2; ++n) {
        HNPipelineRegion region{};
        if (api.hn_pipeline_region(api.product, n, &region, sizeof(region)) != 1 || region.variant_index >= 2)
            return failed("Cannot read actual pipeline dispatch region");
        const bool tail = out.metadata[region.variant_index].local_size[0] == 3;
        const uint32_t bit = tail ? 2 : 1, expectedLocal[3] = {tail ? 3U : 64U, 1, 1};
        const uint32_t expectedGroups[3] = {tail ? 1U : 64U, 1, 1};
        const uint32_t expectedBase[3] = {tail ? 4096U : 0U, 0, 0}, expectedGroupBase[3] = {tail ? 64U : 0U, 0, 0};
        const uint32_t expectedSpec[3] = {0, 1, 2};
        const uint32_t expectedPush[12] = {4099, 1, 1, tail ? 4096U : 0U, 0, 0, tail ? 64U : 0U, 0, 0, 65, 1, 1};
        const auto& d = region.dispatch;
        if ((regions & bit) || std::memcmp(d.local_size, expectedLocal, sizeof(expectedLocal)) ||
            std::memcmp(d.group_count, expectedGroups, sizeof(expectedGroups)) ||
            std::memcmp(d.thread_base, expectedBase, sizeof(expectedBase)) ||
            std::memcmp(d.threadgroup_base, expectedGroupBase, sizeof(expectedGroupBase)) ||
            std::memcmp(d.local_size_spec_ids, expectedSpec, sizeof(expectedSpec)) ||
            d.push_constant_offset || d.push_constant_size != 48 ||
            std::memcmp(d.push_constants, expectedPush, sizeof(expectedPush)))
            return failed("Actual compiler regions do not exactly cover 4096 full and three tail threads");
        regions |= bit;
    }
    if (sizes != 3 || resources != 3 || regions != 3) return failed("Incomplete actual pipeline contract");
    api.hn_pipeline_free(api.product); api.product = nullptr;
    return true;
}
uint32_t inputWord(uint32_t n) {
    switch (n % 16) {
    case 0: return 0; case 1: return UINT32_MAX; case 2: return UINT32_MAX - 1;
    case 3: return 0x7fffffff; case 4: return 0x80000000;
    default: return 0x137ad9bfU + n * 0x9e3779b9U;
    }
}
uint32_t expectedWord(uint32_t input, uint32_t index, bool affine) {
    return affine ? ((input * 7U) ^ 0x5a5a5a5aU) + index : input + 1U;
}
uint32_t guardWord(uint32_t n) { return 0xf17e0000U ^ (n * 0x01020305U); }
uint32_t resultWord(const Words& input, uint32_t n, bool affine, unsigned job) {
    if (n >= kElements) return guardWord(n) ^ (job == 2 ? 0x6acd972bU : 0U);
    uint32_t value = input[n];
    for (unsigned pass = 0; pass < job; ++pass) value = expectedWord(value, n, affine);
    return value;
}
bool compare(const Words& actual, const Words& input, bool affine, unsigned job) {
    for (uint32_t n = 0; n < kWords; ++n) {
        const uint32_t expected = resultWord(input, n, affine, job);
        if (actual[n] != expected) {
            std::fprintf(stderr, "Job%u %s mismatch at word%u: expected=%08x observed=%08x\n",
                job, n < kElements ? "GPU result" : "guard", n, expected, actual[n]); return false;
        }
    }
    return true;
}
bool missing(const char* operation, t::Result result) {
    if (result.status == t::Status::NotFound) return true;
    std::fprintf(stderr, "%s must reject released history: status=%u IOReturn=0x%08x\n",
        operation, unsigned(result.status), unsigned(result.iokit)); return false;
}
bool zeroed(t::Client& client, const t::Buffer& buffer, uint32_t bytes) {
    Words actual; actual.fill(UINT32_MAX);
    if (bytes > kAllocationBytes || !checked("read zeroed allocation", client.read(buffer, 0, actual.data(), bytes))) return false;
    for (uint32_t n = 0; n < bytes / 4; ++n) if (actual[n]) return failed("Native allocation was not zeroed");
    return true;
}
bool run(const Options& o, const Product& product) {
    t::Client client;
    if (!checked(o.reuse ? "open reusable product v1.4 profile" : "open product v1.3 profile",
        o.reuse ? client.openReusableProduct(o.registry) : client.openProduct(o.registry))) return false;
    bool passed = false;
    try {
        passed = [&]() {
            a::HelloReply info{}; if (!checked("information", client.info(info))) return false;
            std::printf("Target registry=%llu epoch=%llu ABI=%u.%u capabilities=0x%llx\n",
                static_cast<unsigned long long>(info.registryId), static_cast<unsigned long long>(info.header.epoch),
                info.header.major, info.header.minor, static_cast<unsigned long long>(info.capabilities));
            if (info.pageBytes != 4096 || info.maxAllocationBytes < kAllocationBytes ||
                info.vaEnd <= info.vaBegin || info.vaEnd - info.vaBegin < 2 * kAllocationBytes)
                return failed("Target cannot provide two isolated 20KiB data mappings");
            t::Buffer inputBuffer{}, outputBuffer{}, secondOutput{};
            if (!checked("allocate input", client.allocate(kAllocationBytes, 4096, a::Domain::System, 0, inputBuffer)) ||
                !checked("allocate output", client.allocate(kAllocationBytes, 4096, a::Domain::System, 0, outputBuffer)) ||
                !zeroed(client, inputBuffer, kAllocationBytes) || !zeroed(client, outputBuffer, kAllocationBytes) ||
                !checked("map input read-only", client.mapVa(inputBuffer, info.vaBegin, 0, kAllocationBytes, a::Read)) ||
                !checked("map output read/write", client.mapVa(outputBuffer, info.vaBegin + kAllocationBytes,
                    0, kAllocationBytes, a::Read | a::Write))) return false;
            if (o.reuse && (!checked("allocate initially unmapped C", client.allocate(kAllocationBytes, 4096,
                    a::Domain::System, 0, secondOutput)) || !zeroed(client, secondOutput, kAllocationBytes))) return false;
            std::array<t::ProductVariant, 2> variants{};
            for (uint32_t n = 0; n < 2; ++n) {
                const auto& metadata = product.metadata[n]; auto& variant = variants[n];
                const uint32_t bytes = (metadata.code_bytes + 4095U) & ~4095U;
                if (!checked("allocate unmapped code staging", client.allocate(bytes, 4096, a::Domain::System, 0, variant.code)) ||
                    !zeroed(client, variant.code, bytes) || !checked("upload actual compiler code", client.write(
                        variant.code, 0, product.code[n].data(), product.code[n].size()))) return false;
                // Staging BOs are deliberately never GPU-mapped. Native capture
                // copies these bytes into separate kernel-owned RX allocations.
                variant.codeBytes = metadata.code_bytes; variant.executableBytes = metadata.executable_bytes;
                std::memcpy(variant.local, metadata.local_size, sizeof(variant.local));
                variant.waveSize = metadata.wave_size; variant.numSgprs = metadata.num_sgprs; variant.numVgprs = metadata.num_vgprs;
            }
            Words input{}, poison{}, actual{};
            for (uint32_t n = 0; n < kElements; ++n) {
                input[n] = inputWord(n); poison[n] = ~expectedWord(input[n], n, o.affine);
            }
            for (uint32_t n = kElements; n < kWords; ++n) { input[n] = ~guardWord(n); poison[n] = guardWord(n); }
            if (!checked("upload inputs", client.write(inputBuffer, 0, input.data(), kAllocationBytes)) ||
                !checked("upload poisoned output and guards", client.write(outputBuffer, 0, poison.data(), kAllocationBytes))) return false;
            if (o.reuse) {
                // This is C's initial poison only. No CPU write is made to A,
                // B, C or code after the first submission; B's actual GPU-written
                // allocation supplies the second job's input without reupload.
                for (uint32_t n = 0; n < kWords; ++n) {
                    const uint32_t expected = resultWord(input, n, o.affine, 2);
                    poison[n] = n < kElements ? ~expected : expected;
                }
                if (!checked("upload initial C poison and distinct guards",
                    client.write(secondOutput, 0, poison.data(), kAllocationBytes))) return false;
            }
            t::ProductResource resources[2] = {{inputBuffer, 0, kAllocationBytes, 0}, {outputBuffer, 0, kAllocationBytes, 1}};
            t::ProductDispatch dispatch{}; dispatch.variants = variants.data(); dispatch.variantCount = 2;
            dispatch.resources = resources; dispatch.resourceCount = 2;
            std::memcpy(dispatch.grid, kGrid, sizeof(kGrid)); std::memcpy(dispatch.nominalLocal, kLocal, sizeof(kLocal));
            dispatch.result = outputBuffer; dispatch.resultBytes = kAllocationBytes;
            t::Fence fence{};
            if (!checked("submit actual owned compiler product", client.product(dispatch, fence)) ||
                !checked("GPU-written completion", client.wait(fence, a::kMaxWaitNs))) return false;
            actual.fill(0xacabdead);
            if (!checked("read immutable GPU output and guards", client.readResult(fence, 0, actual.data(), kAllocationBytes))) return false;
            if (!compare(actual, input, o.affine, 1)) return false;
            std::printf("Verified GPU readback: %u active values, %u untouched suffix guards, sequence=%llu\n",
                kElements, kWords - kElements, static_cast<unsigned long long>(fence.sequence));
            if (!o.reuse) return true;
            const t::Buffer retainedB = outputBuffer;
            if (!checked("unmap A", client.unmapVa(inputBuffer, info.vaBegin, 0, kAllocationBytes)) ||
                !checked("unmap B read/write", client.unmapVa(outputBuffer, info.vaBegin + kAllocationBytes, 0, kAllocationBytes)) ||
                !checked("remap retained B read-only", client.mapVa(outputBuffer, info.vaBegin, 0, kAllocationBytes, a::Read)) ||
                !checked("map C read/write", client.mapVa(secondOutput, info.vaBegin + kAllocationBytes, 0,
                    kAllocationBytes, a::Read | a::Write))) return false;
            resources[0].buffer = outputBuffer; resources[1].buffer = secondOutput; dispatch.result = secondOutput;
            t::Fence second{};
            if (!checked("submit B to C using retained GPU data and actual compiler products", client.product(dispatch, second)) ||
                !checked("second GPU-written completion", client.wait(second, a::kMaxWaitNs))) return false;
            if (second.epoch != fence.epoch || second.sequence <= fence.sequence ||
                outputBuffer.epoch != retainedB.epoch || outputBuffer.handle.id != retainedB.handle.id ||
                outputBuffer.handle.generation != retainedB.handle.generation)
                return failed("Reusable job identity or retained B handle changed unexpectedly");
            actual.fill(0xacabdead);
            if (!checked("read second GPU output and guards", client.readResult(second, 0, actual.data(), kAllocationBytes)) ||
                !compare(actual, input, o.affine, 2)) return false;
            // Real BO reads after both completed jobs separately confirm that
            // the buffer objects survived; these CPU copies are only compared.
            actual.fill(0xacabdead);
            if (!checked("read retained B BO", client.read(outputBuffer, 0, actual.data(), kAllocationBytes)) ||
                !compare(actual, input, o.affine, 1)) return false;
            actual.fill(0xacabdead);
            if (!checked("read retained C BO", client.read(secondOutput, 0, actual.data(), kAllocationBytes)) ||
                !compare(actual, input, o.affine, 2)) return false;
            actual.fill(0xacabdead);
            if (!checked("wait historical first completion", client.wait(fence, a::kMaxWaitNs)) ||
                !checked("read historical first output after second job", client.readResult(fence, 0, actual.data(), kAllocationBytes)) ||
                !compare(actual, input, o.affine, 1)) return false;
            t::Fence released = fence;
            if (!checked("release first historical result", client.releaseResult(fence)) || fence.sequence || fence.epoch)
                return failed("First history release failed or left a live client fence");
            uint32_t untouched = 0x9ab13dc5;
            if (!missing("wait released result", client.wait(released, a::kMaxWaitNs)) ||
                !missing("read released result", client.readResult(released, 0, &untouched, sizeof(untouched))) ||
                untouched != 0x9ab13dc5 || !missing("repeat release", client.releaseResult(released))) return false;
            actual.fill(0xacabdead);
            if (!checked("read surviving second historical result", client.readResult(second, 0, actual.data(), kAllocationBytes)) ||
                !compare(actual, input, o.affine, 2)) return false;
            std::printf("Verified retained B to C, two full/tail GPU jobs, two histories and released-result rejection; second sequence=%llu\n",
                static_cast<unsigned long long>(second.sequence));
            if (!checked("release second historical result", client.releaseResult(second)) || second.sequence || second.epoch)
                return failed("Second history release failed or left a live client fence");
            if (!checked("unmap retained B", client.unmapVa(outputBuffer, info.vaBegin, 0, kAllocationBytes)) ||
                !checked("unmap C", client.unmapVa(secondOutput, info.vaBegin + kAllocationBytes, 0, kAllocationBytes)) ||
                !checked("free A", client.release(inputBuffer)) || !checked("free retained B", client.release(outputBuffer)) ||
                !checked("free C", client.release(secondOutput))) return false;
            for (auto& v : variants) if (!checked("free code staging after both jobs", client.release(v.code))) return false;
            return true;
        }();
    } catch (const std::exception& error) { std::fprintf(stderr, "Native smoke failed: %s\n", error.what()); }
    // Check real native Close on every opened session, including failures.
    // A transport error/timeout is never treated as a retirement proof.
    const bool closed = checked("explicit native close", client.close());
    return passed && closed;
}
}
int main(int argc, char** argv) {
    Options o;
    if (!options(argc, argv, o)) {
        std::fprintf(stderr, "usage: %s --run --library /path/runtime.dylib --aco /path/hackynavi-aco "
            "--llvm-dis /path/llvm-dis --spirv-val /path/spirv-val --input /path/kernel.air "
            "--format air|llvm --check add-one|affine-xor [--entry name] [--registry id] [--reuse]\n"
            "Navi31 target only. Compiles supplied source, then executes4099 threads using real local64/local3 code.\n", argv[0]);
        return 2;
    }
    try {
        Runtime api; Product product;
        if (!compile(o, api, product) || !run(o, product)) return 1;
    } catch (const std::exception& error) { std::fprintf(stderr, "Product smoke failed: %s\n", error.what()); return 1; }
    std::puts(o.reuse ? "PASS: supplied-source compilation, two real native GPU full/tail jobs with retained data, result history, guards, explicit frees and close verified." :
        "PASS: supplied-source compilation, real native GPU full/tail dispatch, readback, guards and explicit close verified.");
    return 0;
}
