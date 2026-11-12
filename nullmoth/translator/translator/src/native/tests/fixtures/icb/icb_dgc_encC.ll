; ModuleID = 'nvmtl-air'
; encC of user/tests/metal/mtl_gpu_icb2.m: a compute ICB encoded from a kernel: Metal's own AIR (M1 Pro compiler, nvmtl-air-dis; tbaa and alias-scope metadata removed)
source_filename = "encC"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.CArgs = type { %"struct.metal::command_buffer", %"struct.metal::compute_pipeline_state", %"struct.metal::compute_pipeline_state" }
%"struct.metal::command_buffer" = type { ptr addrspace(1) }
%"struct.metal::compute_pipeline_state" = type { ptr addrspace(1) }
%struct.CPtrs = type { ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(2) }

; Function Attrs: mustprogress nounwind willreturn
define void @encC(ptr addrspace(1) noundef readonly captures(none) "air-buffer-no-alias" %a, ptr addrspace(1) noundef readonly captures(none) "air-buffer-no-alias" %p, i32 noundef %t) local_unnamed_addr #0 {
entry:
  %cmp.not = icmp eq i32 %t, 0
  br i1 %cmp.not, label %if.end, label %return

if.end:                                           ; preds = %entry
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.CArgs, ptr addrspace(1) %a, i64 0, i32 0, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %out = getelementptr inbounds %struct.CPtrs, ptr addrspace(1) %p, i64 0, i32 0
  %0 = bitcast ptr addrspace(1) %p to ptr addrspace(1)
  %1 = load ptr addrspace(1), ptr addrspace(1) %0, align 8
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 0, ptr addrspace(1) %1, i64 -1, i32 0) #2
  %tag1 = getelementptr inbounds %struct.CPtrs, ptr addrspace(1) %p, i64 0, i32 1
  %2 = bitcast ptr addrspace(1) %tag1 to ptr addrspace(1)
  %3 = load ptr addrspace(1), ptr addrspace(1) %2, align 8
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 0, ptr addrspace(1) %3, i64 -1, i32 1) #2
  tail call void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1) captures(none) %agg.tmp.sroa.0.0.copyload, i32 0, <3 x i32> <i32 2, i32 1, i32 1>, <3 x i32> <i32 32, i32 1, i32 1>) #2
  %agg.tmp1.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %agg.tmp3.sroa.0.0..sroa_idx = getelementptr inbounds %struct.CArgs, ptr addrspace(1) %a, i64 0, i32 1, i32 0
  %agg.tmp3.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp3.sroa.0.0..sroa_idx, align 8
  tail call void @air.set_pipeline_state_compute_command(ptr addrspace(1) captures(none) %agg.tmp1.sroa.0.0.copyload, i32 1, ptr addrspace(1) readonly %agg.tmp3.sroa.0.0.copyload) #2
  %4 = load ptr addrspace(1), ptr addrspace(1) %out, align 8
  %add.ptr = getelementptr inbounds i32, ptr addrspace(1) %4, i64 64
  %5 = bitcast ptr addrspace(1) %add.ptr to ptr addrspace(1)
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp1.sroa.0.0.copyload, i32 1, ptr addrspace(1) %5, i64 -1, i32 0) #2
  %tag2 = getelementptr inbounds %struct.CPtrs, ptr addrspace(1) %p, i64 0, i32 2
  %6 = bitcast ptr addrspace(1) %tag2 to ptr addrspace(1)
  %7 = load ptr addrspace(1), ptr addrspace(1) %6, align 8
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp1.sroa.0.0.copyload, i32 1, ptr addrspace(1) %7, i64 -1, i32 1) #2
  tail call void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1) captures(none) %agg.tmp1.sroa.0.0.copyload, i32 1, <3 x i32> <i32 2, i32 1, i32 1>, <3 x i32> <i32 32, i32 1, i32 1>) #2
  %agg.tmp5.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %agg.tmp7.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp3.sroa.0.0..sroa_idx, align 8
  tail call void @air.set_pipeline_state_compute_command(ptr addrspace(1) captures(none) %agg.tmp5.sroa.0.0.copyload, i32 2, ptr addrspace(1) readonly %agg.tmp7.sroa.0.0.copyload) #2
  %8 = load ptr addrspace(1), ptr addrspace(1) %out, align 8
  %add.ptr10 = getelementptr inbounds i32, ptr addrspace(1) %8, i64 128
  %9 = bitcast ptr addrspace(1) %add.ptr10 to ptr addrspace(1)
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp5.sroa.0.0.copyload, i32 2, ptr addrspace(1) %9, i64 -1, i32 0) #2
  %tag3 = getelementptr inbounds %struct.CPtrs, ptr addrspace(1) %p, i64 0, i32 3
  %10 = bitcast ptr addrspace(1) %tag3 to ptr addrspace(1)
  %11 = load ptr addrspace(2), ptr addrspace(1) %10, align 8
  tail call void @air.set_kernel_buffer_compute_command.p2i8(ptr addrspace(1) captures(none) %agg.tmp5.sroa.0.0.copyload, i32 2, ptr addrspace(2) %11, i64 -1, i32 1) #2
  tail call void @air.set_barrier_compute_command(ptr addrspace(1) captures(none) %agg.tmp5.sroa.0.0.copyload, i32 2) #2
  tail call void @air.concurrent_dispatch_threads_compute_command(ptr addrspace(1) captures(none) %agg.tmp5.sroa.0.0.copyload, i32 2, <3 x i32> <i32 64, i32 1, i32 1>, <3 x i32> <i32 32, i32 1, i32 1>) #2
  %agg.tmp11.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %agg.tmp13.sroa.0.0..sroa_idx = getelementptr inbounds %struct.CArgs, ptr addrspace(1) %a, i64 0, i32 2, i32 0
  %agg.tmp13.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(1) %agg.tmp13.sroa.0.0..sroa_idx, align 8
  tail call void @air.set_pipeline_state_compute_command(ptr addrspace(1) captures(none) %agg.tmp11.sroa.0.0.copyload, i32 3, ptr addrspace(1) readonly %agg.tmp13.sroa.0.0.copyload) #2
  %12 = load ptr addrspace(1), ptr addrspace(1) %out, align 8
  %add.ptr15 = getelementptr inbounds i32, ptr addrspace(1) %12, i64 192
  %13 = bitcast ptr addrspace(1) %add.ptr15 to ptr addrspace(1)
  tail call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none) %agg.tmp11.sroa.0.0.copyload, i32 3, ptr addrspace(1) %13, i64 -1, i32 0) #2
  tail call void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1) captures(none) %agg.tmp11.sroa.0.0.copyload, i32 3, <3 x i32> <i32 2, i32 1, i32 1>, <3 x i32> <i32 32, i32 1, i32 1>) #2
  br label %return

