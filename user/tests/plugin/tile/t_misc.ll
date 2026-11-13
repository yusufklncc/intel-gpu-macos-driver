; ModuleID = 'nvmtl-air'
source_filename = "t_misc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }

; Function Attrs: convergent mustprogress nounwind willreturn
define void @t_misc(%"struct.metal::_imageblock_base" %ib.coerce, <2 x i16> noundef %tid, ptr addrspace(1) %out.coerce, <2 x i16> noundef %tg) local_unnamed_addr #0 {
entry:
  %0 = tail call i16 @air.get_imageblock_samples() #6
  %1 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %tid, i32 0, i16 0) #7
  %2 = tail call fast <2 x float> @air.load.implicit_imageblock.v2f32(i32 1, <2 x i16> %tid, i32 0, i16 0) #7
  %3 = tail call i16 @air.load.implicit_imageblock.i16(i32 2, <2 x i16> %tid, i32 0, i16 0) #7
  %4 = tail call fast half @air.convert.f.f16.u.i16(i16 %0) #6
  %5 = insertelement <4 x half> %1, half %4, i64 0
  %add = fadd fast <2 x float> %2, splat (float 1.000000e+00)
  %add4 = add i16 %3, 2
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %5, i32 0, <2 x i16> %tid, i32 0, i16 0) #8
  tail call void @air.store.implicit_imageblock.v2f32(<2 x float> %add, i32 1, <2 x i16> %tid, i32 0, i16 0) #8
  tail call void @air.store.implicit_imageblock.i16(i16 %add4, i32 2, <2 x i16> %tid, i32 0, i16 0) #8
  tail call void @air.wg.barrier(i32 8, i32 5, i32 1) #9
  %6 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %tid, i32 0, i16 0) #7
  %7 = tail call <2 x i32> @air.convert.u.v2i32.u.v2i16(<2 x i16> %tid) #6
  tail call void @air.write_texture_2d.v4f16(ptr addrspace(1) captures(none) %out.coerce, <2 x i32> %7, <4 x half> %6, i32 0, i32 2) #10, !alias.scope !22
  %8 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %tid, i32 2, i16 1) #7
  %9 = tail call fast <2 x float> @air.load.implicit_imageblock.v2f32(i32 1, <2 x i16> %tid, i32 2, i16 1) #7
  %10 = tail call i16 @air.load.implicit_imageblock.i16(i32 2, <2 x i16> %tid, i32 2, i16 1) #7
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %8, i32 0, <2 x i16> %tid, i32 1, i16 1) #8
  tail call void @air.store.implicit_imageblock.v2f32(<2 x float> %9, i32 1, <2 x i16> %tid, i32 1, i16 1) #8
  tail call void @air.store.implicit_imageblock.i16(i16 %10, i32 2, <2 x i16> %tid, i32 1, i16 1) #8
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(write)
declare void @air.store.implicit_imageblock.i16(i16, i32, <2 x i16>, i32, i16) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn memory(write)
declare void @air.store.implicit_imageblock.v2f32(<2 x float>, i32, <2 x i16>, i32, i16) local_unnamed_addr #1

; Function Attrs: mustprogress nounwind willreturn memory(write)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare i16 @air.load.implicit_imageblock.i16(i32, <2 x i16>, i32, i16) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare <2 x float> @air.load.implicit_imageblock.v2f32(i32, <2 x i16>, i32, i16) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare <4 x half> @air.load.implicit_imageblock.v4f16(i32, <2 x i16>, i32, i16) local_unnamed_addr #2

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.write_texture_2d.v4f16(ptr addrspace(1) captures(none), <2 x i32>, <4 x half>, i32, i32) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare <2 x i32> @air.convert.u.v2i32.u.v2i16(<2 x i16>) local_unnamed_addr #4

; Function Attrs: convergent mustprogress nounwind willreturn
declare void @air.wg.barrier(i32, i32, i32) local_unnamed_addr #5

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare half @air.convert.f.f16.u.i16(i16) local_unnamed_addr #4

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare i16 @air.get_imageblock_samples() local_unnamed_addr #4

attributes #0 = { convergent mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="64" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(write) }
attributes #2 = { mustprogress nofree nounwind willreturn memory(read) }
attributes #3 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #4 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #5 = { convergent mustprogress nounwind willreturn }
attributes #6 = { nounwind willreturn memory(none) }
attributes #7 = { nounwind willreturn memory(read) }
attributes #8 = { nounwind willreturn memory(write) }
attributes #9 = { convergent nounwind willreturn }
attributes #10 = { nounwind willreturn memory(argmem: readwrite) }

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
!14 = !{ptr @t_misc, !15, !16}
!15 = !{}
!16 = !{!17, !19, !20, !21}
!17 = !{i32 0, !"air.imageblock", !"implicit", !"air.struct_type_info", !18, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"imageblock<MS, layout_implicit>", !"air.arg_name", !"ib"}
!18 = !{i32 0, i32 8, i32 0, !"half4", !"c", !"air.render_target", i32 0, i32 8, i32 8, i32 0, !"float2", !"f", !"air.render_target", i32 1, i32 16, i32 2, i32 0, !"short", !"s", !"air.render_target", i32 2}
!19 = !{i32 1, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tid"}
!20 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<half, write>", !"air.arg_name", !"out"}
!21 = !{i32 3, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tg", !"air.arg_unused"}
!22 = !{!23}
!23 = distinct !{!23, !24, !"air-alias-scope-textures"}
!24 = distinct !{!24, !"air-alias-scopes(t_misc)"}
