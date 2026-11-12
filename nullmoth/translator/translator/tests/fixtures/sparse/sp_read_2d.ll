; ModuleID = 'nvmtl-air'
source_filename = "sp_read_2d"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
define <4 x float> @sp_read_2d(<4 x float> %in.0, <2 x float> %in.1, <3 x float> %in.2, ptr addrspace(1) %t.coerce) local_unnamed_addr #0 {
entry:
  %0 = shufflevector <4 x float> %in.0, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %1 = tail call <2 x i32> @air.convert.u.v2i32.f.v2f32(<2 x float> %0) #4
  %2 = tail call ptr addrspace(2) @air.get_read_sampler() #5
  %3 = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %t.coerce, ptr addrspace(2) %2, <2 x i32> %1, <2 x i32> zeroinitializer, i32 1, i32 0) #6
  %4 = extractvalue { <4 x float>, i8 } %3, 1
  %5 = extractvalue { <4 x float>, i8 } %3, 0
  %6 = and i8 %4, 1
  %tobool.i.not.not = icmp eq i8 %6, 0
  %cond.i = select fast i1 %tobool.i.not.not, <4 x float> zeroinitializer, <4 x float> <float 1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00>
  %add = fadd fast <4 x float> %cond.i, %5
  ret <4 x float> %add
}

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nounwind willreturn memory(inaccessiblemem: read)
declare ptr addrspace(2) @air.get_read_sampler() local_unnamed_addr #2

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare <2 x i32> @air.convert.u.v2i32.f.v2f32(<2 x float>) local_unnamed_addr #3

attributes #0 = { mustprogress nofree nounwind willreturn memory(read) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { mustprogress nofree nounwind willreturn memory(inaccessiblemem: read) }
attributes #3 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #4 = { nounwind willreturn memory(none) }
attributes #5 = { nounwind willreturn memory(inaccessiblemem: read) }
attributes #6 = { nounwind willreturn memory(argmem: read) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.fragment = !{!14}

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
!14 = !{ptr @sp_read_2d, !15, !17}
!15 = !{!16}
!16 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!17 = !{!18, !19, !20, !21}
!18 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"p"}
!19 = !{i32 1, !"air.fragment_input", !"generated(2uvDv2_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv", !"air.arg_unused"}
!20 = !{i32 2, !"air.fragment_input", !"generated(1dDv3_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float3", !"air.arg_name", !"d", !"air.arg_unused"}
!21 = !{i32 3, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
