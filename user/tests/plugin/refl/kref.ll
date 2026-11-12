; ModuleID = 'nvmtl-air'
source_filename = "kref"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%struct.Params = type { <4 x float>, i32, [3 x float] }

; Function Attrs: convergent mustprogress nounwind willreturn
define void @kref(ptr addrspace(1) noundef writeonly captures(none) "air-buffer-no-alias" %out, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(32) "air-buffer-no-alias" %p, ptr addrspace(1) noundef readnone captures(none) "air-buffer-no-alias" %unused, ptr addrspace(1) %src.coerce, ptr addrspace(1) %dst.coerce, ptr addrspace(1) %dep.coerce, ptr addrspace(2) readonly captures(none) %s.coerce, ptr addrspace(3) noundef captures(none) "air-buffer-no-alias" %tg, ptr addrspace(3) noundef captures(none) "air-buffer-no-alias" %tg2, i32 noundef %i, i32 noundef %l) local_unnamed_addr #0 {
entry:
  %vecinit4 = insertelement <2 x i32> <i32 poison, i32 0>, i32 %i, i64 0
  %0 = tail call ptr addrspace(2) @air.get_read_sampler() #7
  %1 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %src.coerce, ptr addrspace(2) %0, <2 x i32> %vecinit4, <2 x i32> zeroinitializer, i32 0, i32 1) #8
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %scale = getelementptr inbounds %struct.Params, ptr addrspace(2) %p, i64 0, i32 0
  %3 = load <4 x float>, ptr addrspace(2) %scale, align 16, !tbaa !29, !alias.scope !32, !noalias !35
  %mul = fmul fast <4 x float> %2, %3
  %4 = tail call { float, i8 } @air.sample_depth_2d.f32(ptr addrspace(1) readonly captures(none) %dep.coerce, ptr addrspace(2) readonly captures(none) %s.coerce, i32 1, <2 x float> splat (float 5.000000e-01), i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #9, !alias.scope !41, !noalias !42
  %5 = extractvalue { float, i8 } %4, 0
  %splat.splatinsert = insertelement <4 x float> poison, float %5, i64 0
  %splat.splat = shufflevector <4 x float> %splat.splatinsert, <4 x float> poison, <4 x i32> zeroinitializer
  %add = fadd fast <4 x float> %splat.splat, %mul
  %idxprom = zext i32 %l to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(3) %tg, i64 %idxprom
  store <4 x float> %add, ptr addrspace(3) %arrayidx, align 16, !tbaa !43, !alias.scope !45, !noalias !46
  %arrayidx7 = getelementptr inbounds i32, ptr addrspace(3) %tg2, i64 %idxprom
  store i32 %i, ptr addrspace(3) %arrayidx7, align 4, !tbaa !47, !alias.scope !49, !noalias !50
  tail call void @air.wg.barrier(i32 2, i32 5, i32 1) #10
  %add8 = add i32 %l, 1
  %count = getelementptr inbounds %struct.Params, ptr addrspace(2) %p, i64 0, i32 1
  %6 = load i32, ptr addrspace(2) %count, align 16, !tbaa !51, !alias.scope !32, !noalias !35
  %rem = urem i32 %add8, %6
  %idxprom9 = zext i32 %rem to i64
  %arrayidx10 = getelementptr inbounds <4 x float>, ptr addrspace(3) %tg, i64 %idxprom9
  %7 = load <4 x float>, ptr addrspace(3) %arrayidx10, align 16, !tbaa !43, !alias.scope !45, !noalias !46
  %8 = load i32, ptr addrspace(3) %tg2, align 4, !tbaa !47, !alias.scope !49, !noalias !50
  %9 = tail call fast float @air.convert.f.f32.u.i32(i32 %8) #11
  %splat.splatinsert12 = insertelement <4 x float> poison, float %9, i64 0
  %splat.splat13 = shufflevector <4 x float> %splat.splatinsert12, <4 x float> poison, <4 x i32> zeroinitializer
  %add14 = fadd fast <4 x float> %splat.splat13, %7
  %idxprom15 = zext i32 %i to i64
  %arrayidx16 = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom15
  store <4 x float> %add14, ptr addrspace(1) %arrayidx16, align 16, !tbaa !43, !alias.scope !53, !noalias !54
  %splat.splatinsert17 = insertelement <4 x i32> poison, i32 %i, i64 0
  %splat.splat18 = shufflevector <4 x i32> %splat.splatinsert17, <4 x i32> poison, <4 x i32> zeroinitializer
  tail call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) captures(none) %dst.coerce, <2 x i32> %vecinit4, <4 x i32> %splat.splat18, i32 0, i32 2) #12, !alias.scope !55, !noalias !56
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.write_texture_2d.u.v4i32(ptr addrspace(1) captures(none), <2 x i32>, <4 x i32>, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr #2

; Function Attrs: convergent mustprogress nounwind willreturn
declare void @air.wg.barrier(i32, i32, i32) local_unnamed_addr #3

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { float, i8 } @air.sample_depth_2d.f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), i32, <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #4

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32) local_unnamed_addr #5

