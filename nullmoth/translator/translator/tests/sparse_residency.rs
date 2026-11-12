//! Sparse-texture residency: `sparse_color<T>` from `sparse_sample`, `sparse_read`, `sparse_gather`,
//! `sparse_sample_compare` and `sparse_gather_compare`. Apple's frontend emits the same intrinsic as the plain call, whose
//! `{ T, i8 }` result carries the residency in member 1 (nonzero: not resident); the sparse form only extracts it. The
//! fixtures are the AIR Apple's compiler (Metal 4.1, AIR 2.9) made of `fixtures/sparse/probe.metal`.
//!
//! With `TransformOptions::sparse_residency` (the plugin's NVMTL_SPARSE_TEXTURES) a read residency becomes the call's
//! OpImageSparse* twin and OpImageSparseTexelsResident; without it every texture is resident and the flag is 0. Before
//! this the flag was OpUndef: a shader branching on `resident()` took either side.
use metal2vulkan::passes::{Stage, TransformOptions};
use metal2vulkan::{disassemble, translate_native_no_retry_constructed_with_options};

fn spirv_val(spv: &[u8]) {
    let out = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .and_then(|mut c| {
            use std::io::Write;
            c.stdin.take().unwrap().write_all(spv)?;
            c.wait_with_output()
        });
    if let Ok(out) = out {
        assert!(out.status.success(), "spirv-val: {}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    }
}

fn translate(ll: &str, sparse: bool) -> Result<String, String> {
    let options = TransformOptions { sparse_residency: sparse, ..TransformOptions::default() };
    let spv = translate_native_no_retry_constructed_with_options(ll, Stage::Fragment, options)?;
    spirv_val(&spv);
    disassemble(&spv)
}

/// (fixture, the sparse instruction it must become, the plain instruction it must no longer use)
const RESIDENT: &[(&str, &str, &str)] = &[
    (include_str!("fixtures/sparse/sp_sample_2d.ll"), "OpImageSparseSampleImplicitLod", "OpImageSampleImplicitLod"),
    (include_str!("fixtures/sparse/sp_sample_level_2d.ll"), "OpImageSparseSampleExplicitLod", "OpImageSampleExplicitLod"),
    (include_str!("fixtures/sparse/sp_sample_grad_2d.ll"), "OpImageSparseSampleExplicitLod", "OpImageSampleExplicitLod"),
    (include_str!("fixtures/sparse/sp_sample_minlod_2d.ll"), "OpImageSparseSampleImplicitLod", "OpImageSampleImplicitLod"),
    (include_str!("fixtures/sparse/sp_read_2d.ll"), "OpImageSparseFetch", "OpImageFetch"),
    (include_str!("fixtures/sparse/sp_gather_2d.ll"), "OpImageSparseGather", "OpImageGather"),
    (include_str!("fixtures/sparse/sp_sample_2da_half.ll"), "OpImageSparseSampleImplicitLod", "OpImageSampleImplicitLod"),
    (include_str!("fixtures/sparse/sp_read_3d_uint.ll"), "OpImageSparseFetch", "OpImageFetch"),
    (include_str!("fixtures/sparse/sp_sample_cube.ll"), "OpImageSparseSampleImplicitLod", "OpImageSampleImplicitLod"),
    (include_str!("fixtures/sparse/sp_depth_compare.ll"), "OpImageSparseSampleDrefImplicitLod", "OpImageSampleDrefImplicitLod"),
    (include_str!("fixtures/sparse/sp_depth_gather_compare.ll"), "OpImageSparseDrefGather", "OpImageDrefGather"),
    (include_str!("fixtures/sparse/sp_depth_read.ll"), "OpImageSparseFetch", "OpImageFetch"),
    (include_str!("fixtures/sparse/sp_rw_read.ll"), "OpImageSparseRead", "OpImageRead"),
];

fn count(asm: &str, op: &str) -> usize {
    asm.split_whitespace().filter(|w| *w == op).count()
}

#[test]
fn residency_reads_become_sparse_image_instructions() {
    for (ll, sparse_op, plain_op) in RESIDENT {
        let name = ll.lines().find_map(|l| l.strip_prefix("source_filename = ")).unwrap_or("?");
        let asm = translate(ll, true).unwrap_or_else(|e| panic!("{name}: must translate: {e}"));
        assert_eq!(count(&asm, sparse_op), 1, "{name}: one {sparse_op}\n{asm}");
        assert_eq!(count(&asm, plain_op), 0, "{name}: the plain {plain_op} is replaced\n{asm}");
        assert_eq!(count(&asm, "OpImageSparseTexelsResident"), 1, "{name}: one residency query\n{asm}");
        assert!(asm.contains("OpCapability SparseResidency"), "{name}: SparseResidency declared\n{asm}");
        // the flag Metal reads is 0 when resident: a select of the residency between 0 and 1
        let resident = asm.lines().find_map(|l| l.split_once(" = OpImageSparseTexelsResident").map(|(id, _)| id.trim().to_string()))
            .unwrap_or_default();
        assert!(asm.lines().any(|l| l.contains("OpSelect") && l.split_whitespace().nth(4) == Some(resident.as_str())),
                "{name}: the AIR flag is selected by the residency {resident}\n{asm}");
    }
}

#[test]
fn without_sparse_textures_every_texture_reads_resident() {
    for (ll, sparse_op, plain_op) in RESIDENT {
        let name = ll.lines().find_map(|l| l.strip_prefix("source_filename = ")).unwrap_or("?");
        let asm = translate(ll, false).unwrap_or_else(|e| panic!("{name}: must translate: {e}"));
        assert_eq!(count(&asm, sparse_op), 0, "{name}: no {sparse_op} without sparse textures\n{asm}");
        assert_eq!(count(&asm, plain_op), 1, "{name}: the plain {plain_op}\n{asm}");
        assert!(!asm.contains("SparseResidency") && !asm.contains("OpImageSparseTexelsResident"), "{name}\n{asm}");
        // the flag is the constant 0 (resident), not an undefined value a branch on resident() could take either way
        let uchar = asm.lines().find_map(|l| l.split_once(" = OpTypeInt").filter(|(_, r)| r.split_whitespace().next() == Some("8"))
            .map(|(id, _)| id.trim().to_string()));
        if let Some(uchar) = uchar {
            assert!(!asm.lines().any(|l| { let w: Vec<&str> = l.split_whitespace().collect(); w.get(2) == Some(&"OpUndef") && w.get(3) == Some(&uchar.as_str()) }),
                    "{name}: no undefined residency\n{asm}");
        }
    }
}

#[test]
fn a_call_whose_residency_is_not_read_stays_plain() {
    let asm = translate(include_str!("fixtures/sparse/sp_value_only.ll"), true).expect("must translate");
    assert!(!asm.contains("OpImageSparse") && !asm.contains("SparseResidency"), "{asm}");
    assert_eq!(count(&asm, "OpImageSampleImplicitLod"), 1, "{asm}");
}

#[test]
fn a_residency_the_lowering_computes_from_several_texels_is_refused() {
    let ll = include_str!("fixtures/sparse/sp_pixel_array_refused.ll");
    let err = translate(ll, true).expect_err("a filtered pixel-coordinate sample has no single residency");
    assert!(err.contains("sparse_color::resident() is read") && err.contains("cannot be expressed"), "{err}");
    // without sparse textures the same shader translates: every texture is resident
    translate(ll, false).expect("resident without sparse textures");
}
