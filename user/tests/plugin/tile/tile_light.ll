; ModuleID = 'nvmtl-air'
source_filename = "tile_light"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }

; Function Attrs: mustprogress nounwind willreturn
define void @tile_light(%"struct.metal::_imageblock_base" %ib.coerce, <2 x i16> noundef %tid, ptr addrspace(2) noundef readonly align 16 captures(none) dereferenceable(16) "air-buffer-no-alias" %L) local_unnamed_addr #0 {
entry:
  %0 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %tid, i32 0, i16 0) #4
  %1 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 1, <2 x i16> %tid, i32 0, i16 0) #4
  %2 = shufflevector <4 x half> %1, <4 x half> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %mul = fmul fast <3 x half> %2, splat (half 0xH4000)
  %sub = fadd fast <3 x half> %mul, splat (half 0xHBC00)
  %3 = load <4 x float>, ptr addrspace(2) %L, align 16, !alias.scope !21
  %4 = shufflevector <4 x float> %3, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %5 = tail call fast <3 x half> @air.convert.f.v3f16.f.v3f32(<3 x float> %4) #5
  %6 = tail call fast half @air.dot.v3f16(<3 x half> %sub, <3 x half> %5) #5
  %7 = tail call fast half @air.fmax.f16(half %6, half 0xH0000) #5
  %8 = shufflevector <4 x half> %0, <4 x half> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %splat.splatinsert = insertelement <3 x half> poison, half %7, i64 0
  %splat.splat = shufflevector <3 x half> %splat.splatinsert, <3 x half> poison, <3 x i32> zeroinitializer
  %mul4 = fmul fast <3 x half> %splat.splat, %8
  %vecinit = shufflevector <3 x half> %mul4, <3 x half> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>
  %vecinit5 = insertelement <4 x half> %vecinit, half 0xH3C00, i64 3
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %0, i32 0, <2 x i16> %tid, i32 0, i16 0) #6
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %1, i32 1, <2 x i16> %tid, i32 0, i16 0) #6
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %vecinit5, i32 2, <2 x i16> %tid, i32 0, i16 0) #6
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(write)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare half @air.fmax.f16(half, half) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare half @air.dot.v3f16(<3 x half>, <3 x half>) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare <3 x half> @air.convert.f.v3f16.f.v3f32(<3 x float>) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare <4 x half> @air.load.implicit_imageblock.v4f16(i32, <2 x i16>, i32, i16) local_unnamed_addr #3

attributes #0 = { mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="64" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(write) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #3 = { mustprogress nofree nounwind willreturn memory(read) }
attributes #4 = { nounwind willreturn memory(read) }
attributes #5 = { nounwind willreturn memory(none) }
attributes #6 = { nounwind willreturn memory(write) }

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
!14 = !{ptr @tile_light, !15, !16}
!15 = !{}
!16 = !{!17, !19, !20}
!17 = !{i32 0, !"air.imageblock", !"implicit", !"air.struct_type_info", !18, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"imageblock<Deferred, layout_implicit>", !"air.arg_name", !"ib"}
!18 = !{i32 0, i32 8, i32 0, !"half4", !"albedo", !"air.render_target", i32 0, i32 8, i32 8, i32 0, !"half4", !"normal", !"air.render_target", i32 1, i32 16, i32 8, i32 0, !"half4", !"lit", !"air.render_target", i32 2}
!19 = !{i32 1, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tid"}
!20 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"L"}
!21 = !{!22}
!22 = distinct !{!22, !23, !"air-alias-scope-arg(2)"}
!23 = distinct !{!23, !"air-alias-scopes(tile_light)"}
