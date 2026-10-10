// SPDX-License-Identifier: MIT
#include "../runtime/graphics_build.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
std::vector<uint8_t> read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv) {
    using namespace hackynavi;
    using namespace hackynavi::compute;
    if (argc != 2) return 2;
    const std::string root = argv[1];
    const Shader::Tools tools{root + "/apple-air-dis", root + "/spirv-val"};
    const auto vertexBytes = read(root + "/fixtures/vertex.bin");
    const auto fragmentBytes = read(root + "/fixtures/fragment.bin");
    const auto bufferBytes = read(root + "/fixtures/fragment-buffer.bin");
    const auto vertexBufferBytes = read(root + "/fixtures/vertex-buffer.bin");
    check(!vertexBytes.empty() && !fragmentBytes.empty(), "Verified independently compiled stage bytes exist");
    gfx11graphics::Layout layout{};
    check(gfx11graphics::layout(vertexBytes.size(), fragmentBytes.size(), layout) == gfx11graphics::Status::Ok,
          "Actual stage code has bounded canonical layout");
    if (failures) return 1;
    std::array<uint8_t, nativeabi::kGraphicsImageBytes> expected{};
    std::copy(vertexBytes.begin(), vertexBytes.end(), expected.begin());
    std::copy(fragmentBytes.begin(), fragmentBytes.end(), expected.begin() + layout.fragmentOffset);
    check(!bufferBytes.empty(), "Actual independently compiled buffer-reading fragment exists");
    gfx11graphics::Layout bufferLayout{};
    check(gfx11graphics::layout(vertexBytes.size(), bufferBytes.size(), bufferLayout) == gfx11graphics::Status::Ok,
          "Actual buffer stage code has bounded canonical layout");
    if (failures) return 1;
    std::array<uint8_t, nativeabi::kGraphicsImageBytes> expectedBuffer{};
    std::copy(vertexBytes.begin(), vertexBytes.end(), expectedBuffer.begin());
    std::copy(bufferBytes.begin(), bufferBytes.end(), expectedBuffer.begin() + bufferLayout.fragmentOffset);
    gfx11graphics::Layout vertexLayout{}, combinedLayout{};
    check(!vertexBufferBytes.empty(), "Actual independent vertex-buffer machine product exists");
    check(gfx11graphics::layout(vertexBufferBytes.size(),fragmentBytes.size(),vertexLayout)==gfx11graphics::Status::Ok &&
          gfx11graphics::layout(vertexBufferBytes.size(),bufferBytes.size(),combinedLayout)==gfx11graphics::Status::Ok,
          "Actual vertex-buffer stage pairs have bounded canonical layouts");
    if (failures) return 1;
    std::array<uint8_t,nativeabi::kGraphicsImageBytes> expectedVertex{},expectedCombined{};
    std::copy(vertexBufferBytes.begin(),vertexBufferBytes.end(),expectedVertex.begin());
    std::copy(fragmentBytes.begin(),fragmentBytes.end(),expectedVertex.begin()+vertexLayout.fragmentOffset);
    std::copy(vertexBufferBytes.begin(),vertexBufferBytes.end(),expectedCombined.begin());
    std::copy(bufferBytes.begin(),bufferBytes.end(),expectedCombined.begin()+combinedLayout.fragmentOffset);
    unsigned pipelines = 0;
    for (const auto format : {HN_SHADER_AIR, HN_SHADER_METALLIB}) {
        auto raw = read(root + (format == HN_SHADER_AIR ? "/fixtures/graphics.air" : "/fixtures/graphics.metallib"));
        check(!raw.empty(), "Actual Apple input exists");
        std::shared_ptr<const GraphicsShader> vertex, fragment;
        std::string diagnostic;
        auto result = GraphicsShader::compile(raw.data(), raw.size(), format, HN_SHADER_VERTEX,
            "hn_triangle_vertex", tools, vertex, diagnostic);
        check(bool(result) && vertex && diagnostic.empty(), "Actual GraphicsShader vertex translation");
        if (!result) { std::fprintf(stderr, "%s\n", diagnostic.c_str()); return 1; }
        result = GraphicsShader::compile(raw.data(), raw.size(), format, HN_SHADER_FRAGMENT,
            "hn_constant_fragment", tools, fragment, diagnostic);
        check(bool(result) && fragment && diagnostic.empty(), "Actual GraphicsShader fragment translation");
        if (!result) { std::fprintf(stderr, "%s\n", diagnostic.c_str()); return 1; }
        const auto vertexSpirv = vertex->spirvWords(), fragmentSpirv = fragment->spirvWords();
        std::fill(raw.begin(), raw.end(), uint8_t(0xb7));
        std::vector<uint8_t>().swap(raw);
        check(vertex->spirvWords() == vertexSpirv && fragment->spirvWords() == fragmentSpirv,
              "Actual shader owns SPIR-V after input mutation/free");
        std::shared_ptr<const GraphicsPipeline> pipeline;
        result = buildGraphicsPipeline(*fragment, *vertex, pipeline, diagnostic);
        check(!result && !pipeline && !diagnostic.empty(), "Reversed actual stage pair rejects without output");
        result = buildGraphicsPipeline(*vertex, *fragment, pipeline, diagnostic);
        check(bool(result) && pipeline && diagnostic.empty(), "Actual C++ path compiles and independently admits NGG/PS");
        if (!result) { std::fprintf(stderr, "%s\n", diagnostic.c_str()); return 1; }
        ++pipelines;
        auto retained = pipeline;
        result = buildGraphicsPipeline(*vertex, *fragment, pipeline, diagnostic);
        check(!result && pipeline == retained, "Nonempty pipeline output is preserved on rejection");
        auto bufferRaw = read(root + (format == HN_SHADER_AIR ? "/fixtures/fragment-buffer.air" : "/fixtures/fragment-buffer.metallib"));
        check(!bufferRaw.empty(), "Actual Apple buffer input exists");
        std::shared_ptr<const GraphicsShader> bufferShader, rejectedShader;
        result = GraphicsShader::compile(bufferRaw.data(), bufferRaw.size(), format, HN_SHADER_FRAGMENT,
            "hn_buffer_fragment", tools, rejectedShader, diagnostic);
        check(!result && !rejectedShader && !diagnostic.empty(), "Resource-free shader factory rejects actual buffer interface");
        result = GraphicsShader::compileFragmentBuffer(bufferRaw.data(), bufferRaw.size(), format,
            "hn_buffer_fragment", tools, bufferShader, diagnostic);
        check(bool(result) && bufferShader && diagnostic.empty(), "Actual fragment buffer shader translates with explicit tools");
        if (!result) { std::fprintf(stderr, "%s\n", diagnostic.c_str()); return 1; }
        const auto* binding = bufferShader->fragmentBufferBinding();
        check(binding && binding->metalIndex == 0 && binding->descriptorSet == 0 && binding->descriptorBinding == 0 &&
              binding->offset == 0 && binding->bytes == 16 && binding->requiredAlignment == 16,
              "Exact actual readonly constant float4 reflection is owned");
        const auto bufferSpirv = bufferShader->spirvWords();
        std::fill(bufferRaw.begin(), bufferRaw.end(), uint8_t(0x5a));
        std::vector<uint8_t>().swap(bufferRaw);
        check(bufferShader->spirvWords() == bufferSpirv, "Buffer shader owns SPIR-V after input mutation/free");
        std::shared_ptr<const GraphicsPipeline> rejectedPipeline;
        result = buildGraphicsPipeline(*vertex, *bufferShader, rejectedPipeline, diagnostic);
        check(!result && !rejectedPipeline && !diagnostic.empty(), "Resource-free builder rejects buffer profile");
        std::shared_ptr<const FragmentBufferGraphicsPipeline> bufferPipeline;
        result = buildFragmentBufferGraphicsPipeline(*vertex, *fragment, bufferPipeline, diagnostic);
        check(!result && !bufferPipeline && !diagnostic.empty(), "Buffer builder rejects resource-free fragment profile");
        result = buildFragmentBufferGraphicsPipeline(*bufferShader, *vertex, bufferPipeline, diagnostic);
        check(!result && !bufferPipeline && !diagnostic.empty(), "Buffer builder rejects reversed stage pair");
        result = buildFragmentBufferGraphicsPipeline(*vertex, *bufferShader, bufferPipeline, diagnostic);
        check(bool(result) && bufferPipeline && diagnostic.empty(), "Actual C ABI buffer path compiles and independently admits NGG/PS");
        if (!result) { std::fprintf(stderr, "%s\n", diagnostic.c_str()); return 1; }
        ++pipelines;
        auto retainedBuffer = bufferPipeline;
        result = buildFragmentBufferGraphicsPipeline(*vertex, *bufferShader, bufferPipeline, diagnostic);
        check(!result && bufferPipeline == retainedBuffer, "Nonempty buffer pipeline output is preserved on rejection");
        auto vertexRaw=read(root+(format==HN_SHADER_AIR?"/fixtures/vertex-buffer.air":"/fixtures/vertex-buffer.metallib"));
        std::shared_ptr<const GraphicsShader> vertexBufferShader;
        result=GraphicsShader::compile(vertexRaw.data(),vertexRaw.size(),format,HN_SHADER_VERTEX,
            "hn_buffer_vertex",tools,rejectedShader,diagnostic);
        check(!result&&!rejectedShader&&!diagnostic.empty(),"Resource-free factory rejects actual vertex buffer interface");
        result=GraphicsShader::compileVertexBuffer(vertexRaw.data(),vertexRaw.size(),format,
            "hn_buffer_vertex",tools,vertexBufferShader,diagnostic);
        check(bool(result)&&vertexBufferShader&&diagnostic.empty(),"Actual vertex buffer translation with explicit tools");
        if(!result){std::fprintf(stderr,"%s\n",diagnostic.c_str());return 1;}
        const auto* vertexBinding=vertexBufferShader->vertexBufferBinding();
        check(vertexBinding&&vertexBinding->metalIndex==0&&vertexBinding->descriptorSet==0&&
              vertexBinding->descriptorBinding==0&&vertexBinding->offset==0&&vertexBinding->bytes==48&&
              vertexBinding->requiredAlignment==16,"Draw-derived48-byte bound from real VertexIndex-strided reflection");
        const auto vertexBufferSpirv=vertexBufferShader->spirvWords();
        std::fill(vertexRaw.begin(),vertexRaw.end(),uint8_t(0xcd));std::vector<uint8_t>().swap(vertexRaw);
        check(vertexBufferShader->spirvWords()==vertexBufferSpirv,"Vertex shader owns SPIR-V after actual input is freed");
        result=buildGraphicsPipeline(*vertexBufferShader,*fragment,rejectedPipeline,diagnostic);
        check(!result&&!rejectedPipeline&&!diagnostic.empty(),"Old graphics builder rejects actual VBO profile");
        std::shared_ptr<const FragmentBufferGraphicsPipeline> rejectedBuffer;
        result=buildFragmentBufferGraphicsPipeline(*vertexBufferShader,*bufferShader,rejectedBuffer,diagnostic);
        check(!result&&!rejectedBuffer&&!diagnostic.empty(),"Fragment-only builder rejects VBO profile");
        std::shared_ptr<const VertexBufferGraphicsPipeline> vertexPipeline;
        result=buildVertexBufferGraphicsPipeline(*vertex,*fragment,vertexPipeline,diagnostic);
        check(!result&&!vertexPipeline&&!diagnostic.empty(),"Vertex-buffer builder rejects resource-free vertex");
        result=buildVertexBufferGraphicsPipeline(*vertexBufferShader,*bufferShader,vertexPipeline,diagnostic);
        check(!result&&!vertexPipeline&&!diagnostic.empty(),"Vertex-only builder rejects fragment buffer interface");
        result=buildVertexBufferGraphicsPipeline(*vertexBufferShader,*fragment,vertexPipeline,diagnostic);
        check(bool(result)&&vertexPipeline&&diagnostic.empty(),"Actual vertex-buffer C ABI compiles and admits real machine bytes");
        if(!result){std::fprintf(stderr,"%s\n",diagnostic.c_str());return 1;}
        ++pipelines;
        const auto retainedVertex=vertexPipeline;
        result=buildVertexBufferGraphicsPipeline(*vertexBufferShader,*fragment,vertexPipeline,diagnostic);
        check(!result&&vertexPipeline==retainedVertex,"Existing vertex pipeline preserved on rejection");
        std::shared_ptr<const VertexFragmentBufferGraphicsPipeline> combinedPipeline;
        result=buildVertexFragmentBufferGraphicsPipeline(*vertexBufferShader,*fragment,combinedPipeline,diagnostic);
        check(!result&&!combinedPipeline&&!diagnostic.empty(),"Combined builder requires fragment buffer interface");
        result=buildVertexFragmentBufferGraphicsPipeline(*vertex,*bufferShader,combinedPipeline,diagnostic);
        check(!result&&!combinedPipeline&&!diagnostic.empty(),"Combined builder requires vertex buffer interface");
        result=buildVertexFragmentBufferGraphicsPipeline(*vertexBufferShader,*bufferShader,combinedPipeline,diagnostic);
        check(bool(result)&&combinedPipeline&&diagnostic.empty(),"Actual combined compiler pipeline independently admitted");
        if(!result){std::fprintf(stderr,"%s\n",diagnostic.c_str());return 1;}
        ++pipelines;
        const auto retainedCombined=combinedPipeline;
        result=buildVertexFragmentBufferGraphicsPipeline(*vertexBufferShader,*bufferShader,combinedPipeline,diagnostic);
        check(!result&&combinedPipeline==retainedCombined,"Existing combined pipeline preserved on rejection");
        vertexBufferShader.reset();
        vertex.reset(); fragment.reset(); bufferShader.reset();
        std::array<uint8_t, nativeabi::kGraphicsImageBytes> actual{};
        check(bool(pipeline->copyImage(actual.data(), actual.size())), "Pipeline owns code after shader destruction");
        check(actual == expected, "Actual complete canonical image equals independently generated machine bytes");
        check(pipeline->vertexBytes() == vertexBytes.size() && pipeline->fragmentBytes() == fragmentBytes.size(),
              "Actual stage extents preserved");
        actual.fill(0xa7);
        const auto before = actual;
        check(!pipeline->copyImage(actual.data(), actual.size() - 1) && actual == before,
              "Wrong-sized readback preserves caller output");
        check(bufferPipeline->vertexBytes() == vertexBytes.size() && bufferPipeline->fragmentBytes() == bufferBytes.size() &&
              bufferPipeline->fragmentBufferIndex() == 0 && bufferPipeline->fragmentBufferBytes() == 16 &&
              bufferPipeline->fragmentBufferAlignment() == 16, "Admitted buffer pipeline owns exact stage extents and binding");
        check(bool(bufferPipeline->copyImage(actual.data(), actual.size())), "Buffer pipeline owns code after shader destruction");
        check(actual == expectedBuffer, "Actual buffer canonical image equals independent machine products");
        actual.fill(0x39);
        const auto bufferBefore = actual;
        check(!bufferPipeline->copyImage(actual.data(), actual.size() - 1) && actual == bufferBefore,
              "Wrong-sized buffer pipeline copy preserves output");
        check(bool(vertexPipeline->copyImage(actual.data(),actual.size()))&&actual==expectedVertex,
              "Owned vertex-buffer image equals independent real code after shader destruction");
        check(vertexPipeline->vertexBytes()==vertexBufferBytes.size()&&vertexPipeline->fragmentBytes()==fragmentBytes.size()&&
              vertexPipeline->vertexBufferIndex()==0&&vertexPipeline->vertexBufferBytes()==48&&
              vertexPipeline->vertexBufferAlignment()==16,"Vertex-only code extents and actual binding contract");
        actual.fill(0x77);const auto vertexBefore=actual;
        check(!vertexPipeline->copyImage(actual.data(),actual.size()-1)&&actual==vertexBefore,
              "Short vertex image copy preserves output");
        check(bool(combinedPipeline->copyImage(actual.data(),actual.size()))&&actual==expectedCombined,
              "Owned combined image equals both independently compiled real products");
        check(combinedPipeline->vertexBytes()==vertexBufferBytes.size()&&combinedPipeline->fragmentBytes()==bufferBytes.size()&&
              combinedPipeline->vertexBufferIndex()==0&&combinedPipeline->vertexBufferBytes()==48&&
              combinedPipeline->vertexBufferAlignment()==16&&combinedPipeline->fragmentBufferIndex()==0&&
              combinedPipeline->fragmentBufferBytes()==16&&combinedPipeline->fragmentBufferAlignment()==16,
              "Combined pipeline owns exact two-stage interface and extents");
        actual.fill(0x99);const auto combinedBefore=actual;
        check(!combinedPipeline->copyImage(actual.data(),actual.size()-1)&&actual==combinedBefore,
              "Short combined image copy preserves output");
    }
    std::printf("%u checks, %u failures, %u actual compiler-backed graphics pipelines; no GPU or Metal objects\n",
                checks, failures, pipelines);
    return failures ? 1 : 0;
}
