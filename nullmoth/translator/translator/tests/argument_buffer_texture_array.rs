//! `array<texture2d<float>, N>` inside an argument buffer (tier 2): the app writes each element's `gpuResourceID`
//! (eight bytes, the bindless-heap slot in the low word) at `field + 8 * i`, and the shader reads element `i`. Element
//! `i` must read the heap slot in ITS word, for a dynamic and a constant `i` alike. The array used to become one
//! `[N x image]` variable at the field's binding: the plugin declares every texture binding with a count of one and
//! binds the field's first resource ID there, so element 0 read the right texture and every other element read
//! nothing (mtl_desc_paths argbuf-array and use-heap on the B580, 2026-11-13: 7 of 8 wrong).
use metal2vulkan::passes::{Stage, TransformOptions};
use metal2vulkan::{disassemble, translate_native_no_retry_constructed_with_options};

const DYNAMIC: &str = include_str!("fixtures/argbuf_texture_array/k_dynamic.ll");
const FIXED: &str = include_str!("fixtures/argbuf_texture_array/k_fixed.ll");

fn spirv_val(spv: &[u8]) {
    let out = std::process::Command::new("spirv-val").args(["--target-env", "vulkan1.3", "-"]).stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::piped()).spawn().and_then(|mut c| {
            use std::io::Write; c.stdin.take().unwrap().write_all(spv)?; c.wait_with_output() });
    if let Ok(out) = out { assert!(out.status.success(), "spirv-val: {}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr)); }
}

fn translate(ll: &str) -> String {
    let spv = translate_native_no_retry_constructed_with_options(ll, Stage::Kernel, TransformOptions::default())
        .expect("the shader must translate");
    spirv_val(&spv);
    disassemble(&spv).expect("disassemble")
}

/// No image variable is an array (the field's `[N x image]` binding is gone), and the one sampled image is loaded from
/// the bindless heap at a slot masked out of a word read from the argument buffer.
fn assert_heap_element(asm: &str) {
    assert!(!asm.lines().any(|line| line.contains("OpTypeArray") && {
        let elem = line.split_whitespace().nth(3).unwrap_or("");
        asm.lines().any(|l| l.trim_start().starts_with(&format!("{elem} = OpTypeImage")))
    }), "an image array is still declared\n{asm}");
    let sampled: Vec<_> = asm.lines().filter(|line| line.contains("OpSampledImage")).collect();
    assert_eq!(sampled.len(), 1, "{asm}");
    let image = sampled[0].split_whitespace().nth(4).unwrap();
    let def = |id: &str| asm.lines().find(|l| l.trim_start().starts_with(&format!("{id} = "))).unwrap_or_else(|| panic!("{id}\n{asm}"));
    let load: Vec<_> = def(image).split_whitespace().collect();
    assert_eq!(load[2], "OpLoad", "{asm}");
    let chain: Vec<_> = def(load[4]).split_whitespace().filter(|w| !w.is_empty()).collect();
    assert!(chain[2].ends_with("AccessChain"), "{asm}");
    let heap = chain[4];
    let heap_var: Vec<_> = def(heap).split_whitespace().collect();
    assert_eq!(heap_var[2], "OpVariable", "{asm}");
    assert!(asm.contains(&format!("OpDecorate {heap} DescriptorSet {}", metal2vulkan::reflect::BINDLESS_HEAP_SET)), "{asm}");
    let slot: Vec<_> = def(chain[5]).split_whitespace().collect();
    assert_eq!(slot[2], "OpBitwiseAnd", "the heap slot is not masked out of a resource-ID word\n{asm}");
}

#[test]
fn a_dynamically_indexed_texture_array_reads_each_elements_heap_slot() {
    let asm = translate(DYNAMIC);
    assert_heap_element(&asm);
}

#[test]
fn a_constant_element_of_a_texture_array_reads_its_own_heap_slot() {
    let asm = translate(FIXED);
    assert_heap_element(&asm);
    // element 5's resource ID: the low word at byte 40 of the argument buffer (word 10)
    assert!(asm.lines().any(|l| l.contains("OpConstant") && l.trim_end().ends_with(" 10")), "{asm}");
}
