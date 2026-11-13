//! Visible function tables emulated by linking (the plugin's nvmtl_link_stage through nvmtl_link_runtime[_ids]): each
//! table call becomes a switch over the pipeline's linked functions, and a handle is the case value that selects its
//! function. Before Metal 4 pipeline-independent binary functions that value was always the function's place + 1; now
//! the plugin may give a candidate a process-wide value (at or above INDEPENDENT_ID_BASE) that is the same in every
//! pipeline linking it. The fixtures are user/tests/plugin/link/ (link.metal through this Mac's Metal compiler).
use metal2vulkan::linked_functions::INDEPENDENT_ID_BASE;
use metal2vulkan::passes::{Stage, TransformOptions};
use metal2vulkan::{disassemble, specialize_runtime_table_module, specialize_runtime_table_module_with_ids};

const APPLY: &str = include_str!("fixtures/runtime_table/apply.ll");
const OP_ADD: &str = include_str!("fixtures/runtime_table/op_add.ll");
const OP_MUL: &str = include_str!("fixtures/runtime_table/op_mul.ll");
const OP_NEG: &str = include_str!("fixtures/runtime_table/op_neg.ll");
const FS_TAB: &str = include_str!("fixtures/runtime_table/fs_tab.ll");
const COL_RED: &str = include_str!("fixtures/runtime_table/col_red.ll");
const COL_GREEN: &str = include_str!("fixtures/runtime_table/col_green.ll");

fn ops() -> Vec<(String, String)> {
    vec![("op_add".into(), OP_ADD.into()), ("op_mul".into(), OP_MUL.into()), ("op_neg".into(), OP_NEG.into())]
}

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

fn translate(ll: &str, stage: Stage) -> String {
    let spv = metal2vulkan::translate_native_no_retry_constructed_with_options(ll, stage, TransformOptions::default())
        .unwrap_or_else(|e| panic!("translation: {e}"));
    spirv_val(&spv);
    disassemble(&spv).unwrap()
}

/// The case values of the module's OpSwitch instructions (the dispatcher the table call became).
fn switch_cases(asm: &str) -> Vec<u64> {
    let mut out = Vec::new();
    for line in asm.lines().filter(|l| l.contains("OpSwitch")) {
        let words: Vec<&str> = line.split_whitespace().collect();
        let at = words.iter().position(|w| *w == "OpSwitch").unwrap();
        // OpSwitch %selector %default (literal %label)*
        let mut k = at + 3;
        while k + 1 < words.len() {
            if let Ok(v) = words[k].parse::<u64>() {
                out.push(v);
            }
            k += 2;
        }
    }
    out.sort_unstable();
    out
}

#[test]
fn places_are_the_default_and_the_module_is_unchanged() {
    let old = specialize_runtime_table_module(APPLY, Stage::Kernel, Vec::new(), &ops()).unwrap();
    let none = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &ops(), &[]).unwrap();
    let places = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &ops(), &[1, 2, 3]).unwrap();
    assert_eq!(old, none, "no values given: the link is what it was");
    assert_eq!(old, places, "the places given: the link is what it was");
    assert_eq!(switch_cases(&translate(&old, Stage::Kernel)), vec![1, 2, 3]);
}

#[test]
fn a_pipeline_independent_function_keeps_its_value_in_every_pipeline() {
    let pi = INDEPENDENT_ID_BASE + 7;
    // pipeline A links op_add, op_mul (independent); pipeline B links op_neg, op_mul (independent): op_mul is the same case
    let a = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &ops()[..2], &[1, pi]).unwrap();
    let b_cands = vec![ops()[2].clone(), ops()[1].clone()];
    let b = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &b_cands, &[1, pi]).unwrap();
    assert_eq!(switch_cases(&translate(&a, Stage::Kernel)), vec![1, pi as u64]);
    assert_eq!(switch_cases(&translate(&b, Stage::Kernel)), vec![1, pi as u64]);
    // the independent value comes first in the list here: entries are sorted by value for the linkage
    let c = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &ops(), &[pi, 2, 3]).unwrap();
    assert_eq!(switch_cases(&translate(&c, Stage::Kernel)), vec![2, 3, pi as u64]);
}

#[test]
fn a_fragment_table_takes_the_values_too() {
    let cands = vec![("col_red".to_string(), COL_RED.to_string()), ("col_green".to_string(), COL_GREEN.to_string())];
    let pi = INDEPENDENT_ID_BASE;
    let f = specialize_runtime_table_module_with_ids(FS_TAB, Stage::Fragment, Vec::new(), &cands, &[pi, 2]).unwrap();
    assert_eq!(switch_cases(&translate(&f, Stage::Fragment)), vec![2, pi as u64]);
}

#[test]
fn values_that_are_neither_places_nor_independent_are_refused() {
    for ids in [&[0u32, 2][..], &[1, 5], &[2, 2], &[INDEPENDENT_ID_BASE, INDEPENDENT_ID_BASE]] {
        let e = specialize_runtime_table_module_with_ids(APPLY, Stage::Kernel, Vec::new(), &ops()[..2], ids).unwrap_err();
        assert!(e.contains("handle value"), "{ids:?}: {e}");
    }
}
