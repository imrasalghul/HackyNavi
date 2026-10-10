; Authored MIT negative fixture: one unconditional write and a second write
; guarded by an unsupplied Metal function constant. Treating the constant as zero
; would lose one effect while leaving a seemingly valid nonempty shader.
target triple = "air64-apple-macosx15.0.0"
@extra.MTL_FC_INIT_0_i = addrspace(2) externally_initialized constant i32 undef, section "air.fc_initializer"

define void @partial_write(ptr addrspace(1) %output) {
entry:
  store i32 7, ptr addrspace(1) %output, align 4
  %factor = load i32, ptr addrspace(2) @extra.MTL_FC_INIT_0_i
  %enabled = icmp ne i32 %factor, 0
  br i1 %enabled, label %additional, label %done
additional:
  %second = getelementptr i32, ptr addrspace(1) %output, i32 1
  store i32 8, ptr addrspace(1) %second, align 4
  br label %done
done:
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @partial_write, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"output"}
!air.function_constants = !{!4}
!4 = !{ptr addrspace(2) @extra.MTL_FC_INIT_0_i, !"int", !"extra", i32 0, i1 false}
