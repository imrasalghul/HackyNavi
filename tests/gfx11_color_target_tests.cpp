// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_color_target.h"
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>

namespace {
unsigned checks = 0, failures = 0, products = 0;
void check(bool value, int line) {
    ++checks;
    if (!value) { if (failures < 15) std::fprintf(stderr, "check failed line %d\n", line); ++failures; }
}
#define CHECK(x) check((x), __LINE__)
HNColorLayout expected(HNColorRequest r) {
    const uint32_t row = ((r.width * 4 + 255) / 256) * 256;
    const uint64_t size = uint64_t(row) * r.height;
    return {r.width, r.height, row / 4, row, size, size, 256, 0,
            0x2800a, 0, 0, (r.width - 1) * 16384 + r.height - 1, 0x01000000, 0x428};
}
bool zero(const HNColorLayout& value) {
    const HNColorLayout empty = {};
    return std::memcmp(&empty, &value, sizeof(value)) == 0;
}
void failure(const HNColorTopology& t, const HNColorRequest& r, HNColorStatus status) {
    CHECK(hn_color_target_validate_inputs(&t, &r) == status);
    HNColorLayout s = expected({1, 1});
    CHECK(hn_color_target_validate(&t, &r, &s) == status);
#ifdef HN_COLOR_MESA_TEST
    CHECK(hn_color_target_calculate(&t, &r, &s) == status);
    CHECK(zero(s));
#else
    CHECK(!zero(s));
#endif
}
}
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    std::FILE* records = argc == 2 ? std::fopen(argv[1], "wb") : nullptr;
    if (argc == 2 && !records) return 2;
    const uint32_t dimensions[][2] = {
        {1,1},{2,3},{63,7},{64,9},{65,11},{255,256},{256,257},{257,255},
        {1920,1080},{3840,2160},{4097,4099},{16383,16384},{16384,16383},{16384,16384}
    };
    for (uint32_t pipes = 0; pipes <= 6; ++pipes) {
        for (uint32_t packers = 0; packers <= pipes; ++packers) {
            if (pipes - packers > 2 || packers > 5) continue;
            // Numerical test inputs, not a captured Navi31 configuration.
            // Deprecated bit6 keeps the one-pipe/one-packer case nonzero.
            HNColorTopology t = {pipes | packers << 8 | 0x40, 0};
            for (uint32_t rev : {0u, 14u}) {
                t.revision_strap = rev << 24;
                for (const auto& dim : dimensions) {
                    const HNColorRequest r = {dim[0], dim[1]};
                    CHECK(hn_color_target_validate_inputs(&t, &r) == HN_COLOR_OK);
                    HNColorLayout layout = expected(r);
#ifdef HN_COLOR_MESA_TEST
                    struct Guarded { uint64_t before; HNColorLayout value; uint64_t after; } guarded;
                    std::memset(&guarded, 0xa7, sizeof(guarded));
                    const auto result = hn_color_target_calculate(&t, &r, &guarded.value);
                    if (result != HN_COLOR_OK && failures < 15)
                        std::fprintf(stderr, "actual Mesa rejected p%u k%u rev%u %ux%u status%d\n", pipes,packers,rev,r.width,r.height,int(result));
                    CHECK(result == HN_COLOR_OK);
                    CHECK(guarded.before == UINT64_C(0xa7a7a7a7a7a7a7a7));
                    CHECK(guarded.after == UINT64_C(0xa7a7a7a7a7a7a7a7));
                    CHECK(std::memcmp(&guarded.value, &layout, sizeof(layout)) == 0);
                    layout = guarded.value;
                    if (result == HN_COLOR_OK) ++products;
#endif
                    CHECK(hn_color_target_validate(&t, &r, &layout) == HN_COLOR_OK);
                    if (records) {
                        CHECK(std::fwrite(&t,sizeof(t),1,records)==1);
                        CHECK(std::fwrite(&r,sizeof(r),1,records)==1);
                        CHECK(std::fwrite(&layout,sizeof(layout),1,records)==1);
                    }
                    // Every bit of the pointer-free product is semantically checked.
                    for (size_t bit = 0; bit < sizeof(layout) * 8; ++bit) {
                        auto changed = layout;
                        reinterpret_cast<unsigned char*>(&changed)[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
                        CHECK(hn_color_target_validate(&t, &r, &changed) == HN_COLOR_INVALID_LAYOUT);
                    }
                }
            }
        }
    }
    const HNColorTopology valid = {0x405, 0};
    const HNColorRequest one = {1,1};
    for (uint32_t value : {0u, 16385u, UINT32_MAX}) {
        failure(valid, {value,1}, HN_COLOR_INVALID_DIMENSIONS);
        failure(valid, {1,value}, HN_COLOR_INVALID_DIMENSIONS);
    }
    for (uint32_t bits : {0u,UINT32_MAX,7u,0x8u,0x10u,0x18u,0x20u,0x28u,0x30u,0x38u,0x700u,6u,0x606u})
        failure({bits,0}, one, HN_COLOR_UNSUPPORTED_TOPOLOGY);
    failure({0x405,UINT32_MAX},one,HN_COLOR_UNSUPPORTED_TOPOLOGY);
    failure({0x405,0x0f000000},one,HN_COLOR_UNSUPPORTED_TOPOLOGY);
    // Exhaust the encoded interleave/pipe/packer space at a valid tiny image.
    // The admitted pairs come from the source table's 75-entry address range.
    const uint32_t admitted[] = {0x40,0x41,0x141,0x42,0x142,0x242,0x143,0x243,0x343,
                                0x244,0x344,0x444,0x345,0x445,0x545,0x446,0x546};
    for (uint32_t pipe=0;pipe<8;++pipe) for(uint32_t pkr=0;pkr<8;++pkr) for(uint32_t interleave=0;interleave<8;++interleave) {
        const HNColorTopology t={0x40u|pipe|(pkr<<8)|(interleave<<3),0};
        bool accept=false;for(uint32_t bits:admitted) if(t.gb_addr_config==bits)accept=true;
        const auto status=accept?HN_COLOR_OK:HN_COLOR_UNSUPPORTED_TOPOLOGY;
        CHECK(hn_color_target_validate_inputs(&t,&one)==status);
#ifdef HN_COLOR_MESA_TEST
        HNColorLayout value=expected(one);
        CHECK(hn_color_target_calculate(&t,&one,&value)==status);
        CHECK(accept?hn_color_target_validate(&t,&one,&value)==HN_COLOR_OK:zero(value));
        if(accept)++products;
#endif
    }
    // Unrelated valid strap/config bits do not silently become new topology.
    const HNColorTopology unrelated = {0x50000405, 0x50ffffff};
    CHECK(hn_color_target_validate_inputs(&unrelated,&one)==HN_COLOR_OK);
    HNColorLayout layout = expected(one);
    CHECK(hn_color_target_validate(nullptr,&one,&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate(&valid,nullptr,&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate(&valid,&one,nullptr)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate(&valid,&one,reinterpret_cast<const HNColorLayout*>(UINTPTR_MAX-7))==HN_COLOR_INVALID_ARGUMENT);
    alignas(8) unsigned char unaligned[80] = {};
    CHECK(hn_color_target_validate(reinterpret_cast<const HNColorTopology*>(unaligned+1),&one,&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate(&valid,reinterpret_cast<const HNColorRequest*>(unaligned+1),&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate(&valid,&one,reinterpret_cast<const HNColorLayout*>(unaligned+1))==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_validate_inputs(reinterpret_cast<const HNColorTopology*>(UINTPTR_MAX-3),&one)==HN_COLOR_INVALID_ARGUMENT);
#ifdef HN_COLOR_MESA_TEST
    CHECK(hn_color_target_calculate(nullptr,&one,&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(zero(layout));
    CHECK(hn_color_target_calculate(&valid,nullptr,&layout)==HN_COLOR_INVALID_ARGUMENT);
    CHECK(zero(layout));
    CHECK(hn_color_target_calculate(&valid,&one,reinterpret_cast<HNColorLayout*>(UINTPTR_MAX-7))==HN_COLOR_INVALID_ARGUMENT);
    CHECK(hn_color_target_calculate(&valid,&one,nullptr)==HN_COLOR_INVALID_ARGUMENT);
    std::memset(unaligned,0xb9,sizeof(unaligned));
    CHECK(hn_color_target_calculate(&valid,&one,reinterpret_cast<HNColorLayout*>(unaligned+1))==HN_COLOR_INVALID_ARGUMENT);
    for (unsigned char c : unaligned) CHECK(c==0xb9);
    // Input/output overlap is rejected before dereferencing either input.
    alignas(8) unsigned char overlap[64] = {};
    std::memcpy(overlap,&valid,sizeof(valid)); std::memcpy(overlap+8,&one,sizeof(one));
    CHECK(hn_color_target_calculate(reinterpret_cast<HNColorTopology*>(overlap),
          reinterpret_cast<HNColorRequest*>(overlap+8),reinterpret_cast<HNColorLayout*>(overlap))==HN_COLOR_INVALID_ARGUMENT);
    for (unsigned char c : overlap) CHECK(c==0);
#endif
    if (records) CHECK(std::fclose(records)==0);
    std::printf("%u checks, %u failures, %u actual Mesa products; no GPU executed\n",checks,failures,products);
    return failures ? 1 : 0;
}
