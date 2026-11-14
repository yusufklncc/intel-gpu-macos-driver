; ModuleID = 'nvmtl-air'
source_filename = "mesh_grid"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

; Function Attrs: mustprogress nounwind willreturn
define void @mesh_grid(ptr addrspace(7) %m.coerce, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(16) "air-buffer-no-alias" %base, <3 x i32> noundef %mtg, i32 noundef %tid) local_unnamed_addr #0 {
entry:
  %idx = alloca [6 x i8], align 1
  %cmp = icmp ult i32 %tid, 4
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  %and = and i32 %tid, 1
  %0 = tail call fast float @air.convert.f.f32.u.i32(i32 %and) #4
  %vecinit = insertelement <2 x float> undef, float %0, i64 0
  %shr = lshr i32 %tid, 1
  %1 = tail call fast float @air.convert.f.f32.u.i32(i32 %shr) #4
  %vecinit1 = insertelement <2 x float> %vecinit, float %1, i64 1
  %2 = shufflevector <3 x i32> %mtg, <3 x i32> poison, <2 x i32> <i32 0, i32 1>
  %3 = tail call fast <2 x float> @air.convert.f.v2f32.u.v2i32(<2 x i32> %2) #4
  %add = fadd fast <2 x float> %3, splat (float 1.250000e-01)
  %mul = fmul fast <2 x float> %vecinit1, splat (float 7.500000e-01)
  %add2 = fadd fast <2 x float> %add, %mul
  %div = fmul fast <2 x float> %add2, splat (float 2.500000e-01)
  %4 = extractelement <2 x float> %div, i64 0
  %mul3 = fmul fast float %4, 2.000000e+00
  %sub = fadd fast float %mul3, -1.000000e+00
  %5 = extractelement <2 x float> %div, i64 1
  %mul5 = fmul fast float %5, 2.000000e+00
  %sub6 = fsub fast float 1.000000e+00, %mul5
  %6 = insertelement <4 x float> <float poison, float poison, float 0.000000e+00, float 1.000000e+00>, float %sub, i64 0
  %vecinit9 = insertelement <4 x float> %6, float %sub6, i64 1
  %7 = load <4 x float>, ptr addrspace(2) %base, align 16, !tbaa !25, !alias.scope !29
  %8 = extractelement <3 x i32> %mtg, i64 0
  %and10 = and i32 %8, 1
  %9 = tail call fast float @air.convert.f.f32.u.i32(i32 %and10) #4
  %10 = extractelement <3 x i32> %mtg, i64 1
  %and12 = and i32 %10, 1
  %11 = tail call fast float @air.convert.f.f32.u.i32(i32 %and12) #4
  %shr14 = lshr i32 %8, 1
  %and15 = and i32 %shr14, 1
  %12 = tail call fast float @air.convert.f.f32.u.i32(i32 %and15) #4
  %13 = insertelement <4 x float> <float poison, float poison, float poison, float 0.000000e+00>, float %9, i64 0
  %14 = insertelement <4 x float> %13, float %11, i64 1
  %vecinit17 = insertelement <4 x float> %14, float %12, i64 2
  %add18 = fadd fast <4 x float> %vecinit17, %7
  tail call void @air.set_position_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 %tid, <4 x float> %vecinit9) #5
  tail call void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) captures(none) %m.coerce, i32 0, i32 %tid, <4 x float> %add18) #5
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  %cmp19 = icmp ult i32 %tid, 6
  br i1 %cmp19, label %if.then20, label %if.end21

if.then20:                                        ; preds = %if.end
  %15 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 0
  call void @llvm.lifetime.start.p0(i64 6, ptr nonnull %15) #6
  store i8 0, ptr %15, align 1
  %16 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 1
  store i8 1, ptr %16, align 1
  %17 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 2
  store i8 2, ptr %17, align 1
  %18 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 3
  store i8 1, ptr %18, align 1
  %19 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 4
  store i8 3, ptr %19, align 1
  %20 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 5
  store i8 2, ptr %20, align 1
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 %idxprom
  %21 = load i8, ptr %arrayidx, align 1, !tbaa !32
  tail call void @air.set_index_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 %tid, i8 %21) #5
  call void @llvm.lifetime.end.p0(i64 6, ptr nonnull %15) #6
  br label %if.end21

if.end21:                                         ; preds = %if.then20, %if.end
  %cmp22 = icmp eq i32 %tid, 0
  br i1 %cmp22, label %if.then23, label %if.end24

if.then23:                                        ; preds = %if.end21
  tail call void @air.set_primitive_count_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 2) #5
  br label %if.end24

if.end24:                                         ; preds = %if.then23, %if.end21
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.set_primitive_count_mesh(ptr addrspace(7) captures(none), i32) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.set_index_mesh(ptr addrspace(7) captures(none), i32, i8) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) captures(none), i32, i32, <4 x float>) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.set_position_mesh(ptr addrspace(7) captures(none), i32, <4 x float>) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare <2 x float> @air.convert.f.v2f32.u.v2i32(<2 x i32>) local_unnamed_addr #2

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr captures(none)) #3

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr captures(none)) #3

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #3 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #4 = { nounwind willreturn memory(none) }
attributes #5 = { nounwind willreturn memory(argmem: readwrite) }
attributes #6 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.mesh = !{!14}

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
!14 = !{ptr @mesh_grid, !15, !16}
!15 = !{}
!16 = !{!17, !22, !23, !24}
!17 = !{i32 0, !"air.mesh", !18, !"air.arg_type_name", !"mesh<MV, void, 4, 2, triangle>", !"air.arg_name", !"m"}
!18 = !{!"air.mesh_type_info", !19, !15, i32 4, i32 2, !"air.triangle"}
!19 = !{!20, !21}
!20 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!21 = !{!"air.mesh_vertex_data", i32 0, !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!22 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"base"}
!23 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"mtg"}
!24 = !{i32 3, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!25 = !{!26, !26, i64 0}
!26 = !{!"float", !27, i64 0}
!27 = !{!"omnipotent char", !28, i64 0}
!28 = !{!"Simple C++ TBAA"}
!29 = !{!30}
!30 = distinct !{!30, !31, !"air-alias-scope-arg(1)"}
!31 = distinct !{!31, !"air-alias-scopes(mesh_grid)"}
!32 = !{!27, !27, i64 0}