return:                                           ; preds = %if.end, %entry
  ret void
}

; Function Attrs: mustprogress nounwind willreturn
declare void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1) captures(none), i32, <3 x i32>, <3 x i32>) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) captures(none), i32, ptr addrspace(1), i64, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_pipeline_state_compute_command(ptr addrspace(1) captures(none), i32, ptr addrspace(1) readonly) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.concurrent_dispatch_threads_compute_command(ptr addrspace(1) captures(none), i32, <3 x i32>, <3 x i32>) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_barrier_compute_command(ptr addrspace(1) captures(none), i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn
declare void @air.set_kernel_buffer_compute_command.p2i8(ptr addrspace(1) captures(none), i32, ptr addrspace(2), i64, i32) local_unnamed_addr #1

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="96" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
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
!10 = !{!"Metal", i32 4, i32 0, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_enable"}
!14 = !{ptr @encC, !15, !16}
!15 = !{}
!16 = !{!17, !22, !28}
!17 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !18, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"CArgs", !"air.arg_name", !"a"}
!18 = !{i32 0, i32 8, i32 0, !"command_buffer", !"cb", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"compute_pipeline_state", !"w", !"air.indirect_argument", !20, i32 16, i32 8, i32 0, !"compute_pipeline_state", !"w2", !"air.indirect_argument", !21}
!19 = !{i32 0, !"air.command_buffer", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"command_buffer", !"air.arg_name", !"cb"}
!20 = !{i32 1, !"air.compute_pipeline_state", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"compute_pipeline_state", !"air.arg_name", !"w"}
!21 = !{i32 2, !"air.compute_pipeline_state", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"compute_pipeline_state", !"air.arg_name", !"w2"}
!22 = !{i32 1, !"air.indirect_buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !23, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"CPtrs", !"air.arg_name", !"p"}
!23 = !{i32 0, i32 8, i32 0, !"uint", !"out", !"air.indirect_argument", !24, i32 8, i32 8, i32 0, !"uint", !"tag1", !"air.indirect_argument", !25, i32 16, i32 8, i32 0, !"uint", !"tag2", !"air.indirect_argument", !26, i32 24, i32 8, i32 0, !"uint", !"tag3", !"air.indirect_argument", !27}
!24 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!25 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tag1"}
!26 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tag2"}
!27 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tag3"}
!28 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"t"}
