// SPDX-License-Identifier: MIT
#pragma once
#include "compute_shader.hpp"

namespace hackynavi { namespace compute {
struct FragmentBufferBinding {
    uint32_t metalIndex = 0, descriptorSet = 0, descriptorBinding = 0;
    uint32_t offset = 0, bytes = 16, requiredAlignment = 16;
};
// A draw requirement, not reflection's declared_size or claimed static extent.
// Actual reflection is Unbounded with VertexIndex*16 + component offsets.
// The independently admitted draw restricts VertexIndex to 0..2.
struct VertexBufferBinding {
    uint32_t metalIndex = 0, descriptorSet = 0, descriptorBinding = 0;
    uint32_t offset = 0, bytes = 48, requiredAlignment = 16;
};
// Actual translated graphics stage, independent of GPU/device
// discovery. Retains the owned compiler result and an immutable SPIR-V copy.
// NGG/PS machine-code compilation and independent native ISA admission follow
// separately; successful translation does not imply a drawable pipeline.
class GraphicsShader final {
public:
    static Result compile(const void* input, size_t bytes, HNShaderInput format,
                          HNShaderStage stage, const std::string& entry,
                          const Shader::Tools& tools,
                          std::shared_ptr<const GraphicsShader>& out,
                          std::string& diagnostic);
    // Additive fixed fragment profile. No caller profile flag or reflection is
    // accepted. The actual owned compiler result must report exactly one
    // readonly constant float4 buffer0 covering [0,16), and no other resources.
    // compile() above retains its original resource-free admission.
    static Result compileFragmentBuffer(const void* input, size_t bytes, HNShaderInput format,
                          const std::string& entry, const Shader::Tools& tools,
                          std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic);
    static Result compileVertexBuffer(const void* input, size_t bytes, HNShaderInput format,
                          const std::string& entry, const Shader::Tools& tools,
                          std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic);
    HNShaderStage stage() const { return stage_; }
    const std::string& name() const { return name_; }
    const std::string& reflectionJson() const { return reflection_; }
    const std::string& spirvEntryPoint() const { return spirvEntry_; }
    const std::vector<uint32_t>& spirvWords() const { return words_; }
    const FragmentBufferBinding* fragmentBufferBinding() const {
        return fragmentBuffer_ ? &binding_ : nullptr;
    }
    const VertexBufferBinding* vertexBufferBinding() const { return vertexBuffer_ ? &vertexBinding_ : nullptr; }
    GraphicsShader(const GraphicsShader&) = delete;
    GraphicsShader& operator=(const GraphicsShader&) = delete;
private:
    GraphicsShader() = default;
    static Result compileInternal(const void*, size_t, HNShaderInput, HNShaderStage,
        const std::string&, const Shader::Tools&, bool, bool,
        std::shared_ptr<const GraphicsShader>&, std::string&);
    std::unique_ptr<HNShaderResult, decltype(&hn_shader_result_free)>
        shader_{nullptr, hn_shader_result_free};
    HNShaderStage stage_ = HN_SHADER_VERTEX;
    std::string name_, reflection_, spirvEntry_;
    std::vector<uint32_t> words_;
    bool fragmentBuffer_ = false;
    FragmentBufferBinding binding_{};
    bool vertexBuffer_ = false;
    VertexBufferBinding vertexBinding_{};
};
} }
