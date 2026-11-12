; ModuleID = 'nvmtl-air'
; k_all: AIR as the Metal plugin hands it to the translator (tbaa and alias-scope metadata removed); every render_command
; family a shader can encode (reset, pipeline, vertex and fragment buffers, draws, barriers), for NVMTL_ICB_DGC=1
source_filename = "k_all"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.C = type { %"struct.metal::command_buffer" }
%"struct.metal::command_buffer" = type { ptr addrspace(1) }
%struct.P = type { %"struct.metal::render_pipeline_state" }
%"struct.metal::render_pipeline_state" = type { ptr addrspace(1) }

; Function Attrs: mustprogress nounwind willreturn
define void @k_all(ptr addrspace(1) noundef readonly captures(none) "air-buffer-no-alias" %c, ptr addrspace(1) noundef "air-buffer-no-alias" %ib, ptr addrspace(1) noundef "air-buffer-no-alias" %sb, ptr addrspace(1) noundef "air-buffer-no-alias" %vb, ptr addrspace(2) noundef readonly align 8 captures(none) dereferenceable(8) "air-buffer-no-alias" %p, i32 noundef %t) local_unnamed_addr #0 {
entry:
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.C, ptr addrspace(1) %c, i64 0, i32 0, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  tail call void @air.reset_render_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t) #2
  %agg.tmp1.sroa.0.0..sroa_idx = getelementptr inbounds %struct.P, ptr addrspace(2) %p, i64 0, i32 0, i32 0
  %agg.tmp1.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(2) %agg.tmp1.sroa.0.0..sroa_idx, align 8
  tail call void @air.set_pipeline_state_render_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, ptr addrspace(1) readonly %agg.tmp1.sroa.0.0.copyload) #2
  %0 = bitcast ptr addrspace(1) %vb to ptr addrspace(1)
  tail call void @air.set_vertex_buffer_render_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, ptr addrspace(1) %0, i64 -1, i32 3) #2
  %add.ptr = getelementptr inbounds float, ptr addrspace(1) %vb, i64 4
  %1 = bitcast ptr addrspace(1) %add.ptr to ptr addrspace(1)
  tail call void @air.set_fragment_buffer_render_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, ptr addrspace(1) %1, i32 5) #2
  tail call void @air.draw_primitives_render_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, i32 4, i32 7, i32 8, i32 9, i32 10) #2
  %add.ptr2 = getelementptr inbounds i32, ptr addrspace(1) %ib, i64 2
  tail call void @air.draw_indexed_primitives_render_command.p1i32(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, i32 1, i32 11, ptr addrspace(1) %add.ptr2, i32 12, i32 13, i32 14) #2
  %add.ptr3 = getelementptr inbounds i16, ptr addrspace(1) %sb, i64 3
  tail call void @air.draw_indexed_primitives_render_command.p1i16(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t, i32 0, i32 15, ptr addrspace(1) %add.ptr3, i32 16, i32 17, i32 18) #2
  tail call void @air.set_barrier_render_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t) #2
  tail call void @air.clear_barrier_render_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 %t) #2
  ret void
}

; Function Attrs: mustprogress nounwind willreturn
declare void @air.clear_barrier_render_command(ptr addrspace(1) captures(none), i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_barrier_render_command(ptr addrspace(1) captures(none), i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.draw_indexed_primitives_render_command.p1i16(ptr addrspace(1) captures(none), i32, i32, i32, ptr addrspace(1), i32, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.draw_indexed_primitives_render_command.p1i32(ptr addrspace(1) captures(none), i32, i32, i32, ptr addrspace(1), i32, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.draw_primitives_render_command(ptr addrspace(1) captures(none), i32, i32, i32, i32, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_fragment_buffer_render_command.p1i8(ptr addrspace(1) captures(none), i32, ptr addrspace(1), i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_vertex_buffer_render_command.p1i8(ptr addrspace(1) captures(none), i32, ptr addrspace(1), i64, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_pipeline_state_render_command(ptr addrspace(1) captures(none), i32, ptr addrspace(1) readonly) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.reset_render_command(ptr addrspace(1) captures(none), i32) local_unnamed_addr #1

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn }
attributes #2 = { nounwind willreturn }

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
!10 = !{!"Metal", i32 4, i32 1, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_enable"}
!14 = !{ptr @k_all, !15, !16}
!15 = !{}
!16 = !{!17, !20, !21, !22, !23, !26}
!17 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !18, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"C", !"air.arg_name", !"c"}
!18 = !{i32 0, i32 8, i32 0, !"command_buffer", !"cb", !"air.indirect_argument", !19}
!19 = !{i32 0, !"air.command_buffer", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"command_buffer", !"air.arg_name", !"cb"}
!20 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"ib"}
!21 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 2, !"air.arg_type_align_size", i32 2, !"air.arg_type_name", !"ushort", !"air.arg_name", !"sb"}
!22 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"vb"}
!23 = !{i32 4, !"air.indirect_buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 4, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !24, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"P", !"air.arg_name", !"p"}
!24 = !{i32 0, i32 8, i32 0, !"render_pipeline_state", !"ps", !"air.indirect_argument", !25}
!25 = !{i32 0, !"air.render_pipeline_state", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"render_pipeline_state", !"air.arg_name", !"ps"}
!26 = !{i32 5, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
