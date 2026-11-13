; ModuleID = 'nvmtl-air'
source_filename = "apply"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

; Function Attrs: convergent mustprogress nounwind
define void @apply(ptr addrspace(1) noundef captures(none) "air-buffer-no-alias" %o, ptr addrspace(1) readonly captures(none) %t.coerce, ptr addrspace(2) noundef readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %n, i32 noundef %i) local_unnamed_addr #0 {
entry:
  %0 = load i32, ptr addrspace(2) %n, align 4, !tbaa !21, !alias.scope !25, !noalias !28
  %rem = urem i32 %i, %0
  %1 = tail call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) readonly captures(none) %t.coerce, i32 %rem) #2, !alias.scope !31, !noalias !32
  %2 = bitcast ptr %1 to ptr
  %idxprom = zext i32 %i to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(1) %o, i64 %idxprom
  %3 = load float, ptr addrspace(1) %arrayidx, align 4, !tbaa !33, !alias.scope !35, !noalias !36
  %call1 = tail call fast float %2(float noundef %3) #3
  store float %call1, ptr addrspace(1) %arrayidx, align 4, !tbaa !33, !alias.scope !35, !noalias !36
  ret void
}

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) readonly captures(none), i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nounwind "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { nounwind willreturn memory(argmem: read) }
attributes #3 = { convergent nobuiltin nounwind "no-builtins" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.kernel = !{!14}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"frame-pointer", i32 2}
!2 = !{i32 7, !"air.max_device_buffers", i32 31}
!3 = !{i32 7, !"air.max_constant_buffers", i32 31}
!4 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!5 = !{i32 7, !"air.max_textures", i32 128}
!6 = !{i32 7, !"air.max_read_write_textures", i32 8}
!7 = !{i32 7, !"air.max_samplers", i32 16}
!8 = !{!"Apple metal version 32023.921 (metalfe-32023.921.5)"}
!9 = !{i32 2, i32 9, i32 0}
!10 = !{!"Metal", i32 4, i32 0, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_enable"}
!14 = !{ptr @apply, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"o"}
!18 = !{i32 1, !"air.visible_function_table", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"visible_function_table", !"air.arg_name", !"t"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"n"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!21 = !{!22, !22, i64 0}
!22 = !{!"int", !23, i64 0}
!23 = !{!"omnipotent char", !24, i64 0}
!24 = !{!"Simple C++ TBAA"}
!25 = !{!26}
!26 = distinct !{!26, !27, !"air-alias-scope-arg(2)"}
!27 = distinct !{!27, !"air-alias-scopes(apply)"}
!28 = !{!29, !30}
!29 = distinct !{!29, !27, !"air-alias-scope-arg(0)"}
!30 = distinct !{!30, !27, !"air-alias-scope-arg(1)"}
!31 = !{!30}
!32 = !{!29, !26}
!33 = !{!34, !34, i64 0}
!34 = !{!"float", !23, i64 0}
!35 = !{!29}
!36 = !{!30, !26}