; Function Attrs: mustprogress nofree nounwind willreturn memory(inaccessiblemem: read)
declare ptr addrspace(2) @air.get_read_sampler() local_unnamed_addr #6

attributes #0 = { convergent mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #3 = { convergent mustprogress nounwind willreturn }
attributes #4 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #5 = { mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #6 = { mustprogress nofree nounwind willreturn memory(inaccessiblemem: read) }
attributes #7 = { nounwind willreturn memory(inaccessiblemem: read) }
attributes #8 = { nounwind willreturn memory(argmem: read) }
attributes #9 = { convergent nounwind willreturn memory(argmem: read) }
attributes #10 = { convergent nounwind willreturn }
attributes #11 = { nounwind willreturn memory(none) }
attributes #12 = { nounwind willreturn memory(argmem: readwrite) }

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
!14 = !{ptr @kref, !15, !16}
!15 = !{}
!16 = !{!17, !18, !20, !21, !22, !23, !24, !25, !26, !27, !28}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!18 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !19, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Params", !"air.arg_name", !"p"}
!19 = !{i32 0, i32 16, i32 0, !"float4", !"scale", i32 16, i32 4, i32 0, !"uint", !"count", i32 20, i32 4, i32 3, !"float", !"pad"}
!20 = !{i32 2, !"air.buffer", !"air.location_index", i32 5, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"unused", !"air.arg_unused"}
!21 = !{i32 3, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"src"}
!22 = !{i32 4, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<uint, write>", !"air.arg_name", !"dst"}
!23 = !{i32 5, !"air.texture", !"air.location_index", i32 4, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"dep"}
!24 = !{i32 6, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!25 = !{i32 7, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"tg"}
!26 = !{i32 8, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"tg2"}
!27 = !{i32 9, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!28 = !{i32 10, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"l"}
!29 = !{!30, !30, i64 0}
!30 = !{!"omnipotent char", !31, i64 0}
!31 = !{!"Simple C++ TBAA"}
!32 = !{!33}
!33 = distinct !{!33, !34, !"air-alias-scope-arg(1)"}
!34 = distinct !{!34, !"air-alias-scopes(kref)"}
!35 = !{!36, !37, !38, !39, !40}
!36 = distinct !{!36, !34, !"air-alias-scope-arg(0)"}
!37 = distinct !{!37, !34, !"air-alias-scope-textures"}
!38 = distinct !{!38, !34, !"air-alias-scope-samplers"}
!39 = distinct !{!39, !34, !"air-alias-scope-arg(7)"}
!40 = distinct !{!40, !34, !"air-alias-scope-arg(8)"}
!41 = !{!37, !38}
!42 = !{!36, !33, !39, !40}
!43 = !{!44, !44, i64 0}
!44 = !{!"float", !30, i64 0}
!45 = !{!39}
!46 = !{!36, !33, !37, !38, !40}
!47 = !{!48, !48, i64 0}
!48 = !{!"int", !30, i64 0}
!49 = !{!40}
!50 = !{!36, !33, !37, !38, !39}
!51 = !{!52, !48, i64 16}
!52 = !{!"_ZTS6Params", !44, i64 0, !48, i64 16, !30, i64 20}
!53 = !{!36}
!54 = !{!33, !37, !38, !39, !40}
!55 = !{!37}
!56 = !{!36, !33, !38, !39, !40}
