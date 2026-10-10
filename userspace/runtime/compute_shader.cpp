// SPDX-License-Identifier: MIT
#include "compute_shader.hpp"
#include <filesystem>
#include <new>
#include <stdexcept>

namespace hackynavi { namespace compute {
namespace {
Result fail(Status status, std::string& diagnostic, const char* message) {
    diagnostic = message; return {status, 0};
}
bool boundedText(const std::string& value, size_t maximum) {
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}
}
Result Shader::compile(const void* input, size_t bytes, HNShaderInput format,
                       const std::string& entry, std::shared_ptr<const Shader>& out,
                       std::string& diagnostic) {
    return compileImpl(input,bytes,format,entry,nullptr,out,diagnostic);
}
Result Shader::compile(const void* input, size_t bytes, HNShaderInput format,
                       const std::string& entry,const Tools& tools,
                       std::shared_ptr<const Shader>& out,std::string& diagnostic) {
    return compileImpl(input,bytes,format,entry,&tools,out,diagnostic);
}
Result Shader::compileImpl(const void* input, size_t bytes, HNShaderInput format,
                       const std::string& entry,const Tools* tools,
                       std::shared_ptr<const Shader>& out,std::string& diagnostic) {
    diagnostic.clear();
    try {
        if (out || !input || !bytes || bytes > 64U * 1024U * 1024U ||
            bytes > UINTPTR_MAX - reinterpret_cast<uintptr_t>(input) || !boundedText(entry, 1024) ||
            (format != HN_SHADER_AIR && format != HN_SHADER_LLVM_TEXT && format != HN_SHADER_METALLIB))
            return fail(Status::Invalid, diagnostic, "Invalid compute shader input or nonempty output.");
        if (hn_shader_abi_version() != 1 || hn_pipeline_abi_version() != 1)
            return fail(Status::Unsupported, diagnostic, "Unsupported shader/compiler ABI.");
        if (tools && (!boundedText(tools->airDisassembler,4096)||!boundedText(tools->spirvValidator,4096)))
            return fail(Status::Invalid,diagnostic,"Explicit compiler tools require bounded nonempty paths.");
        auto result = std::shared_ptr<Shader>(new Shader);
        // Exact-thread mode emits specialization IDs, so this initial 1x1x1
        // shape does not fix the later workgroup size. The pipeline compiler
        // specializes each actual dispatch region and validates AIR limits.
        if (tools) {
            const HNShaderTools paths={sizeof(HNShaderTools),1,tools->airDisassembler.c_str(),tools->spirvValidator.c_str()};
            result->shader_.reset(hn_shader_compile_with_tools(&paths,static_cast<const uint8_t*>(input),bytes,
                format,HN_SHADER_COMPUTE,entry.c_str(),1,1,1,HN_SHADER_EXACT_THREADS,0));
        } else {
            result->shader_.reset(hn_shader_compile(static_cast<const uint8_t*>(input), bytes,
                format, HN_SHADER_COMPUTE, entry.c_str(), 1, 1, 1, HN_SHADER_EXACT_THREADS, 0));
        }
        if (!result->shader_)
            return fail(Status::OutOfMemory, diagnostic, "Shader compiler returned no owned result.");
        const char* error = hn_shader_result_error(result->shader_.get());
        if (!error) return fail(Status::Protocol, diagnostic, "Shader result has no status.");
        if (*error) return fail(Status::Unsupported, diagnostic, error);
        size_t words = 0;
        const auto* spirv = hn_shader_result_words(result->shader_.get(), &words);
        const char* reflection = hn_shader_result_reflection(result->shader_.get());
        const char* spirvEntry = hn_shader_result_spirv_entry_point(result->shader_.get());
        if (!spirv || words < 5 || spirv[0] != 0x07230203 || !reflection || !*reflection ||
            !spirvEntry || !*spirvEntry)
            return fail(Status::Protocol, diagnostic, "Successful shader lacks actual code or reflection.");
        result->name_ = entry;
        result->reflection_ = reflection;
        out = std::move(result);
        return {Status::Ok, 0};
    } catch (const std::bad_alloc&) { diagnostic.clear(); return {Status::OutOfMemory, 0}; }
}
Result Shader::build(const uint32_t grid[3], const uint32_t local[3],
                     const std::string& compiler, std::shared_ptr<const Pipeline>& out,
                     std::string& diagnostic) const {
    diagnostic.clear();
    try {
        if (out || !grid || !local || !boundedText(compiler, 4096))
            return fail(Status::Invalid, diagnostic, "Expected an absolute ACO path, dimensions and empty output.");
        // Some standard libraries report malformed UTF-8 conversion as plain
        // runtime_error rather than filesystem_error. Keep this conversion's
        // failure inside the API; do not catch unrelated compiler exceptions.
        bool absolute = false;
        try { absolute = std::filesystem::u8path(compiler).is_absolute(); }
        catch (const std::runtime_error&) {
            return fail(Status::Invalid, diagnostic, "Invalid or unsupported compiler path encoding.");
        }
        if (!absolute)
            return fail(Status::Invalid, diagnostic, "The ACO compiler path must be absolute.");
        uint64_t groupThreads = 1, gridThreads = 1;
        for (unsigned d = 0; d < 3; ++d) {
            if (!grid[d] || !local[d] || local[d] > nativeabi::kMaxProductLocalThreads ||
                gridThreads > UINT64_MAX / grid[d])
                return fail(Status::Invalid, diagnostic, "Empty or overflowing dispatch dimensions.");
            groupThreads *= local[d]; gridThreads *= grid[d];
        }
        if (groupThreads > nativeabi::kMaxProductLocalThreads)
            return fail(Status::Unsupported, diagnostic, "Workgroup exceeds native compute limits.");
        std::unique_ptr<HNPipelineResult, decltype(&hn_pipeline_free)> product(
            hn_pipeline_build(shader_.get(), HN_SHADER_EXACT_THREADS, grid, local, compiler.c_str()),
            hn_pipeline_free);
        if (!product) return fail(Status::OutOfMemory, diagnostic, "Pipeline compiler returned no owned result.");
        const char* error = hn_pipeline_error(product.get());
        if (!error) return fail(Status::Protocol, diagnostic, "Pipeline result has no status.");
        if (*error) return fail(Status::Unsupported, diagnostic, error);
        auto status = Pipeline::capture(product.get(), grid, local, out);
        if (!status) diagnostic = "Compiled pipeline does not satisfy the native compute product contract.";
        return status;
    } catch (const std::bad_alloc&) { diagnostic.clear(); return {Status::OutOfMemory, 0}; }
}
} }
