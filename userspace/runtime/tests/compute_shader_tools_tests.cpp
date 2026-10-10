// SPDX-License-Identifier: MIT
// Actual compiler/pipeline only. No Context, device or GPU is constructed.
#include "../compute_shader.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace hackynavi::compute;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> read(const char* path){
    std::ifstream stream(path,std::ios::binary);check(bool(stream),"real AIR fixture opens");
    stream.seekg(0,std::ios::end);auto size=stream.tellg();check(size>0&&size<64*1024*1024,"bounded AIR fixture");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));stream.seekg(0);stream.read(reinterpret_cast<char*>(bytes.data()),size);
    check(bool(stream),"actual AIR fixture read");return bytes;
}
}
int main(int argc,char** argv){
    try {
        check(argc==5,"supply actual AIR, disassembler, validator and ACO paths");
        const auto source=read(argv[1]);auto input=source;Shader::Tools tools{argv[2],argv[3]};
        std::shared_ptr<const Shader> shader;std::string diagnostic;
        check(bool(Shader::compile(input.data(),input.size(),HN_SHADER_AIR,"add_one",tools,shader,diagnostic)),"explicit runtime compile");
        check(shader&&shader->name()=="add_one"&&!shader->reflectionJson().empty()&&diagnostic.empty(),"actual reflected owned shader");
        tools.airDisassembler="relative-invalid";tools.spirvValidator.clear();std::fill(input.begin(),input.end(),0xff);
        const uint32_t grid[]={4099,1,1},local[]={64,1,1};std::shared_ptr<const Pipeline> product;
        check(bool(shader->build(grid,local,argv[4],product,diagnostic)),"retained tool paths support real ACO");
        shader.reset();check(product&&product->variantCount()==2,"owned full and tail code survives shader free");
        std::vector<uint8_t> code[2];
        for(uint32_t i=0;i<2;++i){HNPipelineVariant v{};check(bool(product->variantInfo(i,v)),"actual metadata");
            check(v.local_size[0]==(i?3u:64u)&&v.local_size[1]==1&&v.local_size[2]==1&&v.wave_size==32,"actual specialized local");
            code[i].resize(v.code_bytes);check(bool(product->copyCode(i,code[i].data(),code[i].size())),"owned actual ISA");}
        check(code[0]!=code[1],"full and tail compile to different code");
        for(const auto& invalid:{std::string(),std::string("relative"),std::string(4097,'x'),
                                 std::string("a\0b",3),std::string("\xff",1)}){
            Shader::Tools bad{invalid,argv[3]};std::shared_ptr<const Shader> empty;
            check(!Shader::compile(source.data(),source.size(),HN_SHADER_AIR,"add_one",bad,empty,diagnostic),"invalid explicit path fails");
            check(!empty&&!diagnostic.empty(),"failure preserves empty output with actual diagnostic");
        }
        Shader::Tools valid{argv[2],argv[3]};
        check(bool(Shader::compile(source.data(),source.size(),HN_SHADER_AIR,"add_one",valid,shader,diagnostic)),"valid tools remain independent");
        auto retained=shader;
        check(!Shader::compile(source.data(),source.size(),HN_SHADER_AIR,"add_one",valid,shader,diagnostic)&&shader==retained,"nonempty output preserved");
        std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"gpu_executed\":false,\"context_opened\":false}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
