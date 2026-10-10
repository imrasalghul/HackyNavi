// SPDX-License-Identifier: MIT
#include "graphics_build.hpp"
#include "../compiler/hackynavi_gfx11_ngg.h"
#include "../compiler/hackynavi_gfx11_fragment.h"
#include "../compiler/hackynavi_gfx11_fragment_buffer.h"
#include "../compiler/hackynavi_gfx11_vertex_buffer.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>

namespace hackynavi { namespace compute { namespace {
std::mutex compilerMutex;
Result failure(Status status, std::string& diagnostic, const char* text) {
    diagnostic = text; return {status, 0};
}
struct File {
    int fd = -1;
    ~File() { if (fd >= 0) ::close(fd); }
};
struct Stage {
    char path[64] = "/tmp/HackyNavi-Graphics.XXXXXX";
    bool owned = false;
    bool create() {
        owned = ::mkdtemp(path) != nullptr;
        if (!owned) return false;
        return !::mkdir((std::string(path)+"/vertex").c_str(),0700) &&
               !::mkdir((std::string(path)+"/fragment").c_str(),0700);
    }
    bool clear() noexcept {
        if (!owned) return true;
        // Only remove the exact known files in this uniquely owned stage.
        // Unexpected output prevents directory removal; it is never followed.
        bool ok = true;
        const char* files[] = {"vertex.spv","fragment.spv","vertex/shader.bin","vertex/shader.json",
            "vertex/aco.txt","vertex/aco-diagnostics.txt","fragment/shader.bin","fragment/shader.json",
            "fragment/aco.txt","fragment/aco-diagnostics.txt"};
        char name[128];
        for (const auto* file : files) {
            const int n = std::snprintf(name,sizeof(name),"%s/%s",path,file);
            if (n <= 0 || size_t(n) >= sizeof(name)) { ok=false; continue; }
            if (::unlink(name) && errno != ENOENT) ok=false;
        }
        for (const auto* dir : {"vertex","fragment"}) {
            const int n = std::snprintf(name,sizeof(name),"%s/%s",path,dir);
            if (n <= 0 || size_t(n) >= sizeof(name)) { ok=false; continue; }
            if (::rmdir(name) && errno != ENOENT) ok=false;
        }
        if (::rmdir(path) && errno != ENOENT) ok=false;
        if (ok) owned=false;
        return ok;
    }
    ~Stage() { clear(); }
};
bool writeWords(const std::string& path,const std::vector<uint32_t>& words) {
    if (words.size()<5 || words.size()>16U*1024U*1024U || words[0]!=0x07230203) return false;
    File file; file.fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (file.fd<0) return false;
    const auto* bytes=reinterpret_cast<const uint8_t*>(words.data());
    const size_t length=words.size()*sizeof(uint32_t);
    size_t offset=0;
    while (offset<length) {
        const ssize_t amount=::write(file.fd,bytes+offset,length-offset);
        if (amount<0 && errno==EINTR) continue;
        if (amount<=0) return false;
        offset+=size_t(amount);
    }
    const int fd=file.fd; file.fd=-1;
    return ::close(fd)==0;
}
bool readCode(const std::string& path,std::vector<uint8_t>& out) {
    File file; file.fd=::open(path.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    struct stat before{};
    if (file.fd<0 || ::fstat(file.fd,&before) || !S_ISREG(before.st_mode) || before.st_nlink!=1 ||
        before.st_size<=20 || before.st_size>1024 || (before.st_size&3)) return false;
    std::vector<uint8_t> data(size_t(before.st_size));
    size_t offset=0;
    while (offset<data.size()) {
        const ssize_t amount=::read(file.fd,data.data()+offset,data.size()-offset);
        if (amount<0 && errno==EINTR) continue;
        if (amount<=0) return false;
        offset+=size_t(amount);
    }
    uint8_t extra=0;
    struct stat after{};
    if (::read(file.fd,&extra,1)!=0 || ::fstat(file.fd,&after) || before.st_dev!=after.st_dev ||
        before.st_ino!=after.st_ino || before.st_size!=after.st_size || after.st_nlink!=1) return false;
    out=std::move(data); return true;
}
} // namespace
Result buildGraphicsPipeline(const GraphicsShader& vertex,const GraphicsShader& fragment,
    std::shared_ptr<const GraphicsPipeline>& output,std::string& diagnostic) {
    diagnostic.clear();
    try {
        if (output || vertex.stage()!=HN_SHADER_VERTEX || fragment.stage()!=HN_SHADER_FRAGMENT ||
            vertex.fragmentBufferBinding() || fragment.fragmentBufferBinding() ||
            vertex.vertexBufferBinding() || fragment.vertexBufferBinding())
            return failure(Status::Invalid,diagnostic,"Expected actual vertex/fragment shaders and an empty output.");
        Stage stage;
        if (!stage.create()) return failure(Status::OutOfMemory,diagnostic,"Unable to create the private graphics compiler stage.");
        const std::string root=stage.path,vs=root+"/vertex.spv",fs=root+"/fragment.spv";
        if (!writeWords(vs,vertex.spirvWords()) || !writeWords(fs,fragment.spirvWords()))
            return failure(Status::Protocol,diagnostic,"Unable to write the actual validated stage SPIR-V.");
        {
            // The two adapters share Mesa's process-global compiler state.
            std::lock_guard<std::mutex> lock(compilerMutex);
            if (hn_gfx11_ngg_compile_validated_file(vs.c_str(),vertex.spirvEntryPoint().c_str(),(root+"/vertex").c_str()))
                return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the vertex shader.");
            if (hn_gfx11_fragment_compile_validated_file(fs.c_str(),fragment.spirvEntryPoint().c_str(),(root+"/fragment").c_str()))
                return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the fragment shader.");
        }
        std::vector<uint8_t> vertexCode,fragmentCode;
        if (!readCode(root+"/vertex/shader.bin",vertexCode) || !readCode(root+"/fragment/shader.bin",fragmentCode))
            return failure(Status::Protocol,diagnostic,"The actual compiler did not produce bounded regular machine-code files.");
        std::shared_ptr<const GraphicsPipeline> pipeline;
        const auto admitted=GraphicsPipeline::capture(vertexCode.data(),vertexCode.size(),fragmentCode.data(),fragmentCode.size(),pipeline);
        if (!admitted) return failure(admitted.status,diagnostic,"The actual machine bytes failed independent GFX11 graphics admission.");
        if (!stage.clear()) return failure(Status::Protocol,diagnostic,"Unable to remove the owned graphics compiler stage.");
        output=std::move(pipeline); return {Status::Ok,0};
    } catch (const std::bad_alloc&) { diagnostic.clear(); return {Status::OutOfMemory,0}; }
}
Result buildFragmentBufferGraphicsPipeline(const GraphicsShader& vertex,const GraphicsShader& fragment,
    std::shared_ptr<const FragmentBufferGraphicsPipeline>& output,std::string& diagnostic) {
    diagnostic.clear();
    try {
        if (output || vertex.stage()!=HN_SHADER_VERTEX || fragment.stage()!=HN_SHADER_FRAGMENT ||
            vertex.fragmentBufferBinding() || !fragment.fragmentBufferBinding() ||
            vertex.vertexBufferBinding() || fragment.vertexBufferBinding())
            return failure(Status::Invalid,diagnostic,"Expected resource-free vertex and admitted buffer fragment with empty output.");
        Stage stage;
        if (!stage.create()) return failure(Status::OutOfMemory,diagnostic,"Unable to create the private graphics compiler stage.");
        const std::string root=stage.path,vs=root+"/vertex.spv",fs=root+"/fragment.spv";
        if (!writeWords(vs,vertex.spirvWords()) || !writeWords(fs,fragment.spirvWords()))
            return failure(Status::Protocol,diagnostic,"Unable to write the actual validated stage SPIR-V.");
        {
            // This is the SAME mutex used by the unchanged resource-free path;
            // separate locks would race Mesa's process-global state.
            std::lock_guard<std::mutex> lock(compilerMutex);
            if (hn_gfx11_ngg_compile_validated_file(vs.c_str(),vertex.spirvEntryPoint().c_str(),(root+"/vertex").c_str()))
                return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the vertex shader.");
            if (hn_gfx11_fragment_buffer_compile_validated_file(fs.c_str(),fragment.spirvEntryPoint().c_str(),(root+"/fragment").c_str()))
                return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the buffer fragment shader.");
        }
        std::vector<uint8_t> vertexCode,fragmentCode;
        if (!readCode(root+"/vertex/shader.bin",vertexCode) || !readCode(root+"/fragment/shader.bin",fragmentCode))
            return failure(Status::Protocol,diagnostic,"The actual compiler did not produce bounded regular machine-code files.");
        std::shared_ptr<const FragmentBufferGraphicsPipeline> pipeline;
        const auto admitted=FragmentBufferGraphicsPipeline::capture(vertexCode.data(),vertexCode.size(),fragmentCode.data(),fragmentCode.size(),pipeline);
        if (!admitted) return failure(admitted.status,diagnostic,"The actual machine bytes failed independent buffer-fragment admission.");
        if (!stage.clear()) return failure(Status::Protocol,diagnostic,"Unable to remove the owned graphics compiler stage.");
        output=std::move(pipeline); return {Status::Ok,0};
    } catch (const std::bad_alloc&) { diagnostic.clear(); return {Status::OutOfMemory,0}; }
}
template<class PipelineType> static Result buildVertexBuffers(const GraphicsShader& vertex,const GraphicsShader& fragment,
    std::shared_ptr<const PipelineType>& output,std::string& diagnostic,bool fragmentBuffer) {
    diagnostic.clear();
    try {
        if (output || vertex.stage()!=HN_SHADER_VERTEX || fragment.stage()!=HN_SHADER_FRAGMENT ||
            !vertex.vertexBufferBinding() || vertex.fragmentBufferBinding() || fragment.vertexBufferBinding() ||
            bool(fragment.fragmentBufferBinding())!=fragmentBuffer)
            return failure(Status::Invalid,diagnostic,"Expected the admitted vertex buffer and matching fragment profile with empty output.");
        Stage stage;
        if (!stage.create()) return failure(Status::OutOfMemory,diagnostic,"Unable to create the private graphics compiler stage.");
        const std::string root=stage.path,vs=root+"/vertex.spv",fs=root+"/fragment.spv";
        if (!writeWords(vs,vertex.spirvWords()) || !writeWords(fs,fragment.spirvWords()))
            return failure(Status::Protocol,diagnostic,"Unable to write the actual validated stage SPIR-V.");
        {
            std::lock_guard<std::mutex> lock(compilerMutex);
            if (hn_gfx11_vertex_buffer_compile_validated_file(vs.c_str(),vertex.spirvEntryPoint().c_str(),(root+"/vertex").c_str()))
                return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the buffer vertex shader.");
            const int status=fragmentBuffer ?
                hn_gfx11_fragment_buffer_compile_validated_file(fs.c_str(),fragment.spirvEntryPoint().c_str(),(root+"/fragment").c_str()) :
                hn_gfx11_fragment_compile_validated_file(fs.c_str(),fragment.spirvEntryPoint().c_str(),(root+"/fragment").c_str());
            if (status) return failure(Status::Unsupported,diagnostic,"The actual Mesa/ACO compiler rejected the fragment shader.");
        }
        std::vector<uint8_t> vertexCode,fragmentCode;
        if (!readCode(root+"/vertex/shader.bin",vertexCode) || !readCode(root+"/fragment/shader.bin",fragmentCode))
            return failure(Status::Protocol,diagnostic,"The actual compiler did not produce bounded regular machine-code files.");
        std::shared_ptr<const PipelineType> pipeline;
        const auto admitted=PipelineType::capture(vertexCode.data(),vertexCode.size(),fragmentCode.data(),fragmentCode.size(),pipeline);
        if (!admitted) return failure(admitted.status,diagnostic,"The actual machine bytes failed independent vertex-buffer admission.");
        if (!stage.clear()) return failure(Status::Protocol,diagnostic,"Unable to remove the owned graphics compiler stage.");
        output=std::move(pipeline);return {Status::Ok,0};
    } catch (const std::bad_alloc&) { diagnostic.clear();return {Status::OutOfMemory,0}; }
}
Result buildVertexBufferGraphicsPipeline(const GraphicsShader& vertex,const GraphicsShader& fragment,
    std::shared_ptr<const VertexBufferGraphicsPipeline>& output,std::string& diagnostic) {
    return buildVertexBuffers(vertex,fragment,output,diagnostic,false);
}
Result buildVertexFragmentBufferGraphicsPipeline(const GraphicsShader& vertex,const GraphicsShader& fragment,
    std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>& output,std::string& diagnostic) {
    return buildVertexBuffers(vertex,fragment,output,diagnostic,true);
}
} }
