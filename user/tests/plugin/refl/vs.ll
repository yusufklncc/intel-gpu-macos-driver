; ModuleID = 'nvmtl-air'
source_filename = "vs"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::matrix" = type { [4 x <4 x float>] }

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define <{ <4 x float>, <2 x float> }> @vs(<2 x float> %in.0, <2 x float> %in.1, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(64) "air-buffer-no-alias" %mvp) local_unnamed_addr #0 {
entry:
  %arrayidx.i16 = getelementptr inbounds %"struct.metal::matrix", ptr addrspace(2) %mvp, i64 0, i32 0, i64 0
  %0 = load <4 x float>, ptr addrspace(2) %arrayidx.i16, align 16, !tbaa !22, !alias.scope !26
  %splat.splat.i = shufflevector <2 x float> %in.0, <2 x float> undef, <4 x i32> zeroinitializer
  %mul.i = fmul fast <4 x float> %0, %splat.splat.i
  %arrayidx.i = getelementptr inbounds %"struct.metal::matrix", ptr addrspace(2) %mvp, i64 0, i32 0, i64 1
  %1 = load <4 x float>, ptr addrspace(2) %arrayidx.i, align 16, !tbaa !22, !alias.scope !26
  %splat.splat.i12 = shufflevector <2 x float> %in.0, <2 x float> undef, <4 x i32> <i32 1, i32 1, i32 1, i32 1>
  %mul.i13 = fmul fast <4 x float> %1, %splat.splat.i12
  %add.i = fadd fast <4 x float> %mul.i13, %mul.i
  %arrayidx.i.i18 = getelementptr inbounds %"struct.metal::matrix", ptr addrspace(2) %mvp, i64 0, i32 0, i64 3
  %2 = load <4 x float>, ptr addrspace(2) %arrayidx.i.i18, align 16, !tbaa !22, !alias.scope !26
  %add.i19 = fadd fast <4 x float> %add.i, %2
  %3 = insertvalue <{ <4 x float>, <2 x float> }> undef, <4 x float> %add.i19, 0
  %4 = insertvalue <{ <4 x float>, <2 x float> }> %3, <2 x float> %in.1, 1
  ret <{ <4 x float>, <2 x float> }> %4
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.vertex = !{!14}

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
!14 = !{ptr @vs, !15, !18}
!15 = !{!16, !17}
!16 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!17 = !{!"air.vertex_output", !"generated(2uvDv2_f)", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!18 = !{!19, !20, !21}
!19 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"pos"}
!20 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!21 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4x4", !"air.arg_name", !"mvp"}
!22 = !{!23, !23, i64 0}
!23 = !{!"float", !24, i64 0}
!24 = !{!"omnipotent char", !25, i64 0}
!25 = !{!"Simple C++ TBAA"}
!26 = !{!27}
!27 = distinct !{!27, !28, !"air-alias-scope-arg(2)"}
!28 = distinct !{!28, !"air-alias-scopes(vs)"}
