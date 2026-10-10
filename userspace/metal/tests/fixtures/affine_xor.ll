; Authored HackyNavi fixture, SPDX-License-Identifier: MIT.
; output[tid] = ((input[tid] * 7u) ^ 0x5a5a5a5au) + tid;
; This is a separate kernel, not a renamed copy of add_one.
target triple = "air64-apple-macosx15.0.0"
define void @affine_xor(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %tid) {
entry:
  %source = getelementptr inbounds i32, ptr addrspace(1) %input, i32 %tid
  %value = load i32, ptr addrspace(1) %source, align 4
  %scaled = mul i32 %value, 7
  %mixed = xor i32 %scaled, 1515870810
  %result = add i32 %mixed, %tid
  %destination = getelementptr inbounds i32, ptr addrspace(1) %output, i32 %tid
  store i32 %result, ptr addrspace(1) %destination, align 4
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @affine_xor, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"input"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"output"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
