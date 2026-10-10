// SPDX-License-Identifier: MIT
// Pure byte admission/launch tests. No GPU execution or model of driver ownership.
#include "hackynavi/gfx11_dispatch_product.hpp"
#include <cstdio>
#include <cstring>
#include <utility>
using namespace hackynavi;
using namespace hackynavi::gfx11dispatch;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);}}while(false)
// Actual independent ACO products from compute-products/products.json. These
// bytes are test inputs, never a production allowlist. The leaf decoder derives
// validity/access/RSRC on every call regardless of these recorded identities.
const char* const codeHex[]={
    "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f8000046d6040c010407fc89bf010087bf000047d606000a02000050e000014280f70389bf8102024a000068e0000143800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf",
    "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f80483049607fc89bf990087bf000055d60608000482000030000050e000014280f70389bf8102024a000068e0000143800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf",
    "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f8000046d6040c010407fc89bf910087bf0600004a82000230000050e001024280f70389bf02002cd787040202010087bf020045d6ff0402045a5a5a5a000068e0010243800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf",
    "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f80483049607fc89bf990087bf000055d60608000482000230000050e001024280f70389bf02002cd787040202010087bf020045d6ff0402045a5a5a5a000068e0010243800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf"
};
unsigned nibble(char c){return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);}
uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(uint8_t* p,uint32_t word){for(unsigned n=0;n<4;++n)p[n]=uint8_t(word>>(n*8));}
gfxisolated::Binding binding(uint32_t id,uint64_t address,uint32_t access,uint64_t bytes=4096) {
    return {0x12345678,0xabcdef98,id,7,address,bytes,0,bytes,access};
}
struct Fixture {
    uint8_t code[8][4096];Variant variants[8];Resource resources[32];Request request;Workspace workspace;
    uint8_t parameters[4096];uint32_t commands[1024];size_t words;
    explicit Fixture(unsigned kernel=0):code{},variants{},resources{},request{},workspace{},parameters{},commands{},words(0) {
        request.owner=0x12345678;request.epoch=0xabcdef98;request.vmid=1;
        request.grid[0]=4099;request.grid[1]=request.grid[2]=1;
        request.nominalLocal[0]=64;request.nominalLocal[1]=request.nominalLocal[2]=1;
        request.variants=variants;request.variantCount=2;request.resources=resources;request.resourceCount=2;
        request.indirect=binding(50,0x40000040000ULL,5);request.parameters=binding(51,0x50000050000ULL,1);
        resources[0]={0,binding(40,0x20000020000ULL,1,20480),0,16640};
        resources[1]={1,binding(41,0x30000030000ULL,3,20480),0,16640};
        for(unsigned n=0;n<2;++n) {
            const unsigned index=kernel*2+n;const size_t bytes=std::strlen(codeHex[index])/2;
            for(size_t b=0;b<bytes;++b)code[n][b]=uint8_t(nibble(codeHex[index][2*b])*16+nibble(codeHex[index][2*b+1]));
            variants[n].program={code[n],bytes,bytes-20,{n?3U:64U,1,1},32,108,24};
            variants[n].shader=binding(n+1,0x10000010000ULL+n*4096,5);
        }
    }
    bool build(){return parameterImage(request,workspace,parameters,sizeof(parameters))==Status::Ok&&
        encode(request,workspace,commands,1024,words)==Status::Ok;}
    Status check(){return validate(request,workspace,parameters,sizeof(parameters),commands,words);}
    // A real, hand-encoded terminating leaf program for exhaustive geometric
    // planning tests. It performs no memory access. It is not advertised as a
    // compiler product or evidence of hardware execution.
    void geometry(const uint32_t* grid,const uint32_t* nominal) {
        request.resourceCount=0;request.variantCount=0;
        for(unsigned d=0;d<3;++d){request.grid[d]=grid[d];request.nominalLocal[d]=nominal[d];}
        for(unsigned mask=0;mask<8;++mask) {
            uint32_t local[3];bool nonempty=true;
            for(unsigned d=0;d<3;++d) {
                const uint32_t full=grid[d]/nominal[d],tail=grid[d]%nominal[d];
                local[d]=(mask&(1U<<d))?tail:nominal[d];
                if((mask&(1U<<d))?!tail:!full)nonempty=false;
            }
            if(!nonempty)continue;
            const size_t n=request.variantCount++;std::memset(code[n],0,4096);
            put32(code[n],0xbfb00000);for(size_t p=4;p<24;p+=4)put32(code[n]+p,0xbf9f0000);
            variants[n].program={code[n],24,4,{local[0],local[1],local[2]},32,108,24};
            variants[n].shader=binding(uint32_t(n+1),0x10000010000ULL+n*4096,5);
        }
    }
};
// Decode PM4 independently and check every launch state at DISPATCH_DIRECT,
// and every ordered completion/cache pair before any subsequent launch.
void inspectPackets(const Fixture& f) {
    uint32_t sh[0x300]={};bool written[0x300]={};size_t cursor=0,dispatches=0,flushes=0,acquires=0,syncs=0;
    bool pending=false,partial=false;
    while(cursor<f.words) {
        const uint32_t header=f.commands[cursor++];if(header==0xffff1000)continue;
        CHECK((header>>30)==3);const size_t payload=((header>>16)&0x3fff)+1;
        CHECK(payload<=f.words-cursor);if(payload>f.words-cursor)return;
        const uint32_t op=(header>>8)&0xff;
        if(op==0x76) {
            CHECK(!pending);CHECK((header&3)==2);const uint32_t first=f.commands[cursor];
            CHECK(first+payload-1<=0x300);
            for(size_t j=1;j<payload&&first+j-1<0x300;++j){sh[first+j-1]=f.commands[cursor+j];written[first+j-1]=true;}
        } else if(op==0x15) {
            CHECK(!pending);CHECK(payload==4);CHECK(dispatches<f.workspace.regionCount);
            if(dispatches>=f.workspace.regionCount)return;
            const Region& region=f.workspace.regions[dispatches];const Variant& v=f.variants[region.variant];
            const gfx11product::Report& report=f.workspace.variants[region.variant];
            for(unsigned d=0;d<3;++d){CHECK(f.commands[cursor+d]==region.groups[d]);CHECK(sh[0x207+d]==region.local[d]);}
            CHECK(f.commands[cursor+3]==0x8001);CHECK(sh[0x20c]==uint32_t(v.shader.gpu>>8));
            CHECK(sh[0x20d]==uint32_t(v.shader.gpu>>40));
            CHECK(written[0x212]&&sh[0x212]==report.rsrc1);CHECK(written[0x213]&&sh[0x213]==report.rsrc2);
            CHECK(written[0x228]&&sh[0x228]==report.rsrc3);
            const uint64_t push=f.request.parameters.gpu+512+dispatches*48;
            CHECK(sh[0x240]==uint32_t(f.request.parameters.gpu)&&sh[0x241]==uint32_t(f.request.parameters.gpu>>32));
            CHECK(sh[0x242]==uint32_t(push)&&sh[0x243]==uint32_t(push>>32));
            CHECK(sh[0x204]==0&&sh[0x205]==0&&sh[0x206]==0);
            CHECK(written[0x218]&&sh[0x218]==0&&written[0x215]&&sh[0x215]==0);
            ++dispatches;pending=true;
        } else if(op==0x46) {
            CHECK(pending&&!partial);CHECK(payload==1&&f.commands[cursor]==0x407);partial=true;++flushes;
        } else if(op==0x58) {
            CHECK(!pending||partial);CHECK(payload==7);CHECK((header&3)==0);
            // Mesa packet3.json ACQUIRE_MEM: PFP engine0, entire48-bit range.
            CHECK(f.commands[cursor]==0&&f.commands[cursor+1]==UINT32_MAX&&f.commands[cursor+2]==0xffffff);
            CHECK(f.commands[cursor+3]==0&&f.commands[cursor+4]==0&&f.commands[cursor+5]==10);
            const uint32_t gcr=f.commands[cursor+6];
            // GLI_ALL[1:0], GLM_WB4, GLM_INV5, GLK_INV7, GLV_INV8,
            // GL1_INV9, GL2_INV14, GL2_WB15; no range/sequence modifiers.
            CHECK(gcr==(1U|(1U<<4)|(1U<<5)|(1U<<7)|(1U<<8)|(1U<<9)|(1U<<14)|(1U<<15)));
            pending=false;partial=false;++acquires;
        } else if(op==0x42){CHECK(!pending&&!partial);CHECK(payload==1&&f.commands[cursor]==0);++syncs;}
        else CHECK(op==0x28||op==0x79||op==0x9b);
        cursor+=payload;
    }
    CHECK(dispatches==f.workspace.regionCount&&flushes==dispatches&&acquires==dispatches+1&&syncs==1);
    CHECK(!pending&&!partial&&!(f.words&7)&&f.words<=1024);
}
void actualProducts() {
    for(unsigned kernel=0;kernel<2;++kernel) {
        Fixture f(kernel);CHECK(f.build());CHECK(f.check()==Status::Ok);
        CHECK(f.workspace.regionCount==2&&f.workspace.readMask==1&&f.workspace.writeMask==2);
        CHECK(f.workspace.regions[0].variant==0&&f.workspace.regions[1].variant==1);
        const uint32_t push0[]={4099,1,1,0,0,0,0,0,0,65,1,1};
        const uint32_t push1[]={4099,1,1,4096,0,0,64,0,0,65,1,1};
        for(unsigned n=0;n<12;++n){CHECK(le32(f.parameters+512+n*4)==push0[n]);CHECK(le32(f.parameters+560+n*4)==push1[n]);}
        for(size_t n=32;n<512;++n)CHECK(f.parameters[n]==0);
        for(size_t n=608;n<4096;++n)CHECK(f.parameters[n]==0);
        for(unsigned n=0;n<2;++n) {
            CHECK(le32(f.parameters+n*16)==uint32_t(f.resources[n].mapping.gpu));
            CHECK(le32(f.parameters+n*16+4)==uint32_t(f.resources[n].mapping.gpu>>32));
            CHECK(le32(f.parameters+n*16+8)==16640);CHECK(le32(f.parameters+n*16+12)==0x30016fac);
        }
        inspectPackets(f);
        // No report/workspace can bypass re-derivation from bytes.
        std::memset(&f.workspace,0xff,sizeof(f.workspace));CHECK(f.check()==Status::Ok);CHECK(f.workspace.readMask==1);
        std::swap(f.variants[0],f.variants[1]);CHECK(f.build());CHECK(f.check()==Status::Ok);
        CHECK(f.workspace.regions[0].variant==1&&f.workspace.regions[1].variant==0);inspectPackets(f);
        for(uint8_t id=1;id<=7;++id){f.request.vmid=id;CHECK(f.check()==Status::Ok);}
        f.resources[0].offset=12;f.resources[1].offset=3840;
        f.resources[0].mapping.bufferOffset=4096;f.resources[0].mapping.allocationBytes+=4096;
        CHECK(f.build());CHECK(f.check()==Status::Ok);CHECK(le32(f.parameters)==uint32_t(f.resources[0].mapping.gpu+12));
        f.resources[1].mapping.gpu=(UINT64_C(1)<<47)-20480;
        CHECK(f.build());CHECK(f.check()==Status::Ok);CHECK(le32(f.parameters+20)==0x7fff);
    }
}
void geometry() {
    const uint32_t local[]={4,3,2};
    for(uint32_t x=1;x<=9;++x)for(uint32_t y=1;y<=7;++y)for(uint32_t z=1;z<=5;++z) {
        Fixture f;const uint32_t grid[]={x,y,z};f.geometry(grid,local);
        CHECK(f.build());CHECK(f.check()==Status::Ok);CHECK(f.workspace.regionCount==f.request.variantCount);
        unsigned seen[9*7*5]={};
        for(size_t n=0;n<f.workspace.regionCount;++n) {
            const Region& r=f.workspace.regions[n];
            for(unsigned d=0;d<3;++d){CHECK(r.push[d]==grid[d]);CHECK(r.push[9+d]==(grid[d]+local[d]-1)/local[d]);}
            for(uint32_t a=0;a<r.local[0]*r.groups[0];++a)
                for(uint32_t b=0;b<r.local[1]*r.groups[1];++b)
                    for(uint32_t c=0;c<r.local[2]*r.groups[2];++c) {
                        const uint32_t px=r.push[3]+a,py=r.push[4]+b,pz=r.push[5]+c;
                        CHECK(px<x&&py<y&&pz<z);if(px<x&&py<y&&pz<z)++seen[(pz*y+py)*x+px];
                    }
        }
        for(uint32_t n=0;n<x*y*z;++n)CHECK(seen[n]==1);
        if(f.workspace.regionCount==8)inspectPackets(f);
    }
    Fixture f;const uint32_t huge[]={UINT32_MAX,UINT32_MAX,1},one[]={1,1,1};f.geometry(huge,one);
    CHECK(f.build());CHECK(f.check()==Status::Ok);CHECK(f.workspace.regions[0].groups[0]==UINT32_MAX);
    f.request.grid[2]=2;CHECK(prepare(f.request,f.workspace)==Status::InvalidDimensions);
}
void pageExtentsAndDescriptorLimits() {
    Fixture f;uint8_t code[16384]={};
    put32(code,0xbfb00000);for(size_t p=4;p<4100;p+=4)put32(code+p,0xbf9f0000);
    f.request.variantCount=1;f.request.resourceCount=0;
    f.request.grid[0]=f.request.nominalLocal[0]=1;
    f.variants[0].program={code,4100,4,{1,1,1},32,8,1};
    f.variants[0].shader.bytes=f.variants[0].shader.allocationBytes=8192;
    CHECK(f.build());CHECK(f.check()==Status::Ok);inspectPackets(f);
    CHECK(f.workspace.variants[0].rsrc1==0x202c0000);
    code[8191]=1;CHECK(f.check()==Status::InvalidShader);code[8191]=0;
    f.variants[0].shader.bytes=f.variants[0].shader.allocationBytes=4096;
    CHECK(f.check()==Status::InvalidBinding);
    for(size_t p=4100;p<sizeof(code);p+=4)put32(code+p,0xbf9f0000);
    f.variants[0].program.codeBytes=sizeof(code);
    f.variants[0].shader.bytes=f.variants[0].shader.allocationBytes=sizeof(code);
    CHECK(f.build());CHECK(f.check()==Status::Ok);
    f.variants[0].program.codeBytes=sizeof(code)+4;CHECK(f.check()==Status::InvalidShader);
    f.variants[0].program.codeBytes=sizeof(code);f.variants[0].program.code=nullptr;
    CHECK(f.check()==Status::InvalidShader);
    f.variants[0].program.code=reinterpret_cast<const uint8_t*>(UINTPTR_MAX-1023);
    CHECK(f.check()==Status::InvalidShader);
    Fixture bounds;
    bounds.resources[0].mapping.bytes=bounds.resources[0].mapping.allocationBytes=UINT64_C(1)<<32;
    bounds.resources[0].bytes=UINT32_MAX-3;
    CHECK(bounds.build());CHECK(bounds.check()==Status::Ok);CHECK(le32(bounds.parameters+8)==0xfffffffc);
    bounds.resources[0].bytes=UINT64_C(1)<<32;CHECK(bounds.check()==Status::InvalidResources);
    bounds.resources[0].bytes=UINT32_MAX-3;bounds.resources[0].offset=4;
    CHECK(bounds.build());CHECK(bounds.check()==Status::Ok);
    bounds.resources[0].offset=8;CHECK(bounds.check()==Status::InvalidResources);
}
void malformed() {
    for(unsigned which=0;which<8;++which) {
        Fixture f;CHECK(f.build());
        if(which==0)f.request.owner=0;if(which==1)f.request.epoch=0;if(which==2)f.request.vmid=0;
        if(which==3)f.request.vmid=8;if(which==4)f.request.variantCount=0;if(which==5)f.request.variantCount=9;
        if(which==6)f.request.variants=nullptr;if(which==7)f.request.resourceCount=33;
        CHECK(f.check()==Status::InvalidArgument);
    }
    for(unsigned d=0;d<3;++d)for(unsigned op=0;op<3;++op) {
        Fixture f;if(op==0)f.request.grid[d]=0;if(op==1)f.request.nominalLocal[d]=0;if(op==2)f.request.nominalLocal[d]=1025;
        CHECK(prepare(f.request,f.workspace)==Status::InvalidDimensions);
    }
    Fixture f;f.request.nominalLocal[1]=1024;CHECK(prepare(f.request,f.workspace)==Status::InvalidDimensions);
    Fixture missing;missing.request.variantCount=1;CHECK(prepare(missing.request,missing.workspace)==Status::MissingVariant);
    Fixture unused;unused.request.grid[0]=4096;CHECK(prepare(unused.request,unused.workspace)==Status::UnusedVariant);
    Fixture duplicate;duplicate.variants[1].program.local[0]=64;CHECK(prepare(duplicate.request,duplicate.workspace)==Status::InvalidShader);
    for(unsigned n=0;n<9;++n) {
        Fixture bad;
        if(n==0)bad.resources[1].binding=0;if(n==1)bad.resources[1].binding=32;if(n==2)bad.resources[1].binding=3;
        if(n==3)bad.resources[0].bytes=0;if(n==4)bad.resources[0].bytes=UINT64_MAX;if(n==5)bad.resources[0].offset=UINT64_MAX;
        if(n==6)bad.resources[0].offset=1;if(n==7)bad.resources[0].bytes=3;if(n==8)bad.request.resourceCount=1;
        CHECK(prepare(bad.request,bad.workspace)==Status::InvalidResources);
    }
    for(unsigned selected=0;selected<6;++selected)for(unsigned mutation=0;mutation<16;++mutation) {
        Fixture bad;gfxisolated::Binding* all[]={&bad.variants[0].shader,&bad.variants[1].shader,
            &bad.resources[0].mapping,&bad.resources[1].mapping,&bad.request.indirect,&bad.request.parameters};
        auto& b=*all[selected];
        if(mutation==0)b.owner++;if(mutation==1)b.epoch++;if(mutation==2)b.id=0;if(mutation==3)b.generation=0;
        if(mutation==4)b.bytes=0;if(mutation==5)b.gpu=0;if(mutation==6)b.gpu=UINT64_C(1)<<47;
        if(mutation==7)b.gpu++;if(mutation==8)b.bytes++;if(mutation==9)b.bufferOffset++;
        if(mutation==10)b.allocationBytes--;if(mutation==11)b.bytes=UINT64_MAX-4095;
        if(mutation==12)b.bufferOffset=b.allocationBytes;if(mutation==13)b.access^=2;
        if(mutation==14)b.access|=8;if(mutation==15)b.access=0;
        CHECK(prepare(bad.request,bad.workspace)!=Status::Ok);
    }
    for(unsigned a=0;a<6;++a)for(unsigned b=a+1;b<6;++b)for(unsigned mutation=0;mutation<2;++mutation) {
        Fixture bad;gfxisolated::Binding* all[]={&bad.variants[0].shader,&bad.variants[1].shader,
            &bad.resources[0].mapping,&bad.resources[1].mapping,&bad.request.indirect,&bad.request.parameters};
        if(mutation==0){all[b]->id=all[a]->id;++all[b]->generation;}
        else all[b]->gpu=all[a]->gpu;
        CHECK(prepare(bad.request,bad.workspace)==(mutation?Status::Overlap:Status::AliasedStorage));
    }
    for(unsigned n=0;n<4;++n) {
        Fixture bad;gfxisolated::Binding* b[]={&bad.variants[0].shader,&bad.variants[1].shader,&bad.request.indirect,&bad.request.parameters};
        b[n]->bytes=b[n]->allocationBytes=8192;CHECK(prepare(bad.request,bad.workspace)==Status::InvalidBinding);
    }
}
void mutationsAndTransactional() {
    Fixture f;CHECK(f.build());
    for(size_t byte=0;byte<4096;++byte)for(unsigned bit=0;bit<8;++bit) {
        f.parameters[byte]^=uint8_t(1U<<bit);CHECK(f.check()==Status::InvalidParameters);f.parameters[byte]^=uint8_t(1U<<bit);
    }
    for(size_t word=0;word<f.words;++word)for(unsigned bit=0;bit<32;++bit) {
        f.commands[word]^=1U<<bit;CHECK(f.check()==Status::InvalidStream);f.commands[word]^=1U<<bit;
    }
    for(size_t n=1;n<f.words;++n)CHECK(validate(f.request,f.workspace,f.parameters,4096,f.commands,n)==Status::InvalidStream);
    f.commands[f.words]=0xffff1000;CHECK(validate(f.request,f.workspace,f.parameters,4096,f.commands,f.words+1)==Status::InvalidStream);
    for(unsigned variant=0;variant<2;++variant)for(size_t n=f.variants[variant].program.codeBytes;n<4096;++n) {
        f.code[variant][n]=1;CHECK(f.check()==Status::InvalidShader);f.code[variant][n]=0;
    }
    // Rejection is based on fresh bytes even with a stale successful report.
    f.code[1][0]^=0xff;CHECK(f.check()==Status::InvalidShader);
    uint8_t output[4096];std::memset(output,0xa5,sizeof(output));uint32_t stream[1024];
    for(auto& word:stream)word=0xa5a5a5a5;size_t words=0x9876;
    CHECK(parameterImage(f.request,f.workspace,output,4096)==Status::InvalidShader);
    CHECK(encode(f.request,f.workspace,stream,1024,words)==Status::InvalidShader);
    f.code[1][0]^=0xff;
    CHECK(parameterImage(f.request,f.workspace,output,4095)==Status::BufferTooSmall);
    CHECK(encode(f.request,f.workspace,stream,f.words-1,words)==Status::BufferTooSmall);
    CHECK(words==0x9876);for(auto byte:output)CHECK(byte==0xa5);for(auto word:stream)CHECK(word==0xa5a5a5a5);
    CHECK(parameterImage(f.request,f.workspace,nullptr,4096)==Status::InvalidArgument);
    CHECK(parameterImage(f.request,f.workspace,reinterpret_cast<uint8_t*>(UINTPTR_MAX-4094),4096)==Status::InvalidArgument);
    CHECK(encode(f.request,f.workspace,nullptr,1024,words)==Status::InvalidArgument);
    CHECK(encode(f.request,f.workspace,reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(stream)+1),1024,words)==Status::InvalidArgument);
    CHECK(encode(f.request,f.workspace,reinterpret_cast<uint32_t*>(UINTPTR_MAX-3),1024,words)==Status::InvalidArgument);
    CHECK(validate(f.request,f.workspace,nullptr,4096,f.commands,f.words)==Status::InvalidArgument);
    CHECK(validate(f.request,f.workspace,f.parameters,4095,f.commands,f.words)==Status::InvalidArgument);
    CHECK(validate(f.request,f.workspace,f.parameters,4096,f.commands,0)==Status::InvalidArgument);
    CHECK(validate(f.request,f.workspace,f.parameters,4096,f.commands,SIZE_MAX)==Status::InvalidArgument);
    CHECK(validate(f.request,f.workspace,f.parameters,4096,reinterpret_cast<uint32_t*>(UINTPTR_MAX-3),f.words)==Status::InvalidArgument);
}
}
int main(){actualProducts();geometry();pageExtentsAndDescriptorLimits();malformed();mutationsAndTransactional();
    std::printf("gfx11-dispatch-product: %zu checks, %zu failures\n",checks,failures);return failures?1:0;}
