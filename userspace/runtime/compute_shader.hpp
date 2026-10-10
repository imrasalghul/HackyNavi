// SPDX-License-Identifier: MIT
#pragma once
#include "compute_context.hpp"
#include <string>

namespace hackynavi { namespace compute {
// Immutable, actual translated compute shader. This is independent of device
// discovery and never opens a GPU. Embedded callers supply explicit trusted
// tools; no process-global environment is changed by these methods.
class Shader final {
public:
    struct Tools { std::string airDisassembler,spirvValidator; };
    // Input is borrowed only during compilation. entry is the exact UTF-8 AIR
    // name, not a renamed SPIR-V entry. Errors leave out unchanged and carry
    // the real compiler diagnostic; allocation failure may leave it empty.
    static Result compile(const void* input, size_t bytes, HNShaderInput format,
                          const std::string& entry, std::shared_ptr<const Shader>& out,
                          std::string& diagnostic);
    // Embedded-driver entrypoint. Paths are canonicalized and owned by the
    // actual compiler shader, including later pipeline validation. No fallback
    // to the legacy overload or environment/PATH occurs on invalid tools.
    static Result compile(const void* input, size_t bytes, HNShaderInput format,
                          const std::string& entry, const Tools& tools,
                          std::shared_ptr<const Shader>& out, std::string& diagnostic);
    const std::string& name() const { return name_; }
    const std::string& reflectionJson() const { return reflection_; }
    // Each grid/local shape gets its real specialized ACO variants and exact
    // tail regions. No cached fixed-workgroup shader is reused for a tail.
    // The product owns its code after this Shader is destroyed. Empty grids
    // represent no dispatch and must be handled by the encoder, not compiled.
    Result build(const uint32_t grid[3], const uint32_t local[3],
                 const std::string& absoluteAcoPath, std::shared_ptr<const Pipeline>& out,
                 std::string& diagnostic) const;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
private:
    static Result compileImpl(const void*,size_t,HNShaderInput,const std::string&,
                              const Tools*,std::shared_ptr<const Shader>&,std::string&);
    Shader() = default;
    std::unique_ptr<HNShaderResult, decltype(&hn_shader_result_free)>
        shader_{nullptr, hn_shader_result_free};
    std::string name_, reflection_;
};
} }
