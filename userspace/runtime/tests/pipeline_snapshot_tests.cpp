// SPDX-License-Identifier: MIT
// Actual compiler/planner/ACO products only. No Context::open, GPU connection,
// generated substitute code, injected transport or CPU rendering backend.
#include "../compute_context.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace hackynavi;
namespace {
size_t checks = 0;
void require(bool condition, const char* reason) {
    ++checks; if (!condition) throw std::runtime_error(reason);
}
std::vector<uint8_t> read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "open supplied real source");
    input.seekg(0, std::ios::end); const auto size = input.tellg();
    require(size > 0 && size <= 64 * 1024 * 1024, "bounded actual source size");
    std::vector<uint8_t> data(static_cast<size_t>(size)); input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), size);
    require(bool(input), "read actual source"); return data;
}
using Shader = std::unique_ptr<HNShaderResult, decltype(&hn_shader_result_free)>;
using Product = std::unique_ptr<HNPipelineResult, decltype(&hn_pipeline_free)>;
Shader compile(std::vector<uint8_t> data, const char* entry, uint32_t format, uint32_t mode) {
    Shader shader(hn_shader_compile(data.data(), data.size(), format, HN_SHADER_COMPUTE,
                                   entry, 64, 1, 1, mode, 0), hn_shader_result_free);
    require(bool(shader), "real shader compiler returned null");
    const char* error = hn_shader_result_error(shader.get());
    if (!error || *error) throw std::runtime_error(std::string("actual shader compile: ") + (error ? error : "null error"));
    std::fill(data.begin(), data.end(), 0xcc); // Compiler cannot keep caller input.
    return shader;
}
Product build(const Shader& shader, const char* compiler, uint32_t mode,
              const uint32_t grid[3], const uint32_t local[3]) {
    Product product(hn_pipeline_build(shader.get(), mode, grid, local, compiler), hn_pipeline_free);
    require(bool(product), "real pipeline builder returned null");
    const char* error = hn_pipeline_error(product.get());
    if (!error || *error) throw std::runtime_error(std::string("actual ACO build: ") + (error ? error : "null error"));
    return product;
}
std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (auto b : bytes) out << std::setw(2) << unsigned(b);
    return out.str();
}
void rejected(const HNPipelineResult* product, const uint32_t* grid, const uint32_t* local) {
    std::shared_ptr<const compute::Pipeline> captured;
    const auto result = compute::Pipeline::capture(product, grid, local, captured);
    require(!result && !captured, "invalid capture must not publish a snapshot");
}
struct Observation { std::string name; std::vector<HNPipelineVariant> variants; std::vector<std::vector<uint8_t>> code; };
Observation exercise(const char* name, const std::vector<uint8_t>& data, uint32_t format,
                     const char* entry, const char* compiler, const uint32_t grid[3],
                     const uint32_t local[3], unsigned expectedVariants, bool negatives) {
    auto shader = compile(data, entry, format, HN_SHADER_EXACT_THREADS);
    auto product = build(shader, compiler, HN_SHADER_EXACT_THREADS, grid, local);
    shader.reset(); // C ABI pipeline must already own all original shader data.
    HNPipelineInfo info{};
    require(hn_pipeline_info(product.get(), &info, sizeof(info)) == 1, "read real product info");
    require(info.variant_count == expectedVariants && info.region_count == expectedVariants && info.resource_count == 2,
            "actual variants/resources match dispatch");
    Observation observed{name, {}, {}};
    std::vector<HNPipelineResource> resources;
    for (uint32_t n = 0; n < info.variant_count; ++n) {
        HNPipelineVariant v{};
        require(hn_pipeline_variant(product.get(), n, &v, sizeof(v)) == 1, "real variant metadata");
        const size_t size = hn_pipeline_blob_size(product.get(), HN_PIPELINE_CODE, n);
        require(size == v.code_bytes && size > 0, "actual code extent");
        std::vector<uint8_t> bytes(size);
        require(hn_pipeline_copy_blob(product.get(), HN_PIPELINE_CODE, n, bytes.data(), size) == 1, "save independent real code bytes");
        observed.variants.push_back(v); observed.code.push_back(std::move(bytes));
    }
    for (uint32_t n = 0; n < info.resource_count; ++n) {
        HNPipelineResource r{};
        require(hn_pipeline_resource(product.get(), n, &r, sizeof(r)) == 1, "real resource metadata");
        if (r.access) resources.push_back(r);
    }
    require(resources.size() == 2, "actual two accessed resources");
    std::shared_ptr<const compute::Pipeline> captured;
    require(bool(compute::Pipeline::capture(product.get(), grid, local, captured)) && captured, "capture real product");
    if (negatives) {
        rejected(nullptr, grid, local); rejected(product.get(), nullptr, local); rejected(product.get(), grid, nullptr);
        const uint32_t badGrids[][3] = {{0,1,1},{4098,1,1},{4099,2,1},{UINT32_MAX,UINT32_MAX,UINT32_MAX}};
        for (const auto& bad : badGrids) rejected(product.get(), bad, local);
        const uint32_t badLocals[][3] = {{0,1,1},{63,1,1},{64,0,1},{64,1,0},{1025,1,1},{64,32,1},{UINT32_MAX,1,1}};
        for (const auto& bad : badLocals) rejected(product.get(), grid, bad);
        auto same = captured;
        require(!compute::Pipeline::capture(product.get(), grid, local, same) && same == captured, "nonempty output unchanged");
        Product error(hn_pipeline_build(nullptr, HN_SHADER_EXACT_THREADS, grid, local, compiler), hn_pipeline_free);
        require(error && hn_pipeline_error(error.get()) && *hn_pipeline_error(error.get()), "actual failed pipeline object");
        rejected(error.get(), grid, local);
    }
    product.reset(); // All subsequent bytes must come from Pipeline's ownership.
    // Reuse independent heap storage to make accidental borrowed source storage
    // observable; sanitizers instrument the production snapshot implementation.
    std::vector<std::vector<uint8_t>> churn(32, std::vector<uint8_t>(16384, 0xa5));
    require(churn.back().back() == 0xa5, "heap storage populated");
    require(captured->variantCount() == observed.variants.size(), "retained variant count");
    require(captured->resources().size() == resources.size() &&
            !std::memcmp(captured->resources().data(), resources.data(), resources.size() * sizeof(HNPipelineResource)),
            "retained real resource records after original free");
    for (uint32_t n = 0; n < captured->variantCount(); ++n) {
        HNPipelineVariant v{};
        require(bool(captured->variantInfo(n, v)) && !std::memcmp(&v, &observed.variants[n], sizeof(v)), "retained variant metadata");
        std::vector<uint8_t> copied(v.code_bytes, 0);
        require(bool(captured->copyCode(n, copied.data(), copied.size())) && copied == observed.code[n],
                "copied actual code equals pre-free independent bytes");
        copied[0] ^= 0xff;
        require(bool(captured->copyCode(n, copied.data(), copied.size())) && copied == observed.code[n],
                "caller mutation cannot alter owned code");
        std::fill(copied.begin(), copied.end(), 0x5c); const auto before = copied;
        require(!captured->copyCode(n, copied.data(), copied.size() - 1) && copied == before, "short destination unchanged");
        require(!captured->copyCode(n, copied.data(), copied.size() + 1) && copied == before, "oversized destination unchanged");
        require(!captured->copyCode(n, nullptr, copied.size()), "null destination rejected");
    }
    HNPipelineVariant sentinel; std::memset(&sentinel, 0x5c, sizeof(sentinel)); const auto before = sentinel;
    require(!captured->variantInfo(captured->variantCount(), sentinel) && !std::memcmp(&sentinel, &before, sizeof(before)),
            "invalid metadata index leaves output unchanged");
    uint32_t word = 0xabcdef01;
    require(!captured->copyCode(UINT32_MAX, &word, sizeof(word)) && word == 0xabcdef01, "invalid code index leaves output unchanged");
    captured.reset(); return observed;
}
}
int main(int argc, char** argv) {
    if (argc != 5) { std::cerr << "usage: pipeline-snapshot-tests ABSOLUTE_ACO APPLE_AIR AFFINE_LL REORDERED_AFFINE_LL\n"; return 2; }
    try {
        require(hn_shader_abi_version() == 1 && hn_pipeline_abi_version() == 1, "actual compiler ABI");
        const auto air = read(argv[2]), affine = read(argv[3]), reordered = read(argv[4]);
        const uint32_t grid[3] = {4099,1,1}, local[3] = {64,1,1};
        std::vector<Observation> cases;
        cases.push_back(exercise("apple-add-one", air, HN_SHADER_AIR, "add_one", argv[1], grid, local, 2, true));
        cases.push_back(exercise("affine-xor", affine, HN_SHADER_LLVM_TEXT, "affine_xor", argv[1], grid, local, 2, true));
        cases.push_back(exercise("affine-reordered-arguments", reordered, HN_SHADER_LLVM_TEXT, "affine_xor", argv[1], grid, local, 2, false));
        for (const auto& test : cases) {
            require(test.variants[0].local_size[0] == 64 && test.variants[1].local_size[0] == 3, "real full/tail variants");
            require(test.code[0] != test.code[1], "actual tail specialization differs");
        }
        require(cases[0].code[0] != cases[1].code[0], "distinct kernels compile distinct ISA");
        const uint32_t grid3[3] = {65,3,3}, local3[3] = {64,2,2};
        cases.push_back(exercise("affine-eight-regions", affine, HN_SHADER_LLVM_TEXT, "affine_xor", argv[1], grid3, local3, 8, false));
        auto wholeShader = compile(affine, "affine_xor", HN_SHADER_LLVM_TEXT, HN_SHADER_WHOLE_WORKGROUPS);
        const uint32_t groups[3] = {65,1,1};
        auto wholeProduct = build(wholeShader, argv[1], HN_SHADER_WHOLE_WORKGROUPS, groups, local);
        rejected(wholeProduct.get(), grid, local); rejected(wholeProduct.get(), groups, local);
        std::cout << "{\"success\":true,\"hardware_executed\":false,\"context_open_called\":false,\"checks\":" << checks << ",\"cases\":[";
        for (size_t c = 0; c < cases.size(); ++c) {
            if (c) std::cout << ',';
            std::cout << "{\"name\":\"" << cases[c].name << "\",\"variants\":[";
            for (size_t n = 0; n < cases[c].variants.size(); ++n) {
                if (n) std::cout << ',';
                const auto& v = cases[c].variants[n];
                std::cout << "{\"local\":[" << v.local_size[0] << ',' << v.local_size[1] << ',' << v.local_size[2]
                          << "],\"code_bytes\":" << v.code_bytes << ",\"code_hex\":\"" << hex(cases[c].code[n]) << "\"}";
            }
            std::cout << "]}";
        }
        std::cout << "]}\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipeline snapshot test failed after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
