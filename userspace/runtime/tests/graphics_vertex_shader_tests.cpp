// SPDX-License-Identifier: MIT
// Uses the genuine compiler DLL and Apple AIR/metallib. No device or GPU call.
#include "../graphics_shader.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace hackynavi { namespace compute { namespace detail {
bool graphicsVertexBufferReflectionAdmitted(const std::string&,const std::string&);
bool graphicsReflectionAdmitted(const std::string&,HNShaderStage,const std::string&);
bool graphicsFragmentBufferReflectionAdmitted(const std::string&,const std::string&);
} } }
using namespace hackynavi::compute;
namespace {
unsigned checks=0;
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
std::vector<uint8_t> read(const char* path) {
    std::ifstream f(path,std::ios::binary);check(bool(f),"actual fixture opens");
    f.seekg(0,std::ios::end);const auto size=f.tellg();check(size>0&&size<64*1024*1024,"bounded input");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()),size);check(bool(f),"actual fixture read");return bytes;
}
std::string replace(std::string text,const char* from,const char* to) {
    const auto at=text.find(from);check(at!=std::string::npos,"actual reflection mutation exists");
    text.replace(at,std::strlen(from),to);return text;
}
void reflection(const GraphicsShader& shader) {
    const auto& text=shader.reflectionJson();
    auto admit=[&](const std::string& input){return detail::graphicsVertexBufferReflectionAdmitted(input,shader.name());};
    check(admit(text),"actual strided unbounded reflection accepted");
    check(admit(" \n"+text+"\t\r\n"),"insignificant whitespace accepted");
    check(!detail::graphicsReflectionAdmitted(text,HN_SHADER_VERTEX,shader.name()),"resource-free path unchanged");
    check(!detail::graphicsFragmentBufferReflectionAdmitted(text,shader.name()),"fragment path unchanged");
    for(size_t i=0;i<text.size();++i)check(!admit(text.substr(0,i)),"truncated reflection rejected");
    const std::pair<const char*,const char*> mutations[]={
        {"\"reflection_version\":56","\"reflection_version\":57"},
        {"\"reflection_version\":56","\"reflection_version\":56,\"reflection_version\":56"},
        {"\"stage\":\"Vertex\"","\"stage\":\"Fragment\""},
        {"\"declared_size\":16","\"declared_size\":48"},
        {"\"extent\":\"Unbounded\"","\"extent\":{\"Object\":{\"bytes\":48}}"},
        {"\"source\":\"VertexIndex\"","\"source\":\"InstanceIndex\""},
        {"\"stride\":16","\"stride\":32"},
        {"\"base_offset\":0","\"base_offset\":4"},
        {"\"access_size\":4","\"access_size\":16"},
        {"\"metal_index\":0","\"metal_index\":1"},
        {"\"descriptor\":{\"set\":0","\"descriptor\":{\"set\":1"},
        {"\"binding\":0","\"binding\":1"},
        {"\"count\":1","\"count\":2"},
        {"\"address_space\":2","\"address_space\":1"},
        {"\"access\":\"ReadOnly\"","\"access\":\"ReadWrite\""},
        {"\"has_unbounded_access\":false","\"has_unbounded_access\":true"},
        {"\"uses_vertex_index\":true","\"uses_vertex_index\":false"},
        {"\"uses_instance_index\":false","\"uses_instance_index\":true"},
        {"\"writes_position\":true","\"writes_position\":false"},
        {"\"static_ranges\":[]","\"static_ranges\":[{\"offset\":0,\"size\":48}]"},
        {"\"type_name\":\"float4\"","\"type_name\":\"half4\""},
        {"\"vertex_attributes\":[]","\"vertex_attributes\":[{}]"},
        {"\"varyings\":[]","\"varyings\":[{}]"},
        {"\"function_constants\":[]","\"function_constants\":[{}]"},
        {"\"buffers\":{\"start\":0","\"buffers\":{\"start\":1"},
        {"\"reflection_version\":56","\"reflection_version\":56,\"future_resource\":0"}
    };
    for(const auto& m:mutations)check(!admit(replace(text,m.first,m.second)),"changed resource/reflection rejected");
    check(!admit(text+"null"),"trailing JSON rejected");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==6,"provide real AIR/metallib/SPV and explicit disassembler/validator");
        const auto air=read(argv[1]),library=read(argv[2]),spv=read(argv[3]);
        const Shader::Tools tools{argv[4],argv[5]};std::string diagnostic;
        for(unsigned kind=0;kind<2;++kind) {
            auto input=kind?library:air;
            std::shared_ptr<const GraphicsShader> shader;
            check(bool(GraphicsShader::compileVertexBuffer(input.data(),input.size(),kind?HN_SHADER_METALLIB:HN_SHADER_AIR,
                "hn_buffer_vertex",tools,shader,diagnostic)),"genuine vertex buffer compiles");
            const auto* b=shader->vertexBufferBinding();
            check(b&&b->metalIndex==0&&b->descriptorSet==0&&b->descriptorBinding==0&&b->offset==0&&
                b->bytes==48&&b->requiredAlignment==16&&!shader->fragmentBufferBinding(),"conditional draw binding requirement");
            check(shader->stage()==HN_SHADER_VERTEX&&shader->spirvEntryPoint()=="main"&&diagnostic.empty(),"actual vertex identity");
            std::fill(input.begin(),input.end(),0xee);input.clear();input.shrink_to_fit();
            check(shader->spirvWords().size()*4==spv.size()&&!std::memcmp(shader->spirvWords().data(),spv.data(),spv.size()),"actual SPIR-V independently owned");
            reflection(*shader);const auto before=shader;
            check(!GraphicsShader::compileVertexBuffer(air.data(),air.size(),HN_SHADER_AIR,"hn_buffer_vertex",tools,shader,diagnostic)&&shader==before,"nonempty output unchanged");
        }
        std::shared_ptr<const GraphicsShader> empty;
        check(!GraphicsShader::compile(air.data(),air.size(),HN_SHADER_AIR,HN_SHADER_VERTEX,"hn_buffer_vertex",tools,empty,diagnostic)&&!empty,"old shader factory still rejects resource");
        check(!GraphicsShader::compileVertexBuffer(nullptr,air.size(),HN_SHADER_AIR,"hn_buffer_vertex",tools,empty,diagnostic),"null input rejected");
        check(!GraphicsShader::compileVertexBuffer(reinterpret_cast<void*>(UINTPTR_MAX-7),16,HN_SHADER_AIR,"hn_buffer_vertex",tools,empty,diagnostic),"wrapped span rejected");
        check(!GraphicsShader::compileVertexBuffer(air.data(),air.size(),HN_SHADER_AIR,"missing",tools,empty,diagnostic),"actual missing entry rejected");
        auto invalidTools=tools;invalidTools.airDisassembler="relative";
        check(!GraphicsShader::compileVertexBuffer(air.data(),air.size(),HN_SHADER_AIR,"hn_buffer_vertex",invalidTools,empty,diagnostic),"no tool fallback");
        std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"gpu_executed\":false}\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
