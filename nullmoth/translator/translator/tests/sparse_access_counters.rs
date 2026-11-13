//! Metal sparse textures, tier 2: per-tile access counters (`TransformOptions::sparse_access_counters`, the plugin's
//! NVMTL_SPARSE_TIER2; src/sparse_counters.rs). Every texel read of a texture the module loads from the bindless heap
//! calls one helper, which reads the heap slot's directory element and adds to the tile's counter. The fixtures are the
//! sparse probes of `fixtures/sparse/probe.metal` (AIR from Apple's compiler) and, for "nothing else changes", every
//! `.ll` fixture of this crate.
use metal2vulkan::passes::{Stage, TransformOptions};
use metal2vulkan::sparse_counters::{COUNTER_HEAP_SLOT, HELPER_NAME};
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

fn options(residency: bool, counters: bool) -> TransformOptions {
    TransformOptions { sparse_residency: residency, sparse_access_counters: counters, ..TransformOptions::default() }
}

fn translate(ll: &str, stage: Stage, o: TransformOptions) -> Result<Vec<u8>, String> {
    translate_native_no_retry_constructed_with_options(ll, stage, o)
}

fn name(ll: &str) -> &str {
    ll.lines().find_map(|l| l.strip_prefix("source_filename = ")).unwrap_or("?")
}

fn count(asm: &str, word: &str) -> usize {
    asm.split_whitespace().filter(|w| *w == word).count()
}

/// The helper's id (from its OpName)
fn helper(asm: &str) -> Option<String> {
    let tail = format!("\"{HELPER_NAME}\"");
    asm.lines().find_map(|l| {
        let w: Vec<&str> = l.split_whitespace().collect();
        (w.first() == Some(&"OpName") && w.get(2) == Some(&tail.as_str())).then(|| w[1].to_string())
    })
}

/// The ids of the helper's calls' arguments: (slot, level, slice, coordinates, normalized)
fn calls(asm: &str) -> Vec<Vec<String>> {
    let Some(callee) = helper(asm) else { return Vec::new() };
    asm.lines()
        .filter(|l| l.contains("OpFunctionCall") && l.split_whitespace().any(|w| w == callee))
        .map(|l| l.split_whitespace().skip_while(|w| *w != callee).skip(1).map(str::to_string).collect())
        .collect()
}

/// The instruction that defines `id` (its words after `=`)
fn def(asm: &str, id: &str) -> Vec<String> {
    asm.lines()
        .find_map(|l| l.trim().strip_prefix(&format!("{id} = ")).map(|r| r.split_whitespace().map(str::to_string).collect()))
        .unwrap_or_default()
}

/// The heap reads each fixture makes, all counted: (fixture, the level argument's kind)
const COUNTED: &[(&str, &str)] = &[
    (include_str!("fixtures/sparse/sp_sample_2d.ll"), "querylod"),
    (include_str!("fixtures/sparse/sp_sample_level_2d.ll"), "lod"),
    (include_str!("fixtures/sparse/sp_sample_grad_2d.ll"), "grad"),
    (include_str!("fixtures/sparse/sp_sample_minlod_2d.ll"), "querylod"),
    (include_str!("fixtures/sparse/sp_read_2d.ll"), "fetch"),
    (include_str!("fixtures/sparse/sp_gather_2d.ll"), "zero"),
    (include_str!("fixtures/sparse/sp_sample_2da_half.ll"), "querylod"),
    (include_str!("fixtures/sparse/sp_read_3d_uint.ll"), "fetch"),
    (include_str!("fixtures/sparse/sp_sample_cube.ll"), "querylod"),
    (include_str!("fixtures/sparse/sp_depth_read.ll"), "fetch"),
    (include_str!("fixtures/sparse/sp_value_only.ll"), "querylod"),
];

