target triple = "air64_v28-apple-macosx26.7.0"

%struct.Bindless = type { [8 x %"struct.metal::texture2d"] }
%"struct.metal::texture2d" = type { ptr addrspace(1) }

define void @k_fixed(ptr addrspace(2) noundef readonly align 8 %b, ptr addrspace(2) %s, ptr addrspace(1) noundef "air-buffer-no-alias" %out, i32 noundef %i) local_unnamed_addr #0 {
entry:
  %idx = zext i32 %i to i64
  %field = getelementptr inbounds %struct.Bindless, ptr addrspace(2) %b, i64 0, i32 0, i64 5
  %tex = load ptr addrspace(1), ptr addrspace(2) %field, align 8
  %r = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %s, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %dst = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idx
  store <4 x float> %v, ptr addrspace(1) %dst, align 16
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nounwind willreturn }
attributes #1 = { mustprogress nofree nounwind willreturn memory(argmem: read) }

!air.kernel = !{!0}
!air.version = !{!9}
!air.language_version = !{!10}

!0 = !{ptr @k_fixed, !1, !2}
!1 = !{}
!2 = !{!3, !6, !7, !8}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !4, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Bindless", !"air.arg_name", !"b"}
!4 = !{i32 0, i32 64, i32 8, !"array<texture2d<float, sample>, 8>", !"t", !"air.indirect_argument", !5}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 8, !"air.sample", !"air.arg_type_name", !"array<texture2d<float, sample>, 8>", !"air.arg_name", !"t"}
!6 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 4, i32 0, i32 0}
