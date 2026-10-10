// SPDX-License-Identifier: MIT
#include "graphics_shader.hpp"
#include <new>
#include <utility>

namespace hackynavi { namespace compute {
namespace {
constexpr size_t MaxReflection = 1024U * 1024U;
bool bounded(const std::string& value, size_t max) {
    return !value.empty() && value.size() <= max && value.find('\0') == std::string::npos;
}
bool copyText(const char* text, size_t max, std::string& out) {
    if (!text) return false;
    size_t n = 0;
    while (n <= max && text[n]) ++n;
    if (n > max) return false;
    out.assign(text, n); return true;
}
std::string quoted(const std::string& text) {
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += char(c);
    }
    result += '"'; return result;
}
// Parse the actual compiler's versioned JSON rather than substring-searching
// nested metadata. Limits bound recursion, tokens, object members and input.
// Captured field values have insignificant whitespace removed, preserving all
// quoted bytes. Unknown/duplicate root fields and unsupported schema fail shut.
class Json final {
public:
    explicit Json(const std::string& source) : source_(source) {}
    bool root(std::vector<std::pair<std::string, std::string>>& fields) {
        if (source_.empty() || source_.size() > MaxReflection) return false;
        if (!object(0, &fields)) return false;
        ws(); return at_ == source_.size();
    }
private:
    void ws() { while (at_ < source_.size() && (source_[at_] == ' ' || source_[at_] == '\n' ||
        source_[at_] == '\r' || source_[at_] == '\t')) ++at_; }
    bool take(char c) { ws(); if (at_ == source_.size() || source_[at_] != c) return false; ++at_; return true; }
    bool string(std::string* out = nullptr) {
        ws(); const size_t start = at_;
        if (at_ == source_.size() || source_[at_++] != '"') return false;
        while (at_ < source_.size()) {
            unsigned char c = static_cast<unsigned char>(source_[at_++]);
            if (c == '"') { if (out) *out = source_.substr(start, at_ - start); return true; }
            if (c < 32) return false;
            if (c == '\\') {
                if (at_ == source_.size()) return false;
                char e = source_[at_++];
                if (e == 'u') {
                    for (unsigned i = 0; i != 4; ++i) {
                        if (at_ == source_.size()) return false;
                        char h = source_[at_++];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                    }
                } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' && e != 'r' && e != 't') return false;
            }
        }
        return false;
    }
    std::string compact(size_t start) const {
        std::string out; bool inString = false, escaped = false;
        for (size_t i = start; i < at_; ++i) {
            char c = source_[i];
            if (inString) {
                out += c;
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
            } else if (c == '"') { inString = true; out += c; }
            else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') out += c;
        }
        return out;
    }
    bool object(unsigned depth, std::vector<std::pair<std::string, std::string>>* output = nullptr) {
        if (!take('{')) return false;
        ws(); if (at_ < source_.size() && source_[at_] == '}') { ++at_; return true; }
        std::vector<std::string> keys;
        do {
            if (keys.size() == 64) return false;
            std::string key; if (!string(&key)) return false;
            for (const auto& old : keys) if (old == key) return false;
            keys.push_back(key);
            if (!take(':')) return false;
            const size_t start = at_;
            if (!value(depth + 1)) return false;
            if (output) output->emplace_back(key, compact(start));
            ws(); if (at_ < source_.size() && source_[at_] == '}') { ++at_; return true; }
        } while (take(','));
        return false;
    }
    bool value(unsigned depth) {
        if (depth > 16 || ++tokens_ > 4096) return false;
        ws(); if (at_ == source_.size()) return false;
        char c = source_[at_];
        if (c == '"') return string();
        if (c == '{') return object(depth);
        if (c == '[') {
            ++at_; ws(); if (at_ < source_.size() && source_[at_] == ']') { ++at_; return true; }
            do {
                if (!value(depth + 1)) return false;
                ws(); if (at_ < source_.size() && source_[at_] == ']') { ++at_; return true; }
            } while (take(','));
            return false;
        }
        for (const char* word : {"null", "true", "false"}) {
            size_t length = 0; while (word[length]) ++length;
            if (source_.compare(at_, length, word) == 0) { at_ += length; return true; }
        }
        if (c == '-') { if (++at_ == source_.size()) return false; }
        if (source_[at_] == '0') ++at_;
        else {
            if (source_[at_] < '1' || source_[at_] > '9') return false;
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
        }
        if (at_ < source_.size() && source_[at_] == '.') {
            const size_t start = ++at_;
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
            if (at_ == start) return false;
        }
        if (at_ < source_.size() && (source_[at_] == 'e' || source_[at_] == 'E')) {
            ++at_; if (at_ < source_.size() && (source_[at_] == '-' || source_[at_] == '+')) ++at_;
            const size_t start = at_;
            while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
            if (at_ == start) return false;
        }
        return true;
    }
    const std::string& source_; size_t at_ = 0, tokens_ = 0;
};
Result fail(Status status, std::string& diagnostic, const char* message) {
    diagnostic = message; return {status, 0};
}
}
namespace detail {
// Internal schema admission, separately exercised with mutations of actual
// compiler reflection. It does not accept caller metadata as shader authority.
static bool reflectionAdmitted(const std::string& json, HNShaderStage stage, const std::string& entry,
                              bool fragmentBuffer, bool vertexBuffer = false) {
    if ((stage != HN_SHADER_VERTEX && stage != HN_SHADER_FRAGMENT) || !bounded(entry, 1024)) return false;
    if (fragmentBuffer && stage != HN_SHADER_FRAGMENT) return false;
    if (vertexBuffer && stage != HN_SHADER_VERTEX) return false;
    std::vector<std::pair<std::string, std::string>> fields;
    if (!Json(json).root(fields)) return false;
    const auto take = [&](const char* name, const std::string& expected) {
        const std::string key = quoted(name);
        for (auto& field : fields) if (field.first == key) {
            if (field.second != expected) return false;
            field.first.clear(); return true;
        }
        return false;
    };
    if (!take("reflection_version", "56") || !take("stage", stage == HN_SHADER_VERTEX ? "\"Vertex\"" : "\"Fragment\"") ||
        !take("entry_point", quoted(entry))) return false;
    const char* binding = "[{\"kind\":\"Buffer\",\"metal_index\":0,\"descriptor\":{\"set\":0,\"binding\":0,\"count\":1},"
        "\"param_index\":0,\"stage_input_location\":null,\"address_space\":2,\"declared_size\":16,"
        "\"extent\":{\"Object\":{\"bytes\":16}},\"footprint\":{\"static_ranges\":[{\"offset\":0,\"size\":16}],"
        "\"strided_accesses\":[],\"has_unbounded_access\":false},\"type_layout\":null,\"type_name\":\"float4\","
        "\"texture_shape\":null,\"embedded_source\":null,\"access\":\"ReadOnly\",\"static_sampler\":null}]";
    const char* vertexBinding = "[{\"kind\":\"Buffer\",\"metal_index\":0,\"descriptor\":{\"set\":0,\"binding\":0,\"count\":1},"
        "\"param_index\":0,\"stage_input_location\":null,\"address_space\":2,\"declared_size\":16,"
        "\"extent\":\"Unbounded\",\"footprint\":{\"static_ranges\":[],\"strided_accesses\":["
        "{\"base_offset\":0,\"access_size\":4,\"terms\":[{\"source\":\"VertexIndex\",\"stride\":16}]},"
        "{\"base_offset\":4,\"access_size\":4,\"terms\":[{\"source\":\"VertexIndex\",\"stride\":16}]},"
        "{\"base_offset\":8,\"access_size\":4,\"terms\":[{\"source\":\"VertexIndex\",\"stride\":16}]},"
        "{\"base_offset\":12,\"access_size\":4,\"terms\":[{\"source\":\"VertexIndex\",\"stride\":16}]}],"
        "\"has_unbounded_access\":false},\"type_layout\":null,\"type_name\":\"float4\",\"texture_shape\":null,"
        "\"embedded_source\":null,\"access\":\"ReadOnly\",\"static_sampler\":null}]";
    if (!take("bindings", vertexBuffer ? vertexBinding : fragmentBuffer ? binding : "[]")) return false;
    for (const char* key : {"argument_buffer_fields", "vertex_attributes", "varyings", "depth_members",
        "stencil_members", "imageblock_layouts", "implicit_imageblock_attachments", "runtime_sampler_specializations",
        "runtime_storage_image_specializations", "function_constants"}) if (!take(key, "[]")) return false;
    for (const char* key : {"depth_qualifier", "local_size", "max_work_group_size", "kernel_dispatch",
        "fragment_sample_positions", "tessellation", "fragment_imageblock"}) if (!take(key, "null")) return false;
    if (stage == HN_SHADER_VERTEX) {
        if (!take("render_targets", "[]") ||
            (!take("vertex_builtins", "{\"uses_vertex_index\":true,\"uses_instance_index\":false,\"writes_position\":true}") &&
             (vertexBuffer || !take("vertex_builtins", "{\"uses_vertex_index\":false,\"uses_instance_index\":false,\"writes_position\":true}")))) return false;
    } else if (!take("vertex_builtins", "null") ||
               !take("render_targets", "[{\"member_index\":0,\"location\":0,\"type_name\":\"float4\"}]")) return false;
    // The resource-bearing profile verifies the actual versioned descriptor
    // namespace as well as its one binding. Empty-resource callers retain the
    // old rule: descriptor ranges are immaterial when none are used.
    const char* descriptorLayout = "{\"version\":1,\"set\":0,\"buffers\":{\"start\":0,\"end\":32},"
        "\"sampled_textures\":{\"start\":32,\"end\":160},\"samplers\":{\"start\":160,\"end\":192},"
        "\"color_inputs\":{\"start\":192,\"end\":200},\"imageblocks\":{\"start\":200,\"end\":224},"
        "\"fragment_imageblocks\":{\"start\":224,\"end\":480},\"storage_textures\":{\"start\":480,\"end\":608},"
        "\"synthetic\":{\"start\":640,\"end\":672}}";
    const bool resource = fragmentBuffer || vertexBuffer;
    bool layout = resource && take("descriptor_layout", descriptorLayout), data = false;
    if (resource && !layout) return false;
    for (const auto& field : fields) {
        if (field.first.empty()) continue;
        if (field.first == "\"descriptor_layout\"" && !layout && !field.second.empty() && field.second.front() == '{') layout = true;
        else if (field.first == "\"datalayout\"" && !data && field.second.size() > 2 && field.second.front() == '"') data = true;
        else return false;
    }
    return layout && data;
}
bool graphicsReflectionAdmitted(const std::string& json, HNShaderStage stage, const std::string& entry) {
    return reflectionAdmitted(json, stage, entry, false);
}
bool graphicsFragmentBufferReflectionAdmitted(const std::string& json, const std::string& entry) {
    return reflectionAdmitted(json, HN_SHADER_FRAGMENT, entry, true);
}
bool graphicsVertexBufferReflectionAdmitted(const std::string& json, const std::string& entry) {
    return reflectionAdmitted(json, HN_SHADER_VERTEX, entry, false, true);
}
}
Result GraphicsShader::compile(const void* input, size_t bytes, HNShaderInput format,
        HNShaderStage stage, const std::string& entry, const Shader::Tools& tools,
        std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic) {
    return compileInternal(input, bytes, format, stage, entry, tools, false, false, out, diagnostic);
}
Result GraphicsShader::compileFragmentBuffer(const void* input, size_t bytes, HNShaderInput format,
        const std::string& entry, const Shader::Tools& tools,
        std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic) {
    return compileInternal(input, bytes, format, HN_SHADER_FRAGMENT, entry, tools, true, false, out, diagnostic);
}
Result GraphicsShader::compileVertexBuffer(const void* input, size_t bytes, HNShaderInput format,
        const std::string& entry, const Shader::Tools& tools,
        std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic) {
    return compileInternal(input, bytes, format, HN_SHADER_VERTEX, entry, tools, false, true, out, diagnostic);
}
Result GraphicsShader::compileInternal(const void* input, size_t bytes, HNShaderInput format,
        HNShaderStage stage, const std::string& entry, const Shader::Tools& tools, bool fragmentBuffer, bool vertexBuffer,
        std::shared_ptr<const GraphicsShader>& out, std::string& diagnostic) {
    diagnostic.clear();
    try {
        if (out || !input || !bytes || bytes > 64U * 1024U * 1024U ||
            bytes > UINTPTR_MAX - reinterpret_cast<uintptr_t>(input) || !bounded(entry, 1024) ||
            (format != HN_SHADER_AIR && format != HN_SHADER_LLVM_TEXT && format != HN_SHADER_METALLIB) ||
            (stage != HN_SHADER_VERTEX && stage != HN_SHADER_FRAGMENT) ||
            !bounded(tools.airDisassembler, 4096) || !bounded(tools.spirvValidator, 4096))
            return fail(Status::Invalid, diagnostic, "Invalid graphics stage/input/tools or nonempty output.");
        if (hn_shader_abi_version() != 1) return fail(Status::Unsupported, diagnostic, "Unsupported shader ABI.");
        auto result = std::shared_ptr<GraphicsShader>(new GraphicsShader);
        const HNShaderTools paths{sizeof(HNShaderTools), 1, tools.airDisassembler.c_str(), tools.spirvValidator.c_str()};
        result->shader_.reset(hn_shader_compile_with_tools(&paths, static_cast<const uint8_t*>(input), bytes,
            format, stage, entry.c_str(), 1, 1, 1, HN_SHADER_WHOLE_WORKGROUPS, 1));
        if (!result->shader_) return fail(Status::OutOfMemory, diagnostic, "Compiler returned no owned graphics result.");
        std::string error;
        if (!copyText(hn_shader_result_error(result->shader_.get()), MaxReflection, error))
            return fail(Status::Protocol, diagnostic, "Graphics result has no bounded status.");
        if (!error.empty()) { diagnostic = std::move(error); return {Status::Unsupported, 0}; }
        size_t count = 0;
        const auto* words = hn_shader_result_words(result->shader_.get(), &count);
        if (!words || count < 5 || count > (64U * 1024U * 1024U / sizeof(uint32_t)) ||
            count > (UINTPTR_MAX - reinterpret_cast<uintptr_t>(words)) / sizeof(uint32_t) || words[0] != 0x07230203 ||
            !copyText(hn_shader_result_reflection(result->shader_.get()), MaxReflection, result->reflection_) ||
            !copyText(hn_shader_result_spirv_entry_point(result->shader_.get()), 1024, result->spirvEntry_) || result->spirvEntry_.empty())
            return fail(Status::Protocol, diagnostic, "Graphics result lacks bounded actual code/reflection/entry.");
        if (!detail::reflectionAdmitted(result->reflection_, stage, entry, fragmentBuffer, vertexBuffer))
            return fail(Status::Unsupported, diagnostic, vertexBuffer ?
                "Graphics reflection exceeds the readonly float4 buffer0/VertexIndex*16 vertex profile." : fragmentBuffer ?
                "Graphics reflection exceeds the readonly buffer0/16-byte fragment profile." :
                "Graphics reflection exceeds the resource-free position/color profile.");
        result->stage_ = stage; result->name_ = entry;
        result->fragmentBuffer_ = fragmentBuffer;
        result->vertexBuffer_ = vertexBuffer;
        result->words_.assign(words, words + count);
        out = std::move(result); return {Status::Ok, 0};
    } catch (const std::bad_alloc&) { diagnostic.clear(); return {Status::OutOfMemory, 0}; }
}
} }
