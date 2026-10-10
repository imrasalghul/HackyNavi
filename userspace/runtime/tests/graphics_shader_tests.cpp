// SPDX-License-Identifier: MIT
// Real compiler only. No Context, Metal object, device or GPU is constructed.
#include "../graphics_shader.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace hackynavi { namespace compute { namespace detail {
// Internal schema checker: mutate actual compiler reflection, without replacing
// the compiler or injecting a fabricated successful result into GraphicsShader.
bool graphicsReflectionAdmitted(const std::string&, HNShaderStage, const std::string&);
} } }
using namespace hackynavi::compute;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
std::vector<uint8_t> read(const char* path) {
    std::ifstream stream(path, std::ios::binary); check(bool(stream), "actual fixture opens");
    stream.seekg(0, std::ios::end); auto count = stream.tellg();
    check(count > 0 && count < 64 * 1024 * 1024, "actual bounded fixture");
    std::vector<uint8_t> data(static_cast<size_t>(count)); stream.seekg(0);
    stream.read(reinterpret_cast<char*>(data.data()), count); check(bool(stream), "fixture read"); return data;
}
std::string replace(const std::string& text, const std::string& from, const std::string& to) {
    auto at = text.find(from); check(at != std::string::npos, "mutation targets real reflected field");
    auto changed = text; changed.replace(at, from.size(), to); return changed;
}
void reflectionChecks(const GraphicsShader& shader) {
    const auto& json = shader.reflectionJson(); const auto stage = shader.stage(); const auto& name = shader.name();
    const auto admit = [&](const std::string& text) { return detail::graphicsReflectionAdmitted(text, stage, name); };
    check(admit(json), "actual reflection accepted");
    check(admit(" \n\t" + json + "\r\n"), "JSON whitespace accepted");
    for (size_t n = 0; n < json.size(); ++n) check(!admit(json.substr(0, n)), "every truncation rejected");
    for (const char* key : {"bindings", "argument_buffer_fields", "vertex_attributes", "varyings", "depth_members",
        "stencil_members", "imageblock_layouts", "implicit_imageblock_attachments", "runtime_sampler_specializations",
        "runtime_storage_image_specializations", "function_constants"}) {
        std::string field = std::string("\"") + key + "\":[]";
        check(!admit(replace(json, field, std::string("\"") + key + "\":[0]")), "resource/attribute entry rejected");
        check(!admit(replace(json, field, std::string("\"") + key + "\":null")), "wrong array type rejected");
    }
    for (const char* key : {"depth_qualifier", "local_size", "max_work_group_size", "kernel_dispatch",
        "fragment_sample_positions", "tessellation", "fragment_imageblock"}) {
        std::string field = std::string("\"") + key + "\":null";
        check(!admit(replace(json, field, std::string("\"") + key + "\":{}")), "unsupported optional metadata rejected");
    }
    check(!admit(replace(json, "\"reflection_version\":56", "\"reflection_version\":57")), "unknown version rejected");
    check(!admit(replace(json, "\"reflection_version\":56", "\"reflection_version\":056")), "invalid JSON number rejected");
    check(!admit(replace(json, "\"reflection_version\":56", "\"reflection_version\":56,\"reflection_version\":56")), "duplicate rejected");
    check(!admit(replace(json, "\"reflection_version\":56", "\"reflection_version\":56,\"future_resource\":[]")), "unknown root rejected");
    check(!admit(json + "null"), "trailing JSON rejected");
    check(!admit(replace(json, "\"entry_point\":\"" + name + "\"", "\"entry_point\":\"other\"")), "name mismatch rejected");
    check(!detail::graphicsReflectionAdmitted(json, HN_SHADER_COMPUTE, name), "compute excluded");
    check(!admit(std::string(1024 * 1024 + 1, ' ')), "bounded reflection rejected");
    if (stage == HN_SHADER_VERTEX) {
        check(!admit(replace(json, "\"uses_instance_index\":false", "\"uses_instance_index\":true")), "instance input rejected");
        check(!admit(replace(json, "\"writes_position\":true", "\"writes_position\":false")), "missing position rejected");
        check(admit(replace(json, "\"uses_vertex_index\":true", "\"uses_vertex_index\":false")), "literal position remains valid");
    } else {
        check(!admit(replace(json, "\"location\":0", "\"location\":1")), "other color target rejected");
        check(!admit(replace(json, "\"type_name\":\"float4\"", "\"type_name\":\"half4\"")), "different color contract rejected");
    }
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 8, "supply AIR, metallib, actual VS/PS SPIR-V, explicit dis/val, SDL metallib");
        const auto air = read(argv[1]), library = read(argv[2]);
        const std::vector<uint8_t> expected[] = {read(argv[3]), read(argv[4])};
        const Shader::Tools valid{argv[5], argv[6]};
        std::shared_ptr<const GraphicsShader> retained[2]; std::string diagnostic;
        for (unsigned stageIndex = 0; stageIndex < 2; ++stageIndex) {
            const auto stage = stageIndex ? HN_SHADER_FRAGMENT : HN_SHADER_VERTEX;
            const std::string name = stageIndex ? "hn_constant_fragment" : "hn_triangle_vertex";
            for (unsigned container = 0; container < 2; ++container) {
                auto input = container ? library : air; auto tools = valid;
                std::shared_ptr<const GraphicsShader> shader;
                check(bool(GraphicsShader::compile(input.data(), input.size(), container ? HN_SHADER_METALLIB : HN_SHADER_AIR,
                    stage, name, tools, shader, diagnostic)), "actual AIR/metallib graphics compilation");
                check(shader && shader->stage() == stage && shader->name() == name && shader->spirvEntryPoint() == "main" && diagnostic.empty(), "actual immutable metadata");
                std::fill(input.begin(), input.end(), 0xff); input.clear(); input.shrink_to_fit();
                tools.airDisassembler.clear(); tools.spirvValidator = "invalid-relative";
                const auto& words = shader->spirvWords();
                check(words.size() * 4 == expected[stageIndex].size() &&
                    !std::memcmp(words.data(), expected[stageIndex].data(), expected[stageIndex].size()), "owned SPIR-V equals independently captured actual compiler output after input destruction");
                reflectionChecks(*shader);
                auto same = shader;
                check(!GraphicsShader::compile(air.data(), air.size(), HN_SHADER_AIR, stage, name, valid, shader, diagnostic) && shader == same, "nonempty output unchanged");
                retained[stageIndex] = std::move(shader);
            }
        }
        for (const auto& path : {std::string(), std::string("relative"), std::string(4097, 'x'), std::string("x\0y", 3)}) {
            Shader::Tools bad{path, valid.spirvValidator}; std::shared_ptr<const GraphicsShader> empty;
            check(!GraphicsShader::compile(air.data(), air.size(), HN_SHADER_AIR, HN_SHADER_VERTEX, "hn_triangle_vertex", bad, empty, diagnostic) && !empty && !diagnostic.empty(), "invalid explicit tools cannot fallback");
        }
        std::shared_ptr<const GraphicsShader> empty;
        check(!GraphicsShader::compile(nullptr, air.size(), HN_SHADER_AIR, HN_SHADER_VERTEX, "hn_triangle_vertex", valid, empty, diagnostic) && !empty, "null input rejected");
        check(!GraphicsShader::compile(reinterpret_cast<void*>(UINTPTR_MAX - 3), 16, HN_SHADER_AIR, HN_SHADER_VERTEX, "hn_triangle_vertex", valid, empty, diagnostic), "overflowing span rejected before access");
        check(!GraphicsShader::compile(air.data(), air.size(), HN_SHADER_AIR, HN_SHADER_COMPUTE, "hn_triangle_vertex", valid, empty, diagnostic), "compute stage rejected");
        check(!GraphicsShader::compile(library.data(), library.size(), HN_SHADER_METALLIB, HN_SHADER_FRAGMENT, "hn_triangle_vertex", valid, empty, diagnostic), "actual container stage mismatch rejected");
        check(!GraphicsShader::compile(air.data(), air.size(), HN_SHADER_AIR, HN_SHADER_VERTEX, "missing_entry", valid, empty, diagnostic), "actual entry mismatch rejected");
        const auto sdl = read(argv[7]);
        check(!GraphicsShader::compile(sdl.data(), sdl.size(), HN_SHADER_METALLIB, HN_SHADER_FRAGMENT, "SDL_Solid_fragment", valid, empty, diagnostic) && !empty, "actual SDL varying fragment unsupported");
        std::cout << "{\"success\":true,\"checks\":" << checks << ",\"gpu_executed\":false,\"metal_objects_created\":false}\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
