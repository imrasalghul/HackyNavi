// SPDX-License-Identifier: MIT
// Compile the unchanged pinned upstream ABI itself, not a copied replacement.
#include <cstdint>
#include <cstddef>
#include <iostream>
#include "psp_gfx_if.h"
static_assert(GFX_CMD_ID_LOAD_TA==1 && GFX_CMD_ID_UNLOAD_TA==2 && GFX_CMD_ID_INVOKE_CMD==3,"command IDs");
static_assert(sizeof(psp_gfx_cmd_resp)==1024 && offsetof(psp_gfx_cmd_resp,resp)==864,"response bounds");
static_assert(offsetof(psp_gfx_cmd_resp,cmd)==28 && offsetof(psp_gfx_cmd_resp,cmd_id)==8,"request bounds");
static_assert(offsetof(psp_gfx_cmd_load_ta,app_phy_addr_lo)==0 && offsetof(psp_gfx_cmd_load_ta,app_phy_addr_hi)==4,"firmware MC");
static_assert(offsetof(psp_gfx_cmd_load_ta,app_len)==8 && offsetof(psp_gfx_cmd_load_ta,cmd_buf_phy_addr_lo)==12,"load inputs");
static_assert(offsetof(psp_gfx_cmd_load_ta,cmd_buf_phy_addr_hi)==16 && offsetof(psp_gfx_cmd_load_ta,cmd_buf_len)==20,"shared inputs");
static_assert(sizeof(psp_gfx_cmd_load_ta)==24 && sizeof(psp_gfx_cmd_unload_ta)==4,"load/unload size");
static_assert(offsetof(psp_gfx_cmd_invoke_cmd,session_id)==0 && offsetof(psp_gfx_cmd_invoke_cmd,ta_cmd_id)==4,"invoke identity");
static_assert(offsetof(psp_gfx_cmd_invoke_cmd,buf)==8 && sizeof(psp_gfx_buf_list)==776,"persistent invoke zeros");
static_assert(offsetof(psp_gfx_resp,status)==0 && offsetof(psp_gfx_resp,session_id)==4,"response session");
int main() { std::cout<<"Actual pinned PSP ABI: 1024-byte command, 864-byte request, session response at868; TA1/2/3 verified\n"; }
