// SPDX-License-Identifier: MIT
// Actual compiler and owned machine bytes only; no Context/device/GPU calls.
#include "../graphics_shader.hpp"
#include "../graphics_buffer_snapshot.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace hackynavi { namespace compute { namespace detail {
bool graphicsFragmentBufferReflectionAdmitted(const std::string&, const std::string&);
bool graphicsReflectionAdmitted(const std::string&, HNShaderStage, const std::string&);
} } }
using namespace hackynavi::compute;
namespace {
unsigned checks = 0;
void check(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
std::vector<uint8_t> read(const char* path) {
    std::ifstream file(path,std::ios::binary); check(bool(file),"actual fixture opens");
    file.seekg(0,std::ios::end); const auto bytes=file.tellg();
    check(bytes>0 && bytes<64*1024*1024,"bounded actual fixture");
    std::vector<uint8_t> out(static_cast<size_t>(bytes)); file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()),bytes); check(bool(file),"actual fixture read"); return out;
}
std::string replace(std::string text,const std::string& from,const std::string& to) {
    const auto position=text.find(from); check(position!=std::string::npos,"meaningful actual reflection mutation");
    text.replace(position,from.size(),to); return text;
}
void reflection(const GraphicsShader& shader) {
    const auto& text=shader.reflectionJson();
    const auto admit=[&](const std::string& value){return detail::graphicsFragmentBufferReflectionAdmitted(value,shader.name());};
    check(admit(text),"real buffer reflection admitted");
    check(admit(" \n\t"+text+"\r\n"),"outer whitespace admitted");
    check(!detail::graphicsReflectionAdmitted(text,HN_SHADER_FRAGMENT,shader.name()),"old reflection profile remains resource-free");
    for(size_t i=0;i<text.size();++i) check(!admit(text.substr(0,i)),"every truncated reflection rejected");
    const std::pair<const char*,const char*> changes[]={
        {"\"reflection_version\":56","\"reflection_version\":57"},
        {"\"reflection_version\":56","\"reflection_version\":56,\"reflection_version\":56"},
        {"\"kind\":\"Buffer\"","\"kind\":\"Texture\""},
        {"\"metal_index\":0","\"metal_index\":1"},
        {"\"descriptor\":{\"set\":0","\"descriptor\":{\"set\":1"},
        {"\"binding\":0","\"binding\":1"},{"\"count\":1","\"count\":2"},
        {"\"param_index\":0","\"param_index\":1"},{"\"address_space\":2","\"address_space\":1"},
        {"\"declared_size\":16","\"declared_size\":32"},{"\"Object\":{\"bytes\":16}","\"Object\":{\"bytes\":8}"},
        {"\"offset\":0,\"size\":16","\"offset\":4,\"size\":16"},
        {"\"offset\":0,\"size\":16","\"offset\":0,\"size\":12"},
        {"\"offset\":0,\"size\":16","\"offset\":0,\"offset\":0,\"size\":16"},
        {"\"strided_accesses\":[]","\"strided_accesses\":[{}]"},
        {"\"has_unbounded_access\":false","\"has_unbounded_access\":true"},
        {"\"access\":\"ReadOnly\"","\"access\":\"ReadWrite\""},
        {"\"type_name\":\"float4\"","\"type_name\":\"half4\""},
        {"\"argument_buffer_fields\":[]","\"argument_buffer_fields\":[{}]"},
        {"\"varyings\":[]","\"varyings\":[{}]"},
        {"\"version\":1,\"set\":0","\"version\":2,\"set\":0"},
        {"\"buffers\":{\"start\":0,\"end\":32}","\"buffers\":{\"start\":1,\"end\":33}"},
        {"\"fragment_imageblock\":null","\"fragment_imageblock\":{}"},
        {"\"function_constants\":[]","\"function_constants\":[{}]"},
        {"\"depth_qualifier\":null","\"depth_qualifier\":\"Any\""},
        {"\"stage\":\"Fragment\"","\"stage\":\"Vertex\""},
        {"\"reflection_version\":56","\"reflection_version\":56,\"future_resource\":0"},
    };
    for(const auto& mutation:changes) check(!admit(replace(text,mutation.first,mutation.second)),"unsupported binding/reflection rejected");
    check(!admit(text+"null"),"trailing JSON rejected");
    check(!detail::graphicsFragmentBufferReflectionAdmitted(text,"different"),"exact actual entry name");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==9,"supply buffer AIR/metallib/SPV, vertex code, buffer code, constant code, explicit dis/val");
        const auto air=read(argv[1]),library=read(argv[2]),spirv=read(argv[3]);
        const auto vertex=read(argv[4]),buffer=read(argv[5]),constant=read(argv[6]);
        const Shader::Tools tools{argv[7],argv[8]};
        std::shared_ptr<const GraphicsShader> held; std::string diagnostic;
        for(unsigned container=0;container<2;++container) {
            auto input=container?library:air; auto paths=tools;
            std::shared_ptr<const GraphicsShader> shader;
            check(bool(GraphicsShader::compileFragmentBuffer(input.data(),input.size(),container?HN_SHADER_METALLIB:HN_SHADER_AIR,
                "hn_buffer_fragment",paths,shader,diagnostic)),"actual buffer shader compiles");
            check(shader && shader->stage()==HN_SHADER_FRAGMENT && shader->name()=="hn_buffer_fragment" &&
                shader->spirvEntryPoint()=="main" && diagnostic.empty(),"actual stage/entry owned");
            const auto* binding=shader->fragmentBufferBinding();
            check(binding && binding->metalIndex==0 && binding->descriptorSet==0 && binding->descriptorBinding==0 &&
                binding->offset==0 && binding->bytes==16 && binding->requiredAlignment==16,"exact readonly resource profile metadata");
            std::fill(input.begin(),input.end(),0xee);input.clear();input.shrink_to_fit();
            paths.airDisassembler.clear();paths.spirvValidator.clear();
            check(shader->spirvWords().size()*4==spirv.size() &&
                !std::memcmp(shader->spirvWords().data(),spirv.data(),spirv.size()),"real SPIR-V owned after source/tools released");
            reflection(*shader);
            const auto before=shader;
            check(!GraphicsShader::compileFragmentBuffer(air.data(),air.size(),HN_SHADER_AIR,"hn_buffer_fragment",tools,shader,diagnostic) &&
                shader==before,"nonempty output stays unchanged");
            held=shader;
        }
        std::shared_ptr<const GraphicsShader> emptyShader;
        check(!GraphicsShader::compile(air.data(),air.size(),HN_SHADER_AIR,HN_SHADER_FRAGMENT,"hn_buffer_fragment",tools,emptyShader,diagnostic) &&
            !emptyShader,"old factory still rejects buffer profile");
        check(!GraphicsShader::compileFragmentBuffer(nullptr,air.size(),HN_SHADER_AIR,"hn_buffer_fragment",tools,emptyShader,diagnostic) &&
            !emptyShader,"null input rejected before compiler");
        check(!GraphicsShader::compileFragmentBuffer(reinterpret_cast<void*>(UINTPTR_MAX-7),16,HN_SHADER_AIR,"hn_buffer_fragment",tools,emptyShader,diagnostic),"wrapped span rejected");
        check(!GraphicsShader::compileFragmentBuffer(air.data(),air.size(),HN_SHADER_AIR,"missing",tools,emptyShader,diagnostic),"actual missing function fails");
        auto badTools=tools;badTools.airDisassembler="relative";
        check(!GraphicsShader::compileFragmentBuffer(air.data(),air.size(),HN_SHADER_AIR,"hn_buffer_fragment",badTools,emptyShader,diagnostic),"explicit tools never fall back");

        auto vertexInput=vertex,fragmentInput=buffer;
        std::shared_ptr<const FragmentBufferGraphicsPipeline> pipeline;
        check(bool(FragmentBufferGraphicsPipeline::capture(vertexInput.data(),vertexInput.size(),fragmentInput.data(),fragmentInput.size(),pipeline)),"actual owned code independently admitted");
        std::fill(vertexInput.begin(),vertexInput.end(),0);vertexInput.clear();vertexInput.shrink_to_fit();
        std::fill(fragmentInput.begin(),fragmentInput.end(),0);fragmentInput.clear();fragmentInput.shrink_to_fit();
        check(pipeline && pipeline->vertexBytes()==vertex.size() && pipeline->fragmentBytes()==buffer.size() &&
            pipeline->fragmentBufferIndex()==0 && pipeline->fragmentBufferBytes()==16 && pipeline->fragmentBufferAlignment()==16,"distinct pipeline dimensions/resource contract");
        std::vector<uint8_t> image(4096,0xa5);
        check(bool(pipeline->copyImage(image.data(),image.size())),"owned image copies");
        hackynavi::gfx11graphics::Layout layout{};
        check(hackynavi::gfx11graphics::layout(vertex.size(),buffer.size(),layout)==hackynavi::gfx11graphics::Status::Ok,"canonical graphics layout");
        check(!std::memcmp(image.data(),vertex.data(),vertex.size()) &&
            !std::memcmp(image.data()+layout.fragmentOffset,buffer.data(),buffer.size()),"actual shader bytes survive original free");
        for(size_t i=vertex.size();i<layout.fragmentOffset;++i)check(image[i]==0,"owned stage-gap padding");
        for(size_t i=layout.fragmentOffset+buffer.size();i<image.size();++i)check(image[i]==0,"owned trailing padding");
        const auto original=image;
        for(size_t length:{size_t(0),size_t(4095),size_t(4097)})
            check(!pipeline->copyImage(image.data(),length) && image==original,"wrong output extent unchanged");
        check(!pipeline->copyImage(const_cast<FragmentBufferGraphicsPipeline*>(pipeline.get()),4096),"output cannot overwrite immutable object");
        check(bool(pipeline->copyImage(image.data(),4096)) && image==original,"alias rejection preserved image");
        const auto before=pipeline;
        check(!FragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),buffer.data(),buffer.size(),pipeline) && pipeline==before,"nonempty pipeline unchanged");
        std::shared_ptr<const FragmentBufferGraphicsPipeline> empty;
        check(!FragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),constant.data(),constant.size(),empty) && !empty,"new capture rejects no-memory constants");
        check(!FragmentBufferGraphicsPipeline::capture(nullptr,vertex.size(),buffer.data(),buffer.size(),empty),"null code rejected");
        check(!FragmentBufferGraphicsPipeline::capture(reinterpret_cast<void*>(UINTPTR_MAX-3),vertex.size(),buffer.data(),buffer.size(),empty),"code span overflow rejected");
        check(!FragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),buffer.data(),buffer.size()-1,empty),"unaligned stage length rejected");
        for(size_t offset:{size_t(0),size_t(8),size_t(36),size_t(44),size_t(68)}) {
            auto changed=buffer;changed[offset]^=1;
            check(!FragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),changed.data(),changed.size(),empty) && !empty,"mutated actual buffer instruction rejected");
        }
        std::shared_ptr<const GraphicsPipeline> old;
        check(!GraphicsPipeline::capture(vertex.data(),vertex.size(),buffer.data(),buffer.size(),old) && !old,"old snapshot still rejects memory profile");
        check(bool(GraphicsPipeline::capture(vertex.data(),vertex.size(),constant.data(),constant.size(),old)),"old actual resource-free capture preserved");
        std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"gpu_executed\":false,\"metal_objects_created\":false}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
