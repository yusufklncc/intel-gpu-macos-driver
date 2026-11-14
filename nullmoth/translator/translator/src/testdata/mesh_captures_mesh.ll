; ModuleID = 'nvmtl-air'
source_filename = "mesh_main"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.Payload = type { <4 x float>, i32, [12 x i8] }

; Function Attrs: mustprogress nounwind willreturn
define void @mesh_main(ptr addrspace(7) %m.coerce, ptr addrspace(6) noundef readonly align 16 captures(none) dereferenceable(32) %pl, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(16) "air-buffer-no-alias" %scale, ptr addrspace(1) noundef readonly captures(none) "air-buffer-no-alias" %add, i32 noundef %tid) local_unnamed_addr #0 {
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
  %cell = getelementptr inbounds %struct.Payload, ptr addrspace(6) %pl, i64 0, i32 1
  %2 = load i32, ptr addrspace(6) %cell, align 16, !tbaa !27
  %rem = and i32 %2, 3
  %3 = tail call fast float @air.convert.f.f32.u.i32(i32 %rem) #4
  %vecinit2 = insertelement <2 x float> undef, float %3, i64 0
  %div44 = lshr i32 %2, 2
  %4 = tail call fast float @air.convert.f.f32.u.i32(i32 %div44) #4
  %vecinit4 = insertelement <2 x float> %vecinit2, float %4, i64 1
  %div5 = fmul fast <2 x float> %vecinit4, splat (float 2.500000e-01)
  %5 = fmul fast <2 x float> %vecinit1, splat (float 1.875000e-01)
  %div7 = fadd fast <2 x float> %5, splat (float 3.125000e-02)
  %add8 = fadd fast <2 x float> %div7, %div5
  %6 = extractelement <2 x float> %add8, i64 0
  %mul9 = fmul fast float %6, 2.000000e+00
  %sub = fadd fast float %mul9, -1.000000e+00
  %7 = extractelement <2 x float> %add8, i64 1
  %mul11 = fmul fast float %7, 2.000000e+00
  %sub12 = fsub fast float 1.000000e+00, %mul11
  %8 = insertelement <4 x float> <float poison, float poison, float 0.000000e+00, float 1.000000e+00>, float %sub, i64 0
  %vecinit15 = insertelement <4 x float> %8, float %sub12, i64 1
  %col = getelementptr inbounds %struct.Payload, ptr addrspace(6) %pl, i64 0, i32 0
  %9 = load <4 x float>, ptr addrspace(6) %col, align 16, !tbaa !33
  %10 = load <4 x float>, ptr addrspace(2) %scale, align 16, !tbaa !34, !alias.scope !35, !noalias !38
  %mul16 = fmul fast <4 x float> %10, %9
  %11 = load <4 x float>, ptr addrspace(1) %add, align 16, !tbaa !34, !alias.scope !38, !noalias !35
  %add17 = fadd fast <4 x float> %mul16, %11
  tail call void @air.set_position_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 %tid, <4 x float> %vecinit15) #5
  tail call void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) captures(none) %m.coerce, i32 0, i32 %tid, <4 x float> %add17) #5
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  %cmp19 = icmp ult i32 %tid, 6
  br i1 %cmp19, label %if.then20, label %if.end22

if.then20:                                        ; preds = %if.end
  %12 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 0
  call void @llvm.lifetime.start.p0(i64 6, ptr nonnull %12) #6
  store i8 0, ptr %12, align 1
  %13 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 1
  store i8 1, ptr %13, align 1
  %14 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 2
  store i8 2, ptr %14, align 1
  %15 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 3
  store i8 1, ptr %15, align 1
  %16 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 4
  store i8 3, ptr %16, align 1
  %17 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 5
  store i8 2, ptr %17, align 1
  %idxprom = zext i32 %tid to i64
  %arrayidx21 = getelementptr inbounds [6 x i8], ptr %idx, i64 0, i64 %idxprom
  %18 = load i8, ptr %arrayidx21, align 1, !tbaa !33
  tail call void @air.set_index_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 %tid, i8 %18) #5
  call void @llvm.lifetime.end.p0(i64 6, ptr nonnull %12) #6
  br label %if.end22

if.end22:                                         ; preds = %if.then20, %if.end
  %cmp23 = icmp eq i32 %tid, 0
  br i1 %cmp23, label %if.then24, label %if.end25

if.then24:                                        ; preds = %if.end22
  tail call void @air.set_primitive_count_mesh(ptr addrspace(7) captures(none) %m.coerce, i32 2) #5
  br label %if.end25

if.end25:                                         ; preds = %if.then24, %if.end22
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
!14 = !{ptr @mesh_main, !15, !16}
!15 = !{}
!16 = !{!17, !22, !24, !25, !26}
!17 = !{i32 0, !"air.mesh", !18, !"air.arg_type_name", !"mesh<MV, void, 4, 2, triangle>", !"air.arg_name", !"m"}
!18 = !{!"air.mesh_type_info", !19, !15, i32 4, i32 2, !"air.triangle"}
!19 = !{!20, !21}
!20 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!21 = !{!"air.mesh_vertex_data", i32 0, !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!22 = !{i32 1, !"air.payload", !"air.struct_type_info", !23, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Payload", !"air.arg_name", !"pl"}
!23 = !{i32 0, i32 16, i32 0, !"float4", !"col", i32 16, i32 4, i32 0, !"uint", !"cell"}
!24 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"scale"}
!25 = !{i32 3, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"add"}
!26 = !{i32 4, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!27 = !{!28, !32, i64 16}
!28 = !{!"_ZTS7Payload", !29, i64 0, !32, i64 16}
!29 = !{!"float", !30, i64 0}
!30 = !{!"omnipotent char", !31, i64 0}
!31 = !{!"Simple C++ TBAA"}
!32 = !{!"int", !30, i64 0}
!33 = !{!30, !30, i64 0}
!34 = !{!29, !29, i64 0}
!35 = !{!36}
!36 = distinct !{!36, !37, !"air-alias-scope-arg(2)"}
!37 = distinct !{!37, !"air-alias-scopes(mesh_main)"}
!38 = !{!39}
!39 = distinct !{!39, !37, !"air-alias-scope-arg(3)"}
