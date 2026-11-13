; ModuleID = 'nvmtl-air'
source_filename = "tile_tg"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v29-apple-macosx27.0.0"

%"struct.metal::_imageblock_base" = type { ptr addrspace(4) }

; Function Attrs: convergent mustprogress nounwind
define void @tile_tg(%"struct.metal::_imageblock_base" %ib.coerce, <2 x i16> noundef %tid, <2 x i16> noundef %tsz, ptr addrspace(3) noundef captures(none) "air-buffer-no-alias" %acc, ptr addrspace(2) noundef readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %k) local_unnamed_addr #0 {
entry:
  %0 = extractelement <2 x i16> %tid, i64 0
  %conv = zext i16 %0 to i32
  %1 = extractelement <2 x i16> %tid, i64 1
  %conv2 = zext i16 %1 to i32
  %2 = extractelement <2 x i16> %tsz, i64 0
  %conv3 = zext i16 %2 to i32
  %mul = mul nuw nsw i32 %conv3, %conv2
  %add = add nuw nsw i32 %mul, %conv
  %3 = tail call fast <4 x half> @air.load.implicit_imageblock.v4f16(i32 0, <2 x i16> %tid, i32 0, i16 0) #5
  %4 = extractelement <4 x half> %3, i64 0
  %conv4 = fpext half %4 to float
  %idxprom = zext i32 %add to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(3) %acc, i64 %idxprom
  store float %conv4, ptr addrspace(3) %arrayidx, align 4, !tbaa !23, !alias.scope !27, !noalias !30
  tail call void @air.wg.barrier(i32 2, i32 5, i32 1) #6
  %cmp = icmp eq i32 %add, 0
  br i1 %cmp, label %for.cond.preheader, label %if.end

for.cond.preheader:                               ; preds = %entry
  %5 = extractelement <2 x i16> %tsz, i64 1
  %conv6 = zext i16 %5 to i32
  %mul7 = mul nuw i32 %conv3, %conv6
  %cmp840.not = icmp eq i32 %mul7, 0
  br i1 %cmp840.not, label %for.cond.cleanup, label %for.body

for.cond.cleanup:                                 ; preds = %for.body, %for.cond.preheader
  %s.0.lcssa = phi float [ 0.000000e+00, %for.cond.preheader ], [ %add11, %for.body ]
  %6 = tail call fast float @air.convert.f.f32.s.i32(i32 %mul7) #7
  %div = fdiv fast float %s.0.lcssa, %6
  store float %div, ptr addrspace(3) %acc, align 4, !tbaa !23, !alias.scope !27, !noalias !30
  br label %if.end

for.body:                                         ; preds = %for.body, %for.cond.preheader
  %i.042 = phi i32 [ %inc, %for.body ], [ 0, %for.cond.preheader ]
  %s.041 = phi float [ %add11, %for.body ], [ 0.000000e+00, %for.cond.preheader ]
  %idxprom9 = zext i32 %i.042 to i64
  %arrayidx10 = getelementptr inbounds float, ptr addrspace(3) %acc, i64 %idxprom9
  %7 = load float, ptr addrspace(3) %arrayidx10, align 4, !tbaa !23, !alias.scope !27, !noalias !30
  %add11 = fadd fast float %7, %s.041
  %inc = add nuw i32 %i.042, 1
  %exitcond.not = icmp eq i32 %inc, %mul7
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !32

if.end:                                           ; preds = %for.cond.cleanup, %entry
  tail call void @air.wg.barrier(i32 2, i32 5, i32 1) #6
  %8 = load float, ptr addrspace(3) %acc, align 4, !tbaa !23, !alias.scope !27, !noalias !30
  %9 = load float, ptr addrspace(2) %k, align 4, !tbaa !23, !alias.scope !30, !noalias !27
  %mul17 = fmul fast float %9, %8
  %conv18 = fptrunc float %mul17 to half
  %10 = insertelement <4 x half> %3, half %conv18, i64 1
  tail call void @air.store.implicit_imageblock.v4f16(<4 x half> %10, i32 0, <2 x i16> %tid, i32 0, i16 0) #8
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(write)
declare void @air.store.implicit_imageblock.v4f16(<4 x half>, i32, <2 x i16>, i32, i16) local_unnamed_addr #1

; Function Attrs: convergent mustprogress nounwind willreturn
declare void @air.wg.barrier(i32, i32, i32) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare float @air.convert.f.f32.s.i32(i32) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nounwind willreturn memory(read)
declare <4 x half> @air.load.implicit_imageblock.v4f16(i32, <2 x i16>, i32, i16) local_unnamed_addr #4

attributes #0 = { convergent mustprogress nounwind "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="64" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(write) }
attributes #2 = { convergent mustprogress nounwind willreturn }
attributes #3 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #4 = { mustprogress nofree nounwind willreturn memory(read) }
attributes #5 = { nounwind willreturn memory(read) }
attributes #6 = { convergent nounwind willreturn }
attributes #7 = { nounwind willreturn memory(none) }
attributes #8 = { nounwind willreturn memory(write) }

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
!14 = !{ptr @tile_tg, !15, !16}
!15 = !{}
!16 = !{!17, !19, !20, !21, !22}
!17 = !{i32 0, !"air.imageblock", !"implicit", !"air.struct_type_info", !18, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"imageblock<MS, layout_implicit>", !"air.arg_name", !"ib"}
!18 = !{i32 0, i32 8, i32 0, !"half4", !"c", !"air.render_target", i32 0}
!19 = !{i32 1, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tid"}
!20 = !{i32 2, !"air.threads_per_threadgroup", !"air.arg_type_name", !"ushort2", !"air.arg_name", !"tsz"}
!21 = !{i32 3, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"acc"}
!22 = !{i32 4, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"k"}
!23 = !{!24, !24, i64 0}
!24 = !{!"float", !25, i64 0}
!25 = !{!"omnipotent char", !26, i64 0}
!26 = !{!"Simple C++ TBAA"}
!27 = !{!28}
!28 = distinct !{!28, !29, !"air-alias-scope-arg(3)"}
!29 = distinct !{!29, !"air-alias-scopes(tile_tg)"}
!30 = !{!31}
!31 = distinct !{!31, !29, !"air-alias-scope-arg(4)"}
!32 = distinct !{!32, !33}
!33 = !{!"llvm.loop.mustprogress"}
