; ModuleID = 'nvmtl-air'
source_filename = "obj_main"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.Payload = type { <4 x float>, i32, [12 x i8] }

; Function Attrs: mustprogress nounwind willreturn
define void @obj_main(ptr addrspace(6) noundef writeonly align 16 captures(none) dereferenceable(32) %pl, ptr addrspace(3) %mgp.0, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(16) "air-buffer-no-alias" %ocol, ptr addrspace(2) noundef readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %cell, i32 noundef %tid) local_unnamed_addr #0 {
entry:
  %cmp = icmp eq i32 %tid, 0
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  %0 = load <4 x float>, ptr addrspace(2) %ocol, align 16, !tbaa !23, !alias.scope !27, !noalias !30
  %col = getelementptr inbounds %struct.Payload, ptr addrspace(6) %pl, i64 0, i32 0
  store <4 x float> %0, ptr addrspace(6) %col, align 16, !tbaa !32
  %1 = load i32, ptr addrspace(2) %cell, align 4, !tbaa !33, !alias.scope !30, !noalias !27
  %cell1 = getelementptr inbounds %struct.Payload, ptr addrspace(6) %pl, i64 0, i32 1
  store i32 %1, ptr addrspace(6) %cell1, align 16, !tbaa !35
  tail call void @air.set_threadgroups_per_grid_mesh_properties(ptr addrspace(3) captures(none) %mgp.0, <3 x i32> splat (i32 1)) #2
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.set_threadgroups_per_grid_mesh_properties(ptr addrspace(3) captures(none), <3 x i32>) local_unnamed_addr #1

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="96" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { nounwind willreturn memory(argmem: readwrite) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.object = !{!14}

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
!10 = !{!"Metal", i32 4, i32 1, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_enable"}
!14 = !{ptr @obj_main, !15, !16}
!15 = !{}
!16 = !{!17, !19, !20, !21, !22}
!17 = !{i32 0, !"air.payload", !"air.struct_type_info", !18, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Payload", !"air.arg_name", !"pl"}
!18 = !{i32 0, i32 16, i32 0, !"float4", !"col", i32 16, i32 4, i32 0, !"uint", !"cell"}
!19 = !{i32 1, !"air.mesh_grid_properties", !"air.arg_type_name", !"mesh_grid_properties", !"air.arg_name", !"mgp"}
!20 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"ocol"}
!21 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"cell"}
!22 = !{i32 4, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!23 = !{!24, !24, i64 0}
!24 = !{!"float", !25, i64 0}
!25 = !{!"omnipotent char", !26, i64 0}
!26 = !{!"Simple C++ TBAA"}
!27 = !{!28}
!28 = distinct !{!28, !29, !"air-alias-scope-arg(2)"}
!29 = distinct !{!29, !"air-alias-scopes(obj_main)"}
!30 = !{!31}
!31 = distinct !{!31, !29, !"air-alias-scope-arg(3)"}
!32 = !{!25, !25, i64 0}
!33 = !{!34, !34, i64 0}
!34 = !{!"int", !25, i64 0}
!35 = !{!36, !34, i64 16}
!36 = !{!"_ZTS7Payload", !24, i64 0, !34, i64 16}
