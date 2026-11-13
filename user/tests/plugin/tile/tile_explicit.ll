; ModuleID = 'nvmtl-air'
source_filename = "tile_explicit"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }

; Function Attrs: convergent mustprogress nounwind willreturn
define void @tile_explicit(%"struct.metal::_imageblock_base" %blk.coerce, <2 x i16> noundef %tid, <2 x i16> noundef %tsz, <2 x i32> noundef %gid, ptr addrspace(1) %out.coerce, ptr addrspace(1) noundef readnone captures(none) "air-buffer-no-alias" %sums) local_unnamed_addr #0 {
entry:
  %0 = tail call ptr addrspace(4) @air.imageblock_data(<2 x i16> %tid, i32 0, i16 0) #5
  %1 = extractelement <2 x i32> %gid, i64 0
  %2 = extractelement <2 x i32> %gid, i64 1
  %add = add i32 %1, %2
  %3 = tail call fast float @air.convert.f.f32.u.i32(i32 %add) #6
  %lum = bitcast ptr addrspace(4) %0 to ptr addrspace(4)
  store float %3, ptr addrspace(4) %lum, align 4, !tbaa !24
  %4 = extractelement <2 x i16> %tid, i64 0
  %conv = zext i16 %4 to i32
  %5 = extractelement <2 x i16> %tid, i64 1
  %conv3 = zext i16 %5 to i32
  %6 = extractelement <2 x i16> %tsz, i64 0
  %conv4 = zext i16 %6 to i32
  %mul = mul nuw nsw i32 %conv4, %conv3
  %add5 = add nuw nsw i32 %mul, %conv
  %idx = getelementptr inbounds i8, ptr addrspace(4) %0, i64 4
  %7 = bitcast ptr addrspace(4) %idx to ptr addrspace(4)
  store i32 %add5, ptr addrspace(4) %7, align 4, !tbaa !31
  tail call void @air.wg.barrier(i32 8, i32 5, i32 1) #7
  %add7 = add nuw nsw i32 %conv, 1
  %rem = urem i32 %add7, %conv4
  %conv9 = trunc i32 %rem to i16
  %vecinit10 = insertelement <2 x i16> %tid, i16 %conv9, i64 0
  %8 = tail call ptr addrspace(4) @air.imageblock_data(<2 x i16> %vecinit10, i32 0, i16 0) #5
  %lum12 = bitcast ptr addrspace(4) %8 to ptr addrspace(4)
  %9 = load float, ptr addrspace(4) %lum12, align 4, !tbaa !24
  %div = fmul fast float %9, 1.562500e-02
  %idx14 = getelementptr inbounds i8, ptr addrspace(4) %8, i64 4
  %10 = bitcast ptr addrspace(4) %idx14 to ptr addrspace(4)
  %11 = load i32, ptr addrspace(4) %10, align 4, !tbaa !31
  %12 = tail call fast float @air.convert.f.f32.u.i32(i32 %11) #6
  %div15 = fmul fast float %12, 3.906250e-03
  %13 = insertelement <4 x float> <float poison, float poison, float 0.000000e+00, float 1.000000e+00>, float %div, i64 0
  %vecinit18 = insertelement <4 x float> %13, float %div15, i64 1
  tail call void @air.write_texture_2d.v4f32(ptr addrspace(1) captures(none) %out.coerce, <2 x i32> %gid, <4 x float> %vecinit18, i32 0, i32 2) #8, !alias.scope !32
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1) captures(none), <2 x i32>, <4 x float>, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare ptr addrspace(4) @air.imageblock_data(<2 x i16>, i32, i16) local_unnamed_addr #3

; Function Attrs: convergent mustprogress nounwind willreturn
declare void @air.wg.barrier(i32, i32, i32) local_unnamed_addr #4

attributes #0 = { convergent mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #3 = { mustprogress nofree nounwind willreturn memory(read) }
attributes #4 = { convergent mustprogress nounwind willreturn }
attributes #5 = { nounwind willreturn memory(read) }
attributes #6 = { nounwind willreturn memory(none) }
attributes #7 = { convergent nounwind willreturn }
attributes #8 = { nounwind willreturn memory(argmem: readwrite) }

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
!14 = !{ptr @tile_explicit, !15, !16}
!15 = !{}
!16 = !{!17, !19, !20, !21, !22, !23}
!17 = !{i32 0, !"air.imageblock", !"explicit", !"air.imageblock_data_size", i32 12, !"air.struct_type_info", !18, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"imageblock<Cell, layout_explicit>", !"air.arg_name", !"blk"}
!18 = !{i32 0, i32 4, i32 0, !"float", !"lum", i32 4, i32 4, i32 0, !"uint", !"idx", i32 8, i32 4, i32 0, !"half2", !"pad"}
!19 = !{i32 1, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tid"}
!20 = !{i32 2, !"air.threads_per_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tsz"}
!21 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!22 = !{i32 4, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, write>", !"air.arg_name", !"out"}
!23 = !{i32 5, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"sums", !"air.arg_unused"}
!24 = !{!25, !26, i64 0}
!25 = !{!"_ZTS4Cell", !26, i64 0, !29, i64 4, !30, i64 8}
!26 = !{!"float", !27, i64 0}
!27 = !{!"omnipotent char", !28, i64 0}
!28 = !{!"Simple C++ TBAA"}
!29 = !{!"int", !27, i64 0}
!30 = !{!"half", !27, i64 0}
!31 = !{!25, !29, i64 4}
!32 = !{!33}
!33 = distinct !{!33, !34, !"air-alias-scope-textures"}
!34 = distinct !{!34, !"air-alias-scopes(tile_explicit)"}
