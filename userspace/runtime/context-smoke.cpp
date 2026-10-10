// SPDX-License-Identifier: MIT
// Target-only test of the actual synchronous compute Context. No GPU work is
// performed without --run, and no replacement shader or software backend exists.
#include "compute_context.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sys/stat.h>
#include <unistd.h>

namespace c = hackynavi::compute;
namespace a = hackynavi::nativeabi;
namespace {
constexpr uint32_t kElements = 4099, kBytes = 20480, kWords = kBytes / 4;
constexpr uint32_t kGrid[3] = {kElements, 1, 1}, kLocal[3] = {64, 1, 1};
constexpr size_t kMaxInputBytes = 64U * 1024U * 1024U;
using Words = std::array<uint32_t, kWords>;
struct Options {
    const char *aco = nullptr, *dis = nullptr, *validator = nullptr;
    const char *input = nullptr, *entry = nullptr;
    uint32_t format = UINT32_MAX;
    uint64_t registry = 0;
    bool affine = false, haveCheck = false, run = false;
};
bool failed(const char* message) { std::fprintf(stderr, "%s\n", message); return false; }
bool checked(const char* operation, c::Result result) {
    if (result) return true;
    std::fprintf(stderr, "%s failed: status=%u IOReturn=0x%08x\n", operation,
        unsigned(result.status), unsigned(result.iokit)); return false;
}
void usage(const char* executable) {
    std::fprintf(stderr,
        "Usage: %s --run --aco /path/hackynavi-aco --llvm-dis /path/apple-air-dis\n"
        "  --spirv-val /path/spirv-val --input /path/kernel --format air|llvm\n"
        "  --check add-one|affine-xor [--entry NAME] [--registry ID]\n"
        "Target only: requires the real v1.4 engine-enabled driver. The compiler C ABI is linked.\n",
        executable);
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
        const char* names[] = {"--aco", "--llvm-dis", "--spirv-val", "--input", "--format", "--check", "--entry", "--registry"};
        unsigned index = 0; while (index < 8 && std::strcmp(argv[n], names[index])) ++index;
        if (index == 8 || (seen & (1U << index)) || ++n == argc) return false;
        seen |= 1U << index; const char* value = argv[n];
        switch (index) {
        case 0: out.aco = value; break;
        case 1: out.dis = value; break;
        case 2: out.validator = value; break;
        case 3: out.input = value; break;
        case 4:
            if (!std::strcmp(value, "air")) out.format = HN_SHADER_AIR;
            else if (!std::strcmp(value, "llvm")) out.format = HN_SHADER_LLVM_TEXT;
            else return false;
            break;
        case 5:
            if (!std::strcmp(value, "add-one")) out.affine = false;
            else if (!std::strcmp(value, "affine-xor")) out.affine = true;
            else return false;
            out.haveCheck = true; break;
        case 6: if (!*value || std::strlen(value) > 1024) return false; out.entry = value; break;
        case 7: {
            char* end = nullptr; errno = 0;
            if (*value == '-' || *value == '+' || *value == ' ' || *value == '\t') return false;
            out.registry = std::strtoull(value, &end, 0);
            if (errno || !out.registry || end == value || *end) return false;
            break;
        }
        }
    }
    if (!out.entry) out.entry = out.affine ? "affine_xor" : "add_one";
    return out.run && out.haveCheck && out.format != UINT32_MAX && path(out.aco, true) &&
        path(out.dis, true) && path(out.validator, true) && path(out.input, false);
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
    return success || failed("Input is empty, exceeds 64MiB, changed size or could not be read");
}
bool compile(const Options& o, std::shared_ptr<const c::Pipeline>& output) {
    if (setenv("METAL2VULKAN_LLVM_DIS", o.dis, 1) || setenv("METAL2VULKAN_SPIRV_VAL", o.validator, 1))
        return failed("Cannot configure actual compiler tools");
    if (hn_shader_abi_version() != 1 || hn_shader_dispatch_abi_version() != 1 || hn_pipeline_abi_version() != 1)
        return failed("Unsupported linked compiler C ABI version");
    std::vector<uint8_t> input;
    if (!readInput(o.input, input)) return false;
    std::printf("Compiling supplied %s, entry=%s, grid=4099,1,1 local=64,1,1\n", o.input, o.entry);
    std::unique_ptr<HNShaderResult, decltype(&hn_shader_result_free)> shader(
        hn_shader_compile(input.data(), input.size(), o.format, HN_SHADER_COMPUTE,
            o.entry, 64, 1, 1, HN_SHADER_EXACT_THREADS, 0), &hn_shader_result_free);
    const char* error = shader ? hn_shader_result_error(shader.get()) : nullptr;
    if (!error || *error) {
        std::fprintf(stderr, "Shader compile failed: %s\n", error ? error : "no valid result"); return false;
    }
    std::unique_ptr<HNPipelineResult, decltype(&hn_pipeline_free)> product(
        hn_pipeline_build(shader.get(), HN_SHADER_EXACT_THREADS, kGrid, kLocal, o.aco), &hn_pipeline_free);
    error = product ? hn_pipeline_error(product.get()) : nullptr;
    if (!error || *error) {
        std::fprintf(stderr, "Pipeline build failed: %s\n", error ? error : "no valid result"); return false;
    }
    HNPipelineInfo info{};
    if (hn_pipeline_info(product.get(), &info, sizeof(info)) != 1 || info.variant_count != 2 ||
        info.region_count != 2 || info.resource_count != 2 || info.descriptor_table_bytes != 32 ||
        info.push_constant_bytes != 48 || info.descriptor_set || info.descriptor_stride_bytes != 16 || info.target_gfx != 1100)
        return failed("Actual compiler product is outside the two-buffer test contract");
    std::array<HNPipelineVariant, 2> original{};
    std::array<std::vector<uint8_t>, 2> code;
    for (uint32_t n = 0; n < 2; ++n) {
        if (hn_pipeline_variant(product.get(), n, &original[n], sizeof(original[n])) != 1 ||
            !original[n].code_bytes || original[n].code_bytes > a::kMaxProductCodeBytes ||
            hn_pipeline_blob_size(product.get(), HN_PIPELINE_CODE, n) != original[n].code_bytes)
            return failed("Cannot inspect bounded actual compiler variant");
        code[n].resize(original[n].code_bytes);
        if (hn_pipeline_copy_blob(product.get(), HN_PIPELINE_CODE, n, code[n].data(), code[n].size()) != 1)
            return failed("Cannot copy actual compiler output for comparison");
    }
    if (!checked("capture immutable compute pipeline", c::Pipeline::capture(product.get(), kGrid, kLocal, output))) return false;
    product.reset(); shader.reset(); // no original compiler handle survives GPU open
    if (output->variantCount() != 2 || output->resources().size() != 2)
        return failed("Captured pipeline shape changed");
    uint32_t seen = 0, resourceMask = 0;
    for (uint32_t n = 0; n < 2; ++n) {
        HNPipelineVariant v{};
        if (!checked("read retained variant metadata", output->variantInfo(n, v))) return false;
        if (std::memcmp(&v, &original[n], sizeof(v)) || v.local_size[1] != 1 || v.local_size[2] != 1 ||
            (v.local_size[0] != 64 && v.local_size[0] != 3) || v.descriptor_binding_mask != 3 ||
            v.descriptor_read_mask != 1 || v.descriptor_write_mask != 2)
            return failed("Retained variant does not match the supplied compiler product");
        const uint32_t bit = v.local_size[0] == 64 ? 1U : 2U;
        if (seen & bit) return failed("Duplicate local-size variant");
        seen |= bit;
        std::vector<uint8_t> copy(v.code_bytes);
        if (!checked("copy retained code", output->copyCode(n, copy.data(), copy.size())) || copy != code[n])
            return failed("Retained shader code changed after freeing the compiler result");
        std::printf("Owned variant%u: local=%u,1,1 code=%u bytes\n", n, v.local_size[0], v.code_bytes);
    }
    for (const auto& r : output->resources()) {
        if (r.descriptor_set || r.binding > 1 || r.metal_index != r.binding || r.declared_element_bytes != 4 ||
            r.access != (r.binding ? 2U : 1U) || r.reflected_access != r.access || (resourceMask & (1U << r.binding)))
            return failed("Expected uint32 input binding0 and output binding1");
        resourceMask |= 1U << r.binding;
    }
    return (seen == 3 && resourceMask == 3) || failed("Incomplete captured pipeline");
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
uint32_t poisonWord(uint32_t n, bool second) {
    return (0xa5a55a5aU ^ (n * 0x9e3779b9U)) ^ (second ? UINT32_MAX : 0U);
}
bool compare(const char* label, const Words& actual, const Words& input, bool affine, unsigned job) {
    for (uint32_t n = 0; n < kWords; ++n) {
        uint32_t expected = input[n];
        if (n < kElements) for (unsigned pass = 0; pass < job; ++pass) expected = expectedWord(expected, n, affine);
        else expected = poisonWord(n, job == 2);
        if (actual[n] != expected) {
            std::fprintf(stderr, "%s %s mismatch at word%u: expected=%08x observed=%08x\n",
                label, n < kElements ? "GPU output" : "guard", n, expected, actual[n]); return false;
        }
    }
    std::printf("%s: all 4099 values and 1021 suffix guards match\n", label); return true;
}
bool run(const Options& o) {
    std::shared_ptr<c::Context> context;
    std::array<std::shared_ptr<c::Buffer>, 3> buffers;
    bool passed = false;
    try {
        passed = [&]() {
            std::shared_ptr<const c::Pipeline> pipeline;
            if (!compile(o, pipeline) || !checked("open actual v1.4 compute context", c::Context::open(o.registry, context))) return false;
            a::HelloReply info{};
            if (!checked("read actual context information", context->information(info))) return false;
            std::printf("Registry=%llu epoch=%llu device=%04x:%04x\n", (unsigned long long)info.registryId,
                (unsigned long long)info.header.epoch, unsigned(info.vendorId), unsigned(info.deviceId));
            for (auto& buffer : buffers)
                if (!checked("allocate persistent 20KiB buffer", context->allocate(kBytes, buffer)) || buffer->length() != kBytes)
                    return failed("Persistent native buffer allocation failed");
            std::array<uint64_t, 3> addresses{};
            for (size_t n = 0; n < buffers.size(); ++n) {
                addresses[n] = buffers[n]->gpuAddress();
                if (addresses[n] < info.vaBegin || addresses[n] >= info.vaEnd || (addresses[n] & 4095) ||
                    kBytes > info.vaEnd - addresses[n]) return failed("Invalid persistent buffer GPU address");
                for (size_t p = 0; p < n; ++p)
                    if (addresses[n] < addresses[p] + kBytes && addresses[p] < addresses[n] + kBytes)
                        return failed("Persistent buffer GPU addresses overlap");
            }
            Words input{}, poisonB{}, poisonC{}, firstB{}, finalB{}, finalC{};
            for (uint32_t n = 0; n < kWords; ++n) {
                input[n] = inputWord(n); poisonB[n] = poisonWord(n, false); poisonC[n] = poisonWord(n, true);
            }
            // These are the only data uploads. The second job consumes the
            // original GPU-written B object, never firstB's CPU copy.
            if (!checked("upload A once", buffers[0]->upload(0, input.data(), kBytes)) ||
                !checked("poison B once", buffers[1]->upload(0, poisonB.data(), kBytes)) ||
                !checked("poison C once", buffers[2]->upload(0, poisonC.data(), kBytes))) return false;
            c::Binding firstBindings[2] = {{0, buffers[0], 0, kBytes}, {1, buffers[1], 0, kBytes}};
            c::Completion first{}, second{};
            if (!checked("dispatch actual A -> B", context->dispatch(pipeline, firstBindings, 2, first)) ||
                !first.sequence || first.epoch != info.header.epoch || !first.epoch)
                return failed("First actual completion is absent or has the wrong epoch");
            if (!checked("download B after job one", buffers[1]->download(0, firstB.data(), kBytes)) ||
                !compare("Job one B", firstB, input, o.affine, 1)) return false;
            c::Binding secondBindings[2] = {{0, buffers[1], 0, kBytes}, {1, buffers[2], 0, kBytes}};
            if (!checked("dispatch retained B -> C", context->dispatch(pipeline, secondBindings, 2, second)) ||
                second.epoch != first.epoch || second.sequence <= first.sequence)
                return failed("Second actual completion must retain epoch and advance sequence");
            if (!checked("download retained B after job two", buffers[1]->download(0, finalB.data(), kBytes)) ||
                !checked("download C after job two", buffers[2]->download(0, finalC.data(), kBytes)) ||
                !compare("Retained B", finalB, input, o.affine, 1) || !compare("Job two C", finalC, input, o.affine, 2)) return false;
            if (firstB != finalB) return failed("The second job changed its retained input B");
            for (size_t n = 0; n < buffers.size(); ++n)
                if (buffers[n]->gpuAddress() != addresses[n]) return failed("A persistent GPU address changed between jobs");
            std::printf("Actual completion observations: epoch=%llu sequence=%llu then %llu\n",
                (unsigned long long)first.epoch, (unsigned long long)first.sequence, (unsigned long long)second.sequence);
            std::printf("Retained B GPU address: 0x%llx\n", (unsigned long long)addresses[1]);
            if (!checked("explicitly release B", buffers[1]->release())) return false;
            uint32_t unchanged = 0x6ac7e219U;
            const auto stale = buffers[1]->download(0, &unchanged, sizeof(unchanged));
            if (stale.status != c::Status::NotFound || unchanged != 0x6ac7e219U)
                return failed("Released Buffer must reject download with NotFound and leave output unchanged");
            return true;
        }();
    } catch (const std::exception& error) { std::fprintf(stderr, "Test exception: %s\n", error.what()); }
      catch (...) { failed("Unknown test exception"); }
    // Explicitly surface each cleanup error on success and failure. The
    // Context/kernel retains recovery authority after any ambiguous failure.
    const char* names[] = {"release A", "release B", "release C"};
    for (size_t n = 0; n < buffers.size(); ++n)
        if (buffers[n] && !checked(names[n], buffers[n]->release())) passed = false;
    if (context && !checked("close actual native context", context->close())) passed = false;
    if (passed) std::puts("PASS: actual Context A->B->C, retained B, distinct guards, release rejection and explicit close.");
    return passed;
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    Options selected;
    if (!options(argc, argv, selected)) { usage(argv[0]); return 2; }
    return run(selected) ? 0 : 1;
}
