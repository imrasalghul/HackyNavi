; Authored HackyNavi compiler fixture (MIT), not a captured Apple shader.
; Equivalent Metal: kernel void add_one(device const uint *input [[buffer(0)]],
; device uint *output [[buffer(1)]], uint tid [[thread_position_in_grid]]) {
;   output[tid] = input[tid] + 1u;
; }
target triple = "air64-apple-macosx15.0.0"

define void @add_one(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %tid) {
entry:
  %source = getelementptr inbounds i32, ptr addrspace(1) %input, i32 %tid
  %value = load i32, ptr addrspace(1) %source, align 4
  %incremented = add i32 %value, 1
  %destination = getelementptr inbounds i32, ptr addrspace(1) %output, i32 %tid
  store i32 %incremented, ptr addrspace(1) %destination, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @add_one, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"input"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"output"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