#[test]
fn every_heap_texel_read_counts_its_tile() {
    for residency in [true, false] {
        for (ll, level) in COUNTED {
            let n = name(ll);
            let spv = translate(ll, Stage::Fragment, options(residency, true)).unwrap_or_else(|e| panic!("{n}: {e}"));
            spirv_val(&spv);
            let asm = disassemble(&spv).unwrap();
            let c = calls(&asm);
            assert_eq!(c.len(), 1, "{n}: one counted read\n{asm}");
            let h = helper(&asm).unwrap();
            assert_eq!(def(&asm, &h).first().map(String::as_str), Some("OpFunction"), "{n}: one helper\n{asm}");
            assert_eq!(count(&asm, "OpAtomicIAdd"), 1, "{n}: one atomic, in the helper\n{asm}");
            assert_eq!(count(&asm, "OpGroupNonUniformBallot"), 1, "{n}: the subgroup's lanes on one tile add once\n{asm}");
            assert!(asm.contains("BuiltIn HelperInvocation"), "{n}: helper invocations count nothing\n{asm}");
            // the directory (binding 3) and counters (binding 4) of the reserved slot of the heap set
            for binding in [3, 4] {
                let var = asm.lines().find_map(|l| {
                    let w: Vec<&str> = l.split_whitespace().collect();
                    (w.len() == 4 && w[0] == "OpDecorate" && w[2] == "Binding" && w[3] == binding.to_string()).then(|| w[1].to_string())
                });
                let var = var.unwrap_or_else(|| panic!("{n}: heap binding {binding}\n{asm}"));
                assert!(asm.lines().any(|l| l.split_whitespace().collect::<Vec<_>>() == ["OpDecorate", var.as_str(), "DescriptorSet", "2"]),
                        "{n}: binding {binding} in the heap set\n{asm}");
            }
            let uint = asm.lines().find_map(|l| l.trim().strip_suffix(" = OpTypeInt 32 0").map(str::to_string)).unwrap();
            let slot = COUNTER_HEAP_SLOT.to_string();
            assert!(asm.lines().any(|l| l.split_whitespace().skip(2).collect::<Vec<_>>() == ["OpConstant", uint.as_str(), slot.as_str()]),
                    "{n}: the reserved slot {COUNTER_HEAP_SLOT}\n{asm}");
            assert!(asm.contains(&format!("OpTypeImage {uint} Buffer 0 0 0 2 R32ui")) && asm.contains(&format!("OpTypeImage {uint} Buffer 0 0 0 1 Unknown")),
                    "{n}: the counter and directory views\n{asm}");
            let args = &c[0];
            let level_arg = args[1].as_str();
            match *level {
                "querylod" => assert!(count(&asm, "OpImageQueryLod") == 1 && asm.contains("OpCapability ImageQuery"),
                                      "{n}: the implicit LOD of the fragment\n{asm}"),
                "grad" => assert!(count(&asm, "OpImageQuerySizeLod") == 1 && asm.contains("Log2") && count(&asm, "OpImageQueryLod") == 0,
                                  "{n}: log2 of the scaled gradient\n{asm}"),
                "lod" => assert!(count(&asm, "OpImageQueryLod") == 0 && count(&asm, "OpImageQuerySizeLod") == 0, "{n}: the level() argument\n{asm}"),
                "fetch" => assert!(asm.lines().any(|l| l.contains("Fetch") && l.split_whitespace().collect::<Vec<_>>().windows(2).any(|w| w == ["Lod", level_arg]))
                                   || def(&asm, level_arg).first().map(String::as_str) == Some("OpBitcast"),
                                   "{n}: the fetch's LOD is the level ({level_arg})\n{asm}"),
                "zero" => assert_eq!(def(&asm, level_arg), ["OpConstant", uint.as_str(), "0"], "{n}: a gather reads level 0\n{asm}"),
                _ => unreachable!(),
            }
            // normalized coordinates for samples and gathers, texels for reads
            let want = if *level == "fetch" { "OpConstantFalse" } else { "OpConstantTrue" };
            assert_eq!(def(&asm, &args[4]).first().map(String::as_str), Some(want), "{n}\n{asm}");
        }
    }
}

#[test]
fn reads_that_cannot_be_of_a_sparse_texture_are_not_counted() {
    // depth compares (a depth texture is never sparse here) and a read-write texture bound directly (no heap slot)
    for ll in [
        include_str!("fixtures/sparse/sp_depth_compare.ll"),
        include_str!("fixtures/sparse/sp_depth_gather_compare.ll"),
        include_str!("fixtures/sparse/sp_rw_read.ll"),
    ] {
        let n = name(ll);
        for residency in [true, false] {
            let off = translate(ll, Stage::Fragment, options(residency, false)).unwrap_or_else(|e| panic!("{n}: {e}"));
            let on = translate(ll, Stage::Fragment, options(residency, true)).unwrap_or_else(|e| panic!("{n}: {e}"));
            assert_eq!(on, off, "{n}: nothing to count, the module is unchanged");
        }
    }
}

#[test]
fn the_count_is_off_by_default() {
    assert!(!TransformOptions::default().sparse_access_counters);
    let ll = include_str!("fixtures/sparse/sp_sample_2d.ll");
    let asm = disassemble(&translate(ll, Stage::Fragment, options(true, false)).unwrap()).unwrap();
    assert!(helper(&asm).is_none() && !asm.contains("OpAtomicIAdd") && !asm.contains("HelperInvocation"), "{asm}");
}

fn fixtures(dir: &std::path::Path, out: &mut Vec<std::path::PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            fixtures(&p, out);
        } else if p.extension().is_some_and(|x| x == "ll") {
            out.push(p);
        }
    }
}

/// The corpus: with the counters off every module is what the default options give, and with them on a module changes
/// only when it reads a heap texture, and then it validates.
#[test]
fn the_corpus_changes_only_where_a_heap_texture_is_read() {
    let mut files = Vec::new();
    fixtures(&std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures"), &mut files);
    fixtures(&std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/testdata"), &mut files);
    files.sort();
    let (mut units, mut counted, mut stages_counted) = (0, 0, std::collections::BTreeSet::new());
    for f in &files {
        let ll = std::fs::read_to_string(f).unwrap();
        for (tag, st) in [("!air.vertex =", Stage::Vertex), ("!air.fragment =", Stage::Fragment), ("!air.kernel =", Stage::Kernel)] {
            if !ll.contains(tag) {
                continue;
            }
            let Ok(default) = translate(&ll, st, TransformOptions::default()) else { continue };
            units += 1;
            let off = translate(&ll, st, options(false, false)).expect("translates as with the default options");
            assert_eq!(off, default, "{}: counters off must be byte-identical", f.display());
            let on = translate(&ll, st, options(false, true)).unwrap_or_else(|e| panic!("{}: {e}", f.display()));
            if on == off {
                continue;
            }
            let asm = disassemble(&on).unwrap();
            assert!(helper(&asm).is_some() && !calls(&asm).is_empty(), "{}: changed without counting\n{asm}", f.display());
            spirv_val(&on);
            counted += 1;
            stages_counted.insert(format!("{st:?}"));
        }
    }
    eprintln!("corpus: {units} modules, {counted} count heap texel reads, stages {stages_counted:?}");
    assert!(units > 50, "the corpus translated only {units} modules");
    assert!(counted > 0, "no module of the corpus reads a heap texture");
}
