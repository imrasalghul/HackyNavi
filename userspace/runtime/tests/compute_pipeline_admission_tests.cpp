// SPDX-License-Identifier: MIT
// Actual AIR/metallib -> translator -> ACO -> owned product -> shared ISA
// admission. No Metal object, Context, user client or GPU is opened here.
#include "../compute_shader.hpp"
#include "hackynavi/gfx11_compute_product.hpp"
#include "hackynavi/gfx11_occupancy.hpp"
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace hackynavi;
namespace {
size_t checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> read(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "open actual fixture");
    stream.seekg(0, std::ios::end); const auto size = stream.tellg();
    require(size > 0 && size <= 64 * 1024 * 1024, "bounded actual fixture");
    std::vector<uint8_t> bytes(static_cast<size_t>(size)); stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    require(bool(stream), "read actual fixture"); return bytes;
}
std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream output; output << std::hex << std::setfill('0');
    for (auto value : bytes) output << std::setw(2) << unsigned(value);
    return output.str();
}
struct Variant {
    HNPipelineVariant metadata{};
    gfx11product::Report admission{};
    std::vector<uint8_t> code;
};
struct Observation { std::string name; std::vector<Variant> variants; };
Observation exercise(const std::vector<uint8_t>& source, HNShaderInput format, const char* entry,
                     const char* compiler, const char* label, const uint32_t grid[3],
                     const uint32_t local[3], uint32_t count) {
    std::shared_ptr<const compute::Shader> shader;
    std::shared_ptr<const compute::Pipeline> product;
    std::string diagnostic;
    const auto compiled = compute::Shader::compile(source.data(), source.size(), format, entry, shader, diagnostic);
    require(bool(compiled) && shader && diagnostic.empty(), std::string(label) + " actual shader: " + diagnostic);
    const auto built = shader->build(grid, local, compiler, product, diagnostic);
    require(bool(built) && product && diagnostic.empty(), std::string(label) + " actual product: " + diagnostic);
    shader.reset(); // Validate the actual owned copy after translator ownership ends.
    require(product->variantCount() == count, "actual full/tail variant count");
    Observation result{label, {}};
    uint32_t reads = 0, writes = 0;
    for (uint32_t index = 0; index < count; ++index) {
        Variant value;
        auto& v = value.metadata;
        require(bool(product->variantInfo(index, v)), "actual variant metadata");
        value.code.resize(v.code_bytes);
        require(bool(product->copyCode(index, value.code.data(), value.code.size())), "actual owned variant code");
        gfx11product::Request request{value.code.data(), value.code.size(), v.executable_bytes,
            {v.local_size[0], v.local_size[1], v.local_size[2]}, v.wave_size, v.num_sgprs, v.num_vgprs};
        const auto status = gfx11product::validate(request, value.admission);
        std::ostringstream failure;
        failure << label << " variant " << index << " admission " << gfx11product::statusString(status)
                << " at byte " << value.admission.byteOffset << " word 0x" << std::hex << value.admission.instructionWord;
        require(status == gfx11product::Status::Ok, failure.str());
        const auto& a = value.admission;
        require(a.rsrc1 == v.compute_pgm_rsrc[0] && a.rsrc2 == v.compute_pgm_rsrc[1] && a.rsrc3 == v.compute_pgm_rsrc[2],
                "decoded launch registers equal actual ACO registers");
        require(a.readMask == v.descriptor_read_mask && a.writeMask == v.descriptor_write_mask &&
                (a.readMask | a.writeMask) == v.descriptor_binding_mask,
                "decoded buffer access equals actual compiler access");
        const uint64_t threads = uint64_t(v.local_size[0]) * v.local_size[1] * v.local_size[2];
        require(threads <= 1024 && gfx11occupancy::fits(v.num_vgprs, uint32_t(threads)),
                "actual compiled allocation fits WGP occupancy");
        require(a.instructionCount && a.requiredSgprs <= v.num_sgprs && a.requiredVgprs <= v.num_vgprs,
                "actual decoder register usage fits declared allocation");
        reads |= a.readMask; writes |= a.writeMask;
        // Corrupt an owned copy of actual compiler output: failed admission
        // must not preserve the preceding successful launch/access report.
        auto corrupted = value.code;
        std::memset(corrupted.data(), 0xff, 4); request.code = corrupted.data();
        auto rejected = a;
        require(gfx11product::validate(request, rejected) != gfx11product::Status::Ok &&
                !rejected.instructionCount && !rejected.requiredSgprs && !rejected.requiredVgprs &&
                !rejected.readMask && !rejected.writeMask && !rejected.rsrc1 && !rejected.rsrc2 && !rejected.rsrc3,
                "mutated actual ISA cannot publish stale success fields");
        result.variants.push_back(std::move(value));
    }
    uint32_t resourceMask = 0;
    for (const auto& resource : product->resources()) {
        require(resource.descriptor_set == 0 && resource.binding < 32, "actual ordinary buffer resource");
        const uint32_t bit = 1U << resource.binding;
        require(!(resourceMask & bit), "actual unique resource binding");
        resourceMask |= bit;
        const uint32_t access = ((reads & bit) ? 1U : 0U) | ((writes & bit) ? 2U : 0U);
        require(access && resource.access == access && (resource.reflected_access & access) == access,
                "aggregate ISA access matches owned AIR resource reflection");
    }
    require(resourceMask == (reads | writes), "all decoded accesses have actual reflected resources");
    return result;
}
}
int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: compute-pipeline-admission-tests ABSOLUTE_ACO APPLE_AIR AFFINE_LL MODERN_AIR MODERN_METALLIB\n";
        return 2;
    }
    try {
        const auto apple = read(argv[2]), affine = read(argv[3]), modern = read(argv[4]), library = read(argv[5]);
        const uint32_t one[3] = {1,1,1}, grid[3] = {4099,1,1}, local[3] = {64,1,1};
        const uint32_t grid3d[3] = {65,3,3}, local3d[3] = {64,2,2};
        std::vector<Observation> results;
        results.push_back(exercise(apple,HN_SHADER_AIR,"add_one",argv[1],"apple/prototype1",one,one,1));
        results.push_back(exercise(apple,HN_SHADER_AIR,"add_one",argv[1],"apple/full-tail",grid,local,2));
        results.push_back(exercise(affine,HN_SHADER_LLVM_TEXT,"affine_xor",argv[1],"affine/prototype1",one,one,1));
        results.push_back(exercise(affine,HN_SHADER_LLVM_TEXT,"affine_xor",argv[1],"affine/full-tail",grid,local,2));
        results.push_back(exercise(affine,HN_SHADER_LLVM_TEXT,"affine_xor",argv[1],"affine/eight-regions",grid3d,local3d,8));
        for (bool prototype : {true, false}) {
            const auto* g = prototype ? one : grid; const auto* l = prototype ? one : local;
            const auto count = prototype ? 1U : 2U;
            auto air = exercise(modern,HN_SHADER_AIR,"add_one",argv[1],prototype ? "modern-air/prototype1" : "modern-air/full-tail",g,l,count);
            auto metallib = exercise(library,HN_SHADER_METALLIB,"add_one",argv[1],prototype ? "metallib/prototype1" : "metallib/full-tail",g,l,count);
            for (size_t n = 0; n < air.variants.size(); ++n) {
                require(air.variants[n].code == metallib.variants[n].code &&
                    !std::memcmp(&air.variants[n].metadata,&metallib.variants[n].metadata,sizeof(HNPipelineVariant)),
                    "actual macOS AIR and exact library function produce identical admitted ISA");
            }
            results.push_back(std::move(air)); results.push_back(std::move(metallib));
        }
        std::cout << "{\"success\":true,\"hardware_executed\":false,\"context_open_called\":false,\"metal_object_created\":false,\"checks\":" << checks << ",\"cases\":[";
        for (size_t n = 0; n < results.size(); ++n) {
            if (n) std::cout << ',';
            std::cout << "{\"name\":\"" << results[n].name << "\",\"variants\":[";
            for (size_t i = 0; i < results[n].variants.size(); ++i) {
                if (i) std::cout << ',';
                const auto& v = results[n].variants[i]; const auto& m = v.metadata;
                std::cout << "{\"local\":[" << m.local_size[0] << ',' << m.local_size[1] << ',' << m.local_size[2]
                    << "],\"code_bytes\":" << m.code_bytes << ",\"code_hex\":\"" << hex(v.code) << "\",\"admission\":\"Ok\",\"read_mask\":"
                    << v.admission.readMask << ",\"write_mask\":" << v.admission.writeMask << '}';
            }
            std::cout << "]}";
        }
        std::cout << "]}\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "compute pipeline admission tests failed after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
