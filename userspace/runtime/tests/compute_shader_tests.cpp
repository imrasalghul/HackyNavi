// SPDX-License-Identifier: MIT
// Real AIR translator, SPIR-V validator, planner and ACO compiler only.
// This program never constructs Context or opens a GPU.
#include "../compute_shader.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace hackynavi;
namespace {
size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> read(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "open supplied real fixture");
    stream.seekg(0, std::ios::end); const auto bytes = stream.tellg();
    require(bytes > 0 && bytes <= 64 * 1024 * 1024, "bounded real fixture");
    std::vector<uint8_t> result(static_cast<size_t>(bytes)); stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()), bytes);
    require(bool(stream), "read supplied real fixture"); return result;
}
using ShaderResult = std::unique_ptr<HNShaderResult, decltype(&hn_shader_result_free)>;
using ProductResult = std::unique_ptr<HNPipelineResult, decltype(&hn_pipeline_free)>;
struct Observation {
    std::string label;
    std::vector<HNPipelineVariant> variants;
    std::vector<std::vector<uint8_t>> code;
    std::vector<HNPipelineResource> resources;
};
Observation direct(const std::vector<uint8_t>& source, HNShaderInput format, const char* name,
                   const char* compiler, const uint32_t grid[3], const uint32_t local[3]) {
    // Use nominal dispatch dimensions directly, independently of the wrapper's
    // retained initial 1x1x1 specialization. Both must generate identical ISA.
    ShaderResult shader(hn_shader_compile(source.data(), source.size(), format, HN_SHADER_COMPUTE,
        name, local[0], local[1], local[2], HN_SHADER_EXACT_THREADS, 0), hn_shader_result_free);
    require(shader && hn_shader_result_error(shader.get()) && !*hn_shader_result_error(shader.get()),
            "independent actual shader compile");
    ProductResult product(hn_pipeline_build(shader.get(), HN_SHADER_EXACT_THREADS, grid, local, compiler), hn_pipeline_free);
    require(product && hn_pipeline_error(product.get()) && !*hn_pipeline_error(product.get()),
            "independent actual pipeline compile");
    shader.reset();
    HNPipelineInfo info{};
    require(hn_pipeline_info(product.get(), &info, sizeof(info)) == 1, "actual direct product info");
    Observation expected;
    for (uint32_t n = 0; n < info.variant_count; ++n) {
        HNPipelineVariant v{};
        require(hn_pipeline_variant(product.get(), n, &v, sizeof(v)) == 1 && v.code_bytes,
                "actual direct variant info");
        std::vector<uint8_t> code(v.code_bytes);
        require(hn_pipeline_copy_blob(product.get(), HN_PIPELINE_CODE, n, code.data(), code.size()) == 1,
                "independent actual variant bytes");
        expected.variants.push_back(v); expected.code.push_back(std::move(code));
    }
    for (uint32_t n = 0; n < info.resource_count; ++n) {
        HNPipelineResource r{};
        require(hn_pipeline_resource(product.get(), n, &r, sizeof(r)) == 1, "actual direct resource info");
        if (r.access) expected.resources.push_back(r);
    }
    // No borrowed C ABI pointer escapes this function.
    return expected;
}
void compare(const compute::Pipeline& product, const Observation& expected) {
    require(product.variantCount() == expected.variants.size(), "owned product variant count");
    require(product.resources().size() == expected.resources.size() &&
        !std::memcmp(product.resources().data(), expected.resources.data(), expected.resources.size() * sizeof(HNPipelineResource)),
        "owned resources equal independently compiled resources");
    for (uint32_t n = 0; n < product.variantCount(); ++n) {
        HNPipelineVariant v{};
        require(bool(product.variantInfo(n, v)) && !std::memcmp(&v, &expected.variants[n], sizeof(v)),
                "initial 1x1x1 wrapper specializes exact actual variant metadata");
        std::vector<uint8_t> bytes(v.code_bytes, 0xa5);
        require(bool(product.copyCode(n, bytes.data(), bytes.size())) && bytes == expected.code[n],
                "wrapper code equals independently compiled actual ACO bytes");
    }
}
void compileFailure(const void* input, size_t bytes, HNShaderInput format, const std::string& entry,
                    compute::Status expectedStatus, const char* text) {
    std::shared_ptr<const compute::Shader> shader;
    std::string diagnostic = "old diagnostic";
    const auto result = compute::Shader::compile(input, bytes, format, entry, shader, diagnostic);
    require(!result && result.status == expectedStatus && !shader, "failed compile preserves empty output and status");
    require(!diagnostic.empty() && diagnostic != "old diagnostic" && diagnostic.find(text) != std::string::npos,
            "failed compile exposes actual diagnostic");
}
void buildFailure(const compute::Shader& shader, const uint32_t* grid, const uint32_t* local,
                  const std::string& compiler, compute::Status expectedStatus) {
    std::shared_ptr<const compute::Pipeline> pipeline;
    std::string diagnostic = "old diagnostic";
    const auto result = shader.build(grid, local, compiler, pipeline, diagnostic);
    require(!result && result.status == expectedStatus && !pipeline, "failed build preserves empty output and status");
    require(!diagnostic.empty() && diagnostic != "old diagnostic", "failed build replaces stale diagnostic");
}
void negatives(const compute::Shader& shader, const std::shared_ptr<const compute::Pipeline>& existing,
               const char* compiler) {
    const uint32_t grid[3] = {4099,1,1}, local[3] = {64,1,1};
    buildFailure(shader, nullptr, local, compiler, compute::Status::Invalid);
    buildFailure(shader, grid, nullptr, compiler, compute::Status::Invalid);
    const uint32_t badGrids[][3] = {{0,1,1},{1,0,1},{1,1,0},{UINT32_MAX,UINT32_MAX,UINT32_MAX}};
    for (const auto& value : badGrids) buildFailure(shader, value, local, compiler, compute::Status::Invalid);
    const uint32_t badLocals[][3] = {{0,1,1},{1,0,1},{1,1,0},{1025,1,1},{UINT32_MAX,1,1}};
    for (const auto& value : badLocals) buildFailure(shader, grid, value, compiler, compute::Status::Invalid);
    const uint32_t tooMany[3] = {64,32,1};
    buildFailure(shader, grid, tooMany, compiler, compute::Status::Unsupported);
    for (const auto& path : {std::string(),std::string("relative-aco"),std::string(4097,'x'),
                            std::string(compiler)+std::string("\0suffix",7)})
        buildFailure(shader, grid, local, path, compute::Status::Invalid);
    // A nonexistent absolute file reaches the real compiler path error; it is
    // not replaced with a mocked successful compiler response.
    buildFailure(shader, grid, local, std::string(compiler)+".does-not-exist", compute::Status::Unsupported);
    // Windows rejects invalid UTF-8 during u8path conversion; POSIX may pass
    // the byte path to the actual compiler launcher, which must still fail.
    std::shared_ptr<const compute::Pipeline> invalidEncodingOutput;
    std::string encodingDiagnostic;
    const auto encodingResult = shader.build(grid, local, std::string(compiler)+char(0xff), invalidEncodingOutput, encodingDiagnostic);
    require(!encodingResult && (encodingResult.status == compute::Status::Invalid || encodingResult.status == compute::Status::Unsupported) &&
            !invalidEncodingOutput && !encodingDiagnostic.empty(), "invalid encoded compiler path cannot publish a product");
    auto preserved = existing;
    std::string diagnostic;
    require(!shader.build(grid, local, compiler, preserved, diagnostic) && preserved == existing && !diagnostic.empty(),
            "failed build leaves preexisting owned pipeline unchanged");
}
std::vector<Observation> exercise(const std::vector<uint8_t>& source, HNShaderInput format,
                                  const char* name, const char* compiler, bool allShapes) {
    auto mutableSource = source;
    std::string mutableName = name, diagnostic = "old diagnostic";
    std::shared_ptr<const compute::Shader> shader;
    require(bool(compute::Shader::compile(mutableSource.data(), mutableSource.size(), format, mutableName, shader, diagnostic)) &&
            shader && diagnostic.empty(), "actual wrapper compile succeeds");
    std::fill(mutableSource.begin(), mutableSource.end(), 0xcc); mutableSource.clear(); mutableSource.shrink_to_fit();
    std::fill(mutableName.begin(), mutableName.end(), 'x');
    const auto savedReflection = shader->reflectionJson();
    require(shader->name() == name && !savedReflection.empty(), "shader owns original exact name and reflection");
    auto retained = shader;
    require(!compute::Shader::compile(source.data(), source.size(), format, name, retained, diagnostic) &&
            retained == shader && !diagnostic.empty(), "nonempty shader output remains unchanged");
    retained.reset();
    const uint32_t grids[][3] = {{4099,1,1},{4096,1,1},{3,1,1},{65,3,3}};
    const uint32_t locals[][3] = {{64,1,1},{64,1,1},{64,1,1},{64,2,2}};
    const uint32_t expectedCounts[] = {2,1,1,8};
    const char* labels[] = {"full-and-tail","whole-only","tail-only","eight-regions"};
    std::vector<std::shared_ptr<const compute::Pipeline>> pipelines;
    std::vector<Observation> observations;
    for (unsigned n = 0; n < (allShapes ? 4U : 1U); ++n) {
        std::shared_ptr<const compute::Pipeline> pipeline;
        diagnostic = "stale";
        const auto result = shader->build(grids[n], locals[n], compiler, pipeline, diagnostic);
        if (!result) throw std::runtime_error(std::string("actual wrapper build: ") + diagnostic);
        require(pipeline && diagnostic.empty() && pipeline->variantCount() == expectedCounts[n], "actual specialized variant count");
        auto expected = direct(source, format, name, compiler, grids[n], locals[n]);
        expected.label = std::string(name) + "/" + labels[n]; compare(*pipeline, expected);
        pipelines.push_back(std::move(pipeline)); observations.push_back(std::move(expected));
    }
    require(observations[0].variants[0].local_size[0] == 64 && observations[0].variants[1].local_size[0] == 3 &&
            observations[0].code[0] != observations[0].code[1], "real full and tail ISA differ");
    negatives(*shader, pipelines[0], compiler);
    require(shader->name() == name && shader->reflectionJson() == savedReflection, "builds and failures leave immutable shader intact");
    shader.reset();
    std::vector<std::vector<uint8_t>> churn(32, std::vector<uint8_t>(16384, 0x5c));
    require(churn.back().back() == 0x5c, "churn freed source/compiler storage");
    for (size_t n = 0; n < pipelines.size(); ++n) compare(*pipelines[n], observations[n]);
    return observations;
}
std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream output; output << std::hex << std::setfill('0');
    for (auto value : bytes) output << std::setw(2) << unsigned(value);
    return output.str();
}
}
int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: compute-shader-tests ABSOLUTE_ACO APPLE_AIR AFFINE_LL FUNCTION_CONSTANT_LL\n"; return 2;
    }
    try {
        const auto air = read(argv[2]), affine = read(argv[3]), constants = read(argv[4]);
        auto observations = exercise(air, HN_SHADER_AIR, "add_one", argv[1], false);
        auto affineObservations = exercise(affine, HN_SHADER_LLVM_TEXT, "affine_xor", argv[1], true);
        require(observations[0].code[0] != affineObservations[0].code[0], "distinct kernels produce distinct actual ISA");
        observations.insert(observations.end(), affineObservations.begin(), affineObservations.end());
        compileFailure(nullptr, air.size(), HN_SHADER_AIR, "add_one", compute::Status::Invalid, "Invalid");
        compileFailure(air.data(), 0, HN_SHADER_AIR, "add_one", compute::Status::Invalid, "Invalid");
        compileFailure(air.data(), 64U*1024U*1024U+1U, HN_SHADER_AIR, "add_one", compute::Status::Invalid, "Invalid");
        compileFailure(reinterpret_cast<const void*>(UINTPTR_MAX-1), 4, HN_SHADER_AIR, "add_one", compute::Status::Invalid, "Invalid");
        compileFailure(air.data(), air.size(), static_cast<HNShaderInput>(3), "add_one", compute::Status::Invalid, "Invalid");
        for (const auto& name : {std::string(),std::string(1025,'x'),std::string("add_one\0suffix",14)})
            compileFailure(air.data(), air.size(), HN_SHADER_AIR, name, compute::Status::Invalid, "Invalid");
        compileFailure(air.data(), air.size(), HN_SHADER_AIR, "other_entry", compute::Status::Unsupported, "entry point mismatch");
        compileFailure(constants.data(), constants.size(), HN_SHADER_LLVM_TEXT, "partial_write", compute::Status::Unsupported, "function constants");
        const std::string malformed = "not LLVM IR or AIR";
        compileFailure(malformed.data(), malformed.size(), HN_SHADER_LLVM_TEXT, "broken", compute::Status::Unsupported, "");
        compileFailure(air.data(), 8, HN_SHADER_AIR, "add_one", compute::Status::Unsupported, "");
        std::cout << "{\"success\":true,\"hardware_executed\":false,\"context_open_called\":false,\"checks\":" << checks << ",\"cases\":[";
        for (size_t n = 0; n < observations.size(); ++n) {
            if (n) std::cout << ',';
            const auto& item = observations[n];
            std::cout << "{\"name\":\"" << item.label << "\",\"variants\":[";
            for (size_t v = 0; v < item.variants.size(); ++v) {
                if (v) std::cout << ',';
                const auto& metadata = item.variants[v];
                std::cout << "{\"local\":[" << metadata.local_size[0] << ',' << metadata.local_size[1] << ',' << metadata.local_size[2]
                          << "],\"code_bytes\":" << metadata.code_bytes << ",\"code_hex\":\"" << hex(item.code[v]) << "\"}";
            }
            std::cout << "]}";
        }
        std::cout << "]}\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "compute shader tests failed after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
