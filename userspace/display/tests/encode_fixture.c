// SPDX-License-Identifier: MIT
#include "dml32_plan.h"
#include "reference_profile.h"
#include "mode_fixture.h"
// Explicit reference-only test policies. No board policy/default is linked
// into either the production calculator or its integer encoding adapter.
int hn_test_calculations(int four_k,struct hn_dml_plan out[4]) {
    display_e2e_pipe_params_st p=mode1080();
    if(four_k) {
        p.pipe.src.surface_width_y=p.pipe.src.viewport_width=p.pipe.src.viewport_width_max=3840;
        p.pipe.src.surface_height_y=p.pipe.src.viewport_height=p.pipe.src.viewport_height_max=2160;
        p.pipe.src.data_pitch=3840;
        p.pipe.dest.recout_width=p.pipe.dest.full_recout_width=p.pipe.dest.hactive=3840;
        p.pipe.dest.recout_height=p.pipe.dest.full_recout_height=p.pipe.dest.vactive=2160;
        p.pipe.dest.htotal=4400;p.pipe.dest.hblank_start=4224;p.pipe.dest.hblank_end=384;
        p.pipe.dest.vtotal=p.pipe.dest.vtotal_min=p.pipe.dest.vtotal_max=2250;
        p.pipe.dest.vblank_start=2242;p.pipe.dest.vblank_end=82;
        p.pipe.dest.vfront_porch=8;p.pipe.dest.vblank_nom=90;p.pipe.dest.pixel_rate_mhz=594;
    }
    for(unsigned n=0;n<4;++n) {
        soc_bounding_box_st soc=reference_soc;
        soc.max_vratio_pre=4;
        soc.urgent_latency_pixel_data_only_us+=n;
        if(hn_dml32_calculate(&soc,&reference_ip,&p,&out[n])!=HN_DML_OK)return 0;
    }
    return 1;
}
