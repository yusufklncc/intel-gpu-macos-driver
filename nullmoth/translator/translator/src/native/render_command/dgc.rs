//! NVMTL_ICB_DGC=1 (docs/design/DESIGN-icb-dgc.md): the extended slot record and the ICB state commands.
//!
//! With the knob the plugin gives every GPU-encodable ICB 1024-byte records (plugin/nvmtl_icb_layout.h,
//! NVMTL_ICB_DGC_*), whose first 64 bytes are the classic record (render_command.rs) and whose rest holds what
//! VK_EXT_device_generated_commands reads per sequence (an index buffer token, vertex buffer tokens, buffer addresses for
//! push-constant tokens), the compute words (a VkDispatchIndirectCommand and the threadgroup size) and CPU-written words
//! that tell a shader what the execute can express:
//!
//! | word     | written by | meaning                                                                      |
//! |----------|------------|------------------------------------------------------------------------------|
//! | 0..15    |            | the classic record                                                           |
//! | 16..19   | CPU + GPU  | VkBindIndexBufferIndirectCommandEXT (address, size, VkIndexType)            |
//! | 20       | CPU        | mode: bit 0 = this ICB's render executes run through DGC                     |
//! | 21       | CPU        | vertex buffer indices a shader may set (bit m)                               |
//! | 22       | CPU        | fragment (render) or kernel (compute) buffer indices a shader may set        |
//! | 23       | GPU        | sticky refusal: a state command the execute cannot express was encoded here  |
//! | 24,25    | CPU        | resource ID of the pipeline the execute binds (0: not known yet)             |
//! | 26,27    | GPU        | resource ID a shader set (diagnostic)                                        |
//! | 28,29,30 | CPU        | address and element count of the identity index buffer (uint32 0..n-1)       |
//! | 31       | GPU        | 1 after set_barrier, 0 after clear_barrier                                   |
//! | 32..34   | CPU + GPU  | VkDispatchIndirectCommand (threadgroups per grid)                            |
//! | 35..37   | GPU        | threads per threadgroup a shader asked for (the plugin learns it from here)  |
//! | 38..40   | CPU        | threads per threadgroup the execute dispatches the slot with (0: none yet)   |
//! | 41..47   | GPU        | scratch: where a refused buffer store lands                                  |
//! | 48..171  | CPU + GPU  | vertex buffer m at 48 + 4m: VkBindVertexBufferIndirectCommandEXT             |
//! | 172..233 | CPU + GPU  | fragment / kernel buffer m at 172 + 2m: its address                          |
//!
//! In DGC mode (word 20 bit 0) an indexed draw takes its index buffer from its own pointer (word 16..19, first index 0)
//! instead of being checked against the CPU-bound index buffer, and a plain draw is executed as an indexed draw through
//! the identity index buffer (first index = first vertex, so every slot of a run has the same token layout and the run
//! stays in order); outside it the classic checks apply unchanged. The topology still comes from the CPU (word 11).
//! A state command the execute cannot express (a buffer index the CPU did not offer in word 21/22, a pipeline other
//! than the one the execute binds, a render barrier) refuses the slot: kind 3, zero counts, and word 23 set so that a
//! draw encoded after it is refused too; reset clears it.
use super::{call_arguments, Emit, KIND_DRAW, KIND_EMPTY, KIND_INDEXED, KIND_REFUSED};
use super::{W_DRAW, W_EXEC_PRIM, W_IB_HI, W_IB_LIMIT, W_IB_LO, W_IB_SHIFT, W_IDX, W_KIND, W_PRIM};
use std::borrow::Cow;
use std::collections::HashMap;

/// 32-bit words per record with the knob (a power of two: the slot index is shifted by RECORD_SHIFT).
pub(crate) const RECORD_WORDS: u32 = 256;
const RECORD_SHIFT: u32 = 8;
const W_IBT: u32 = 16;
const W_MODE: u32 = 20;
const W_VMASK: u32 = 21;
const W_SMASK: u32 = 22;
const W_STICKY: u32 = 23;
const W_EXEC_PIPE: u32 = 24;
const W_GPU_PIPE: u32 = 26;
const W_IOTA: u32 = 28;
const W_IOTA_N: u32 = 30;
const W_BARRIER: u32 = 31;
const W_DISPATCH: u32 = 32;
const W_TG: u32 = 35;
const W_EXEC_TG: u32 = 38;
const W_SCRATCH: u32 = 41;
const W_VB: u32 = 48;
const W_SB: u32 = 172;
const MAX_BUFFERS: u32 = 31;
const KIND_DISPATCH: u32 = 4;
const MODE_DGC_DRAW: u32 = 1;

/// Every intrinsic family the knob lowers (declarations of them are dropped once no call is left).
pub(crate) const LOWERED: [&str; 19] = [
    "air.draw_primitives_render_command",
    "air.draw_indexed_primitives_render_command.p1i16",
    "air.draw_indexed_primitives_render_command.p1i32",
    "air.reset_render_command",
    "air.set_pipeline_state_render_command",
    "air.set_vertex_buffer_render_command.p1i8",
    "air.set_vertex_buffer_render_command.p2i8",
    "air.set_fragment_buffer_render_command.p1i8",
    "air.set_fragment_buffer_render_command.p2i8",
    "air.set_barrier_render_command",
    "air.clear_barrier_render_command",
    "air.reset_compute_command",
    "air.set_pipeline_state_compute_command",
    "air.set_kernel_buffer_compute_command.p1i8",
    "air.set_kernel_buffer_compute_command.p2i8",
    "air.set_barrier_compute_command",
    "air.clear_barrier_compute_command",
    "air.concurrent_dispatch_threadgroups_compute_command",
    "air.concurrent_dispatch_threads_compute_command",
];

#[cfg(test)]
thread_local! {
    /// Tests turn the knob on for their own thread (the environment is shared by the parallel tests).
    pub(crate) static FORCE: std::cell::Cell<Option<bool>> = const { std::cell::Cell::new(None) };
}

/// NVMTL_ICB_DGC=1 (exactly 1), read once: the plugin reads the same variable and lays the records out to match.
pub(crate) fn enabled() -> bool {
    #[cfg(test)]
    if let Some(v) = FORCE.with(|f| f.get()) {
        return v;
    }
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("NVMTL_ICB_DGC").is_ok_and(|v| v == "1"))
}

/// `%x = getelementptr [inbounds] T, ptr addrspace(1|2) %base, i64|i32 IDX` with a scalar T (buffers are any type here).
struct Gep {
    base: String,
    base_ty: String,
    idx_ty: String,
    idx: String,
    bytes: i64,
}

fn gep(body: &str) -> Option<(String, Gep)> {
    let (lhs, rhs) = body.trim().split_once(" = getelementptr ")?;
    let rhs = rhs.strip_prefix("inbounds ").unwrap_or(rhs);
    let rhs = rhs.strip_prefix("nuw ").unwrap_or(rhs);
    let parts: Vec<&str> = rhs.split(", ").collect();
    if parts.len() != 3 || !lhs.starts_with('%') {
        return None;
    }
    let scalar = |t: &str| match t {
        "i8" => Some(1),
        "i16" | "half" | "bfloat" => Some(2),
        "i32" | "float" => Some(4),
        "i64" | "double" => Some(8),
        _ => None,
    };
    let elem = parts[0].trim();
    // a vector element (`<2 x float>`: float2 + k): its allocation size (a 3-vector takes four elements, AIR's v96:128)
    let bytes = match elem.strip_prefix('<').and_then(|v| v.strip_suffix('>')) {
        Some(v) => {
            let (n, t) = v.split_once(" x ")?;
            let n: i64 = n.trim().parse().ok()?;
            if !(1..=4).contains(&n) {
                return None;
            }
            scalar(t.trim())? * if n == 3 { 4 } else { n }
        }
        None => scalar(elem)?,
    };
    let (base_ty, base) = ["ptr addrspace(1) ", "ptr addrspace(2) "]
        .iter()
        .find_map(|p| parts[1].trim().strip_prefix(p).map(|b| (p.trim_end(), b)))?;
    let (idx_ty, idx) = parts[2].trim().split_once(' ')?;
    if !base.starts_with('%') || !matches!(idx_ty, "i64" | "i32") || idx.contains(' ') {
        return None;
    }
    Some((
        lhs.to_string(),
        Gep { base: base.to_string(), base_ty: base_ty.to_string(), idx_ty: idx_ty.to_string(), idx: idx.to_string(), bytes },
    ))
}

/// `%x = bitcast ptr addrspace(N) %y to ptr addrspace(N)` (Metal's buffer setters pass their pointer through one)
fn alias(body: &str) -> Option<(String, String)> {
    let (lhs, rhs) = body.trim().split_once(" = bitcast ")?;
    let (from, to) = rhs.split_once(" to ")?;
    let (from_ty, src) = from.rsplit_once(' ')?;
    if !lhs.starts_with('%') || !src.starts_with('%') || from_ty.trim() != to.trim() || !from_ty.starts_with("ptr") {
        return None;
    }
    Some((lhs.to_string(), src.to_string()))
}

pub(crate) fn lower(san_ll: &str) -> Cow<'_, str> {
    if !LOWERED.iter().any(|name| san_ll.contains(&format!("@{name}("))) {
        return Cow::Borrowed(san_ll);
    }
    let mut out = String::with_capacity(san_ll.len() + 8192);
    let (mut serial, mut changed) = (0usize, false);
    let mut geps: HashMap<String, Gep> = HashMap::new();
    let mut aliases: HashMap<String, String> = HashMap::new();
    for line in san_ll.split_inclusive('\n') {
        let (body, nl) = match line.strip_suffix('\n') {
            Some(body) => (body, "\n"),
            None => (line, ""),
        };
        if body.starts_with("define ") {
            geps.clear();
            aliases.clear();
        } else if let Some((name, g)) = gep(body) {
            geps.insert(name, g);
        } else if let Some((name, src)) = alias(body) {
            aliases.insert(name, src);
        }
        match lower_line(body, serial, &geps, &aliases) {
            Some(lowered) => {
                out.push_str(&lowered);
                out.push_str(nl);
                serial += 1;
                changed = true;
            }
            None => out.push_str(line),
        }
    }
    if !changed {
        return Cow::Borrowed(san_ll);
    }
    Cow::Owned(super::drop_unused_declarations_of(out, &LOWERED))
}

#[derive(Clone, Copy, PartialEq)]
enum Shape {
    Draw,
    Indexed { shift: u32 },
    Reset,
    ResetCompute,
    Pipeline,
    VertexBuffer,
    StageBuffer,
    Barrier { render: bool, set: bool },
    Dispatch { threads: bool },
}

const CALLEES: [(&str, Shape); 19] = [
    ("@air.draw_primitives_render_command(", Shape::Draw),
    ("@air.draw_indexed_primitives_render_command.p1i16(", Shape::Indexed { shift: 1 }),
    ("@air.draw_indexed_primitives_render_command.p1i32(", Shape::Indexed { shift: 2 }),
    ("@air.reset_render_command(", Shape::Reset),
    ("@air.set_pipeline_state_render_command(", Shape::Pipeline),
    ("@air.set_vertex_buffer_render_command.p1i8(", Shape::VertexBuffer),
    ("@air.set_vertex_buffer_render_command.p2i8(", Shape::VertexBuffer),
    ("@air.set_fragment_buffer_render_command.p1i8(", Shape::StageBuffer),
    ("@air.set_fragment_buffer_render_command.p2i8(", Shape::StageBuffer),
    ("@air.set_barrier_render_command(", Shape::Barrier { render: true, set: true }),
    ("@air.clear_barrier_render_command(", Shape::Barrier { render: true, set: false }),
    ("@air.reset_compute_command(", Shape::ResetCompute),
    ("@air.set_pipeline_state_compute_command(", Shape::Pipeline),
    ("@air.set_kernel_buffer_compute_command.p1i8(", Shape::StageBuffer),
    ("@air.set_kernel_buffer_compute_command.p2i8(", Shape::StageBuffer),
    ("@air.set_barrier_compute_command(", Shape::Barrier { render: false, set: true }),
    ("@air.clear_barrier_compute_command(", Shape::Barrier { render: false, set: false }),
    ("@air.concurrent_dispatch_threadgroups_compute_command(", Shape::Dispatch { threads: false }),
    ("@air.concurrent_dispatch_threads_compute_command(", Shape::Dispatch { threads: true }),
];

/// The emitter with a pointer per record word (a word may be loaded and stored in one lowering) and fresh value names.
struct D<'a> {
    e: Emit,
    ptrs: HashMap<u32, String>,
    n: usize,
    geps: &'a HashMap<String, Gep>,
    aliases: &'a HashMap<String, String>,
}

impl D<'_> {
    fn line(&mut self, text: &str) {
        self.e.line(text);
    }
    fn fresh(&mut self, what: &str) -> String {
        self.n += 1;
        format!("%{}.{what}{}", self.e.prefix, self.n)
    }
    fn wptr(&mut self, word: u32) -> String {
        if let Some(p) = self.ptrs.get(&word) {
            return p.clone();
        }
        let (idx, ptr, base) = (self.e.name(&format!("i{word}")), self.e.name(&format!("w{word}")), self.e.name("base"));
        let (cb_ty, cb) = (self.e.cb_ty.clone(), self.e.cb.clone());
        self.line(&format!("{idx} = add i64 {base}, {word}"));
        self.line(&format!("{ptr} = getelementptr inbounds i32, {cb_ty} {cb}, i64 {idx}"));
        self.ptrs.insert(word, ptr.clone());
        ptr
    }
    /// The address of word `first + k` of the slot, `first` an i64 value (a dynamically indexed entry).
    fn dyn_ptr(&mut self, first: &str, k: u32) -> String {
        let (idx, ptr, base) = (self.fresh("di"), self.fresh("dw"), self.e.name("base"));
        let (cb_ty, cb) = (self.e.cb_ty.clone(), self.e.cb.clone());
        let at = self.fresh("dk");
        self.line(&format!("{at} = add i64 {first}, {k}"));
        self.line(&format!("{idx} = add i64 {base}, {at}"));
        self.line(&format!("{ptr} = getelementptr inbounds i32, {cb_ty} {cb}, i64 {idx}"));
        ptr
    }
    fn load(&mut self, word: u32) -> String {
        let ptr = self.wptr(word);
        self.load_at(&ptr, &format!("r{word}_"))
    }
    fn load_at(&mut self, ptr: &str, what: &str) -> String {
        let v = self.fresh(what);
        let ty = self.e.cb_ty.clone();
        self.line(&format!("{v} = load i32, {ty} {ptr}, align 4"));
        v
    }
    fn store(&mut self, word: u32, value: &str) {
        let ptr = self.wptr(word);
        self.store_at(&ptr, value);
    }
    fn store_at(&mut self, ptr: &str, value: &str) {
        let ty = self.e.cb_ty.clone();
        self.line(&format!("store i32 {value}, {ty} {ptr}, align 4"));
    }
    fn op(&mut self, what: &str, rhs: &str) -> String {
        let v = self.fresh(what);
        self.line(&format!("{v} = {rhs}"));
        v
    }
    fn and_all(&mut self, terms: &[String]) -> String {
        let mut acc = terms[0].clone();
        for t in &terms[1..] {
            acc = self.op("ok", &format!("and i1 {acc}, {t}"));
        }
        acc
    }
    fn sel(&mut self, ok: &str, a: &str, b: &str) -> String {
        self.op("s", &format!("select i1 {ok}, i32 {a}, i32 {b}"))
    }
    /// The device address of a pointer argument as an i64: the address of `p` plus k elements for `p + k`, through the
    /// no-op bitcasts Metal's setters add (`%nvicbN.addr`: the emitter gives its source an address payload).
    fn address(&mut self, ty: &str, value: &str) -> String {
        let mut v = value.to_string();
        for _ in 0..8 {
            match self.aliases.get(&v) {
                Some(src) => v = src.clone(),
                None => break,
            }
        }
        let (addr, paddr) = (self.e.name("addr"), self.e.name("paddr"));
        let ty = if ty.starts_with("ptr") || ty.ends_with('*') { ty.to_string() } else { "ptr addrspace(1)".to_string() };
        match self.geps.get(&v) {
            Some(g) if ty.starts_with("ptr") => {
                let (base_ty, base, idx_ty, idx, bytes) = (g.base_ty.clone(), g.base.clone(), g.idx_ty.clone(), g.idx.clone(), g.bytes);
                self.line(&format!("{paddr} = ptrtoint {base_ty} {base} to i64"));
                if let Ok(k) = idx.parse::<i64>() {
                    self.line(&format!("{addr} = add i64 {paddr}, {}", k * bytes));
                } else {
                    let idx = if idx_ty == "i32" { self.op("pidx", &format!("sext i32 {idx} to i64")) } else { idx };
                    let off = self.op("poff", &format!("mul i64 {idx}, {bytes}"));
                    self.line(&format!("{addr} = add i64 {paddr}, {off}"));
                }
            }
            _ => self.line(&format!("{addr} = ptrtoint {ty} {v} to i64")),
        }
        addr
    }
    fn lo_hi(&mut self, v64: &str) -> (String, String) {
        let lo = self.op("lo", &format!("trunc i64 {v64} to i32"));
        let sh = self.op("sh", &format!("lshr i64 {v64}, 32"));
        let hi = self.op("hi", &format!("trunc i64 {sh} to i32"));
        (lo, hi)
    }
    fn u64_of(&mut self, lo: &str, hi: &str) -> String {
        let l = self.op("l64", &format!("zext i32 {lo} to i64"));
        let h = self.op("h64", &format!("zext i32 {hi} to i64"));
        let hs = self.op("hs", &format!("shl i64 {h}, 32"));
        self.op("u64", &format!("or i64 {hs}, {l}"))
    }
    /// Refuses the slot unless `ok`: kind 3, zero counts (both draw forms and the dispatch), the sticky word set.
    fn refuse_unless(&mut self, ok: &str) {
        let st = self.load(W_STICKY);
        let st = self.sel(ok, &st, "1");
        self.store(W_STICKY, &st);
        let k = self.load(W_KIND);
        let k = self.sel(ok, &k, &KIND_REFUSED.to_string());
        self.store(W_KIND, &k);
        for w in [W_IDX, W_IDX + 1, W_DRAW, W_DRAW + 1, W_DISPATCH, W_DISPATCH + 1, W_DISPATCH + 2] {
            let v = self.load(w);
            let v = self.sel(ok, &v, "0");
            self.store(w, &v);
        }
    }
    fn refuse(&mut self) {
        self.store(W_STICKY, "1");
        self.store(W_KIND, &KIND_REFUSED.to_string());
        for w in [W_IDX, W_IDX + 1, W_DRAW, W_DRAW + 1, W_DISPATCH, W_DISPATCH + 1, W_DISPATCH + 2] {
            self.store(w, "0");
        }
    }
    /// The three i32 values of a `<3 x i32>` argument (an SSA vector, a constant vector, zeroinitializer).
    fn vec3(&mut self, ty: &str, value: &str) -> Option<[String; 3]> {
        if ty.replace(' ', "") != "<3xi32>" {
            return None;
        }
        if value == "zeroinitializer" {
            return Some(["0".into(), "0".into(), "0".into()]);
        }
        if let Some(inner) = value.strip_prefix('<').and_then(|v| v.strip_suffix('>')) {
            let el: Vec<String> = inner.split(',').map(|e| e.trim().strip_prefix("i32 ").map(str::to_string)).collect::<Option<_>>()?;
            return <[String; 3]>::try_from(el).ok();
        }
        if !value.starts_with('%') {
            return None;
        }
        let mut out: [String; 3] = Default::default();
        for (k, slot) in out.iter_mut().enumerate() {
            *slot = self.op("el", &format!("extractelement <3 x i32> {value}, i64 {k}"));
        }
        Some(out)
    }
}

fn lower_line(body: &str, serial: usize, geps: &HashMap<String, Gep>, aliases: &HashMap<String, String>) -> Option<String> {
    let (callee, shape) = CALLEES.iter().find(|(c, _)| body.contains(c)).copied()?;
    let (indent, args) = call_arguments(body, callee)?;
    if args.len() < 2 {
        return None;
    }
    let (cb_ty, cb) = args[0].clone();
    if !cb_ty.contains("addrspace(1)") || args[1].0 != "i32" {
        return None;
    }
    let is_i32 = |k: usize| args.get(k).is_some_and(|(t, _)| t == "i32");
    let is_ptr = |k: usize| args.get(k).is_some_and(|(t, _)| t.contains("addrspace(1)") || t.contains("addrspace(2)"));
    let fits = match shape {
        Shape::Draw => args.len() == 7 && (2..7).all(is_i32),
        Shape::Indexed { .. } => args.len() == 8 && is_i32(2) && is_i32(3) && args[4].0.contains("addrspace(1)") && (5..8).all(is_i32),
        Shape::Reset | Shape::ResetCompute | Shape::Barrier { .. } => args.len() == 2,
        Shape::Pipeline => args.len() == 3 && is_ptr(2),
        // (cb, slot, buffer, [i64 stride,] i32 index)
        Shape::VertexBuffer | Shape::StageBuffer => {
            is_ptr(2) && ((args.len() == 4 && is_i32(3)) || (args.len() == 5 && args[3].0 == "i64" && is_i32(4)))
        }
        // (cb, slot, <3 x i32> grid, <3 x i32> tg) or six i32
        Shape::Dispatch { .. } => args.len() == 4 || (args.len() == 8 && (2..8).all(is_i32)),
    };
    if !fits {
        return None;
    }
    let mut d = D {
        e: Emit {
            out: String::new(),
            indent,
            prefix: format!("nvicb{serial}"),
            cb_ty: if cb_ty.starts_with("ptr") { "ptr addrspace(1)".to_string() } else { cb_ty },
            cb,
        },
        ptrs: HashMap::new(),
        n: 0,
        geps,
        aliases,
    };
    if !d.e.cb_ty.starts_with("ptr") {
        let cast = d.e.name("cb");
        let (ty, cb) = (d.e.cb_ty.clone(), d.e.cb.clone());
        d.line(&format!("{cast} = bitcast {ty} {cb} to i32 addrspace(1)*"));
        d.e.cb_ty = "i32 addrspace(1)*".to_string();
        d.e.cb = cast;
    }
    // vectors are read before anything is emitted, so a form this pass does not know leaves the line untouched
    let dispatch = match shape {
        Shape::Dispatch { .. } if args.len() == 4 => {
            let g = d.vec3(&args[2].0, &args[2].1)?;
            let t = d.vec3(&args[3].0, &args[3].1)?;
            Some((g, t))
        }
        Shape::Dispatch { .. } => {
            let v = |k: usize| args[k].1.clone();
            Some(([v(2), v(3), v(4)], [v(5), v(6), v(7)]))
        }
        _ => None,
    };
    let (slot64, base) = (d.e.name("slot"), d.e.name("base"));
    let slot = args[1].1.clone();
    d.line(&format!("{slot64} = zext i32 {slot} to i64"));
    d.line(&format!("{base} = shl i64 {slot64}, {RECORD_SHIFT}"));
    let _ = RECORD_WORDS;
    match shape {
        Shape::Reset => {
            d.store(W_KIND, &KIND_EMPTY.to_string());
            for w in [W_IDX, W_IDX + 1, W_DRAW, W_DRAW + 1, W_STICKY, W_BARRIER] {
                d.store(w, "0");
            }
        }
        Shape::ResetCompute => {
            d.store(W_KIND, &KIND_EMPTY.to_string());
            for w in [W_DISPATCH, W_DISPATCH + 1, W_DISPATCH + 2, W_STICKY, W_BARRIER] {
                d.store(w, "0");
            }
        }
        Shape::Draw => {
            let a = |k: usize| args[k].1.clone();
            let (prim, start, count, instances, base_instance) = (a(2), a(3), a(4), a(5), a(6));
            let exec = d.load(W_EXEC_PRIM);
            let shift = d.load(W_IB_SHIFT);
            let mode = d.load(W_MODE);
            let sticky = d.load(W_STICKY);
            let iota_n = d.load(W_IOTA_N);
            let iota_lo = d.load(W_IOTA);
            let iota_hi = d.load(W_IOTA + 1);
            let primok = d.op("primok", &format!("icmp eq i32 {exec}, {prim}"));
            let mb = d.op("mb", &format!("and i32 {mode}, {MODE_DGC_DRAW}"));
            let dgcm = d.op("dgcm", &format!("icmp ne i32 {mb}, 0"));
            let noib = d.op("noib", &format!("icmp eq i32 {shift}, 0"));
            let okc = d.and_all(&[primok.clone(), noib]);
            // DGC: the draw goes through the identity index buffer, which must hold first vertex + count indices
            let s64 = d.op("s64", &format!("zext i32 {start} to i64"));
            let c64 = d.op("c64", &format!("zext i32 {count} to i64"));
            let end = d.op("end", &format!("add i64 {s64}, {c64}"));
            let n64 = d.op("n64", &format!("zext i32 {iota_n} to i64"));
            let inside = d.op("fits", &format!("icmp ule i64 {end}, {n64}"));
            let okd = d.and_all(&[primok, inside]);
            let ok0 = d.op("ok", &format!("select i1 {dgcm}, i1 {okd}, i1 {okc}"));
            let fresh = d.op("fresh", &format!("icmp eq i32 {sticky}, 0"));
            let ok = d.and_all(&[ok0, fresh]);
            let kind = d.sel(&ok, &KIND_DRAW.to_string(), &KIND_REFUSED.to_string());
            let cnt = d.sel(&ok, &count, "0");
            let ins = d.sel(&ok, &instances, "0");
            let icnt = d.sel(&dgcm, &cnt, "0");
            let iins = d.sel(&dgcm, &ins, "0");
            let nbytes = d.op("nb", &format!("shl i32 {iota_n}, 2"));
            d.store(W_KIND, &kind);
            d.store(W_PRIM, &prim);
            for (w, v) in [(0, icnt), (1, iins), (2, start.clone()), (3, "0".to_string()), (4, base_instance.clone())] {
                d.store(W_IDX + w, &v);
            }
            for (w, v) in [(0, cnt), (1, ins), (2, start), (3, base_instance)] {
                d.store(W_DRAW + w, &v);
            }
            d.store(W_IBT, &iota_lo);
            d.store(W_IBT + 1, &iota_hi);
            d.store(W_IBT + 2, &nbytes);
            d.store(W_IBT + 3, "1"); // VK_INDEX_TYPE_UINT32
        }
        Shape::Indexed { shift } => {
            let a = |k: usize| args[k].1.clone();
            let (prim, count, instances, base_vertex, base_instance) = (a(2), a(3), a(5), a(6), a(7));
            let (ib_ty, ib) = args[4].clone();
            let exec = d.load(W_EXEC_PRIM);
            let lo = d.load(W_IB_LO);
            let hi = d.load(W_IB_HI);
            let limit = d.load(W_IB_LIMIT);
            let have_shift = d.load(W_IB_SHIFT);
            let mode = d.load(W_MODE);
            let sticky = d.load(W_STICKY);
            let addr = d.address(&ib_ty, &ib);
            // the classic checks (render_command.rs): inside the index buffer the CPU bound, of its type, aligned
            let bound = d.u64_of(&lo, &hi);
            let limit64 = d.op("limit64", &format!("zext i32 {limit} to i64"));
            let delta = d.op("delta", &format!("sub i64 {addr}, {bound}"));
            let primok = d.op("primok", &format!("icmp eq i32 {exec}, {prim}"));
            let typeok = d.op("typeok", &format!("icmp eq i32 {have_shift}, {shift}"));
            let haveib = d.op("haveib", &format!("icmp ne i64 {bound}, 0"));
            let above = d.op("above", &format!("icmp uge i64 {addr}, {bound}"));
            let inside = d.op("inside", &format!("icmp ule i64 {delta}, {limit64}"));
            let mis = d.op("misalign", &format!("and i64 {delta}, {}", (1u32 << shift) - 1));
            let aligned = d.op("aligned", &format!("icmp eq i64 {mis}, 0"));
            let okc = d.and_all(&[primok.clone(), typeok, haveib, above, inside, aligned]);
            let first64 = d.op("first64", &format!("lshr i64 {delta}, {shift}"));
            let first32 = d.op("first32", &format!("trunc i64 {first64} to i32"));
            // DGC: any index buffer by its address (non-zero, index-aligned), bound through the index buffer token
            let mb = d.op("mb", &format!("and i32 {mode}, {MODE_DGC_DRAW}"));
            let dgcm = d.op("dgcm", &format!("icmp ne i32 {mb}, 0"));
            let nz = d.op("nz", &format!("icmp ne i64 {addr}, 0"));
            let amis = d.op("amis", &format!("and i64 {addr}, {}", (1u32 << shift) - 1));
            let aal = d.op("aal", &format!("icmp eq i64 {amis}, 0"));
            let okd = d.and_all(&[primok, nz, aal]);
            let ok0 = d.op("ok", &format!("select i1 {dgcm}, i1 {okd}, i1 {okc}"));
            let fresh = d.op("fresh", &format!("icmp eq i32 {sticky}, 0"));
            let ok = d.and_all(&[ok0, fresh]);
            let kind = d.sel(&ok, &KIND_INDEXED.to_string(), &KIND_REFUSED.to_string());
            let cnt = d.sel(&ok, &count, "0");
            let ins = d.sel(&ok, &instances, "0");
            let fst = d.sel(&ok, &first32, "0");
            let first = d.sel(&dgcm, "0", &fst);
            let (alo, ahi) = d.lo_hi(&addr);
            let c64 = d.op("c64", &format!("zext i32 {count} to i64"));
            let nb = d.op("nb", &format!("shl i64 {c64}, {shift}"));
            let big = d.op("big", &format!("icmp ugt i64 {nb}, 4294967295"));
            let nbt = d.op("nbt", &format!("trunc i64 {nb} to i32"));
            let nb32 = d.sel(&big, "-1", &nbt);
            d.store(W_KIND, &kind);
            d.store(W_PRIM, &prim);
            for (w, v) in [(0, cnt), (1, ins), (2, first), (3, base_vertex), (4, base_instance)] {
                d.store(W_IDX + w, &v);
            }
            d.store(W_DRAW, "0");
            d.store(W_DRAW + 1, "0");
            d.store(W_IBT, &alo);
            d.store(W_IBT + 1, &ahi);
            d.store(W_IBT + 2, &nb32);
            d.store(W_IBT + 3, if shift == 2 { "1" } else { "0" }); // VK_INDEX_TYPE_UINT32 / UINT16
        }
        Shape::Pipeline => {
            let (ty, value) = args[2].clone();
            let id = d.address(&ty, &value);
            let elo = d.load(W_EXEC_PIPE);
            let ehi = d.load(W_EXEC_PIPE + 1);
            let exec = d.u64_of(&elo, &ehi);
            let same = d.op("same", &format!("icmp eq i64 {id}, {exec}"));
            let known = d.op("known", &format!("icmp ne i64 {exec}, 0"));
            let ok = d.and_all(&[same, known]);
            let (lo, hi) = d.lo_hi(&id);
            d.store(W_GPU_PIPE, &lo);
            d.store(W_GPU_PIPE + 1, &hi);
            d.refuse_unless(&ok);
        }
        Shape::VertexBuffer | Shape::StageBuffer => {
            let vertex = shape == Shape::VertexBuffer;
            let (ty, value) = args[2].clone();
            let index = args[args.len() - 1].1.clone();
            let stride = (args.len() == 5).then(|| args[3].1.clone()).filter(|s| s != "-1");
            let mask = d.load(if vertex { W_VMASK } else { W_SMASK });
            let inr = d.op("inr", &format!("icmp ult i32 {index}, {MAX_BUFFERS}"));
            let sidx = d.sel(&inr, &index, "0");
            let shr = d.op("shr", &format!("lshr i32 {mask}, {sidx}"));
            let bit = d.op("bit", &format!("and i32 {shr}, 1"));
            let offered = d.op("offered", &format!("icmp ne i32 {bit}, 0"));
            let addr = d.address(&ty, &value);
            let nz = d.op("nz", &format!("icmp ne i64 {addr}, 0"));
            let ok = d.and_all(&[inr, offered, nz]);
            // the entry's first word; a refused store lands in the scratch words
            let (per, first) = if vertex { (4, W_VB) } else { (2, W_SB) };
            let s64 = d.op("e64", &format!("zext i32 {sidx} to i64"));
            let ew = d.op("ew", &format!("mul i64 {s64}, {per}"));
            let ew = d.op("ew", &format!("add i64 {ew}, {first}"));
            let at = d.op("at", &format!("select i1 {ok}, i64 {ew}, i64 {W_SCRATCH}"));
            let (lo, hi) = d.lo_hi(&addr);
            let p0 = d.dyn_ptr(&at, 0);
            d.store_at(&p0, &lo);
            let p1 = d.dyn_ptr(&at, 1);
            d.store_at(&p1, &hi);
            if vertex {
                // the size a shader cannot know: the whole range (the app answers for the reads, as on Apple's GPUs)
                let p2 = d.dyn_ptr(&at, 2);
                d.store_at(&p2, "-1");
                if let Some(stride) = stride {
                    // set_vertex_buffer(buffer, stride, index): a dynamic-stride layout's stride; -1 keeps the CPU's
                    let p3 = d.dyn_ptr(&at, 3);
                    let old = d.load_at(&p3, "ost");
                    let keep = d.op("keep", &format!("icmp eq i64 {stride}, -1"));
                    let s32 = d.op("st", &format!("trunc i64 {stride} to i32"));
                    let v = d.sel(&keep, &old, &s32);
                    d.store_at(&p3, &v);
                }
            }
            d.refuse_unless(&ok);
        }
        Shape::Barrier { render, set } => {
            d.store(W_BARRIER, if set { "1" } else { "0" });
            // a render barrier inside a pass between two DGC sequences has no Vulkan form the execute can choose from
            // GPU memory: refused. Compute slots are each dispatched behind a full barrier, which honours any barrier.
            if render && set {
                d.refuse();
            }
        }
        Shape::Dispatch { threads } => {
            let (g, t) = dispatch?;
            let e: Vec<String> = (0..3).map(|k| d.load(W_EXEC_TG + k)).collect();
            let sticky = d.load(W_STICKY);
            for k in 0..3 {
                d.store(W_TG + k as u32, &t[k]);
            }
            let mut terms = Vec::new();
            for k in 0..3 {
                terms.push(d.op("tgeq", &format!("icmp eq i32 {}, {}", t[k], e[k])));
            }
            terms.push(d.op("known", &format!("icmp ne i32 {}, 0", e[0])));
            terms.push(d.op("fresh", &format!("icmp eq i32 {sticky}, 0")));
            let mut grid = g.clone();
            if threads {
                // only a grid the threadgroups divide exactly: a ragged edge would run threads Metal clips
                for k in 0..3 {
                    let tz = d.op("tz", &format!("icmp eq i32 {}, 0", t[k]));
                    let div = d.sel(&tz, "1", &t[k]);
                    grid[k] = d.op("q", &format!("udiv i32 {}, {div}", g[k]));
                    let r = d.op("r", &format!("urem i32 {}, {div}", g[k]));
                    terms.push(d.op("exact", &format!("icmp eq i32 {r}, 0")));
                    terms.push(d.op("tnz", &format!("icmp ne i32 {}, 0", t[k])));
                }
            }
            let ok = d.and_all(&terms);
            let kind = d.sel(&ok, &KIND_DISPATCH.to_string(), &KIND_REFUSED.to_string());
            d.store(W_KIND, &kind);
            for k in 0..3 {
                let v = d.sel(&ok, &grid[k], "0");
                d.store(W_DISPATCH + k as u32, &v);
            }
        }
    }
    Some(d.e.out.trim_end_matches('\n').to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn on<R>(f: impl FnOnce() -> R) -> R {
        FORCE.with(|v| v.set(Some(true)));
        let r = f();
        FORCE.with(|v| v.set(None));
        r
    }

    const CB: &str = "ptr addrspace(1) captures(none) %cb, i32 %t";

    #[test]
    fn the_knob_off_leaves_the_state_commands_for_the_refusal() {
        let text = format!("  tail call void @air.set_vertex_buffer_render_command.p1i8({CB}, ptr addrspace(1) %0, i64 -1, i32 3) #2\n");
        FORCE.with(|v| v.set(Some(false)));
        assert!(matches!(super::super::lower_render_commands(&text), Cow::Borrowed(_)));
        FORCE.with(|v| v.set(None));
    }

    #[test]
    fn records_are_1024_bytes_with_the_knob() {
        let text = format!("  tail call void @air.reset_render_command({CB}) #2\n");
        let out = on(|| super::super::lower_render_commands(&text).into_owned());
        assert!(out.contains("%nvicb0.base = shl i64 %nvicb0.slot, 8"), "{out}");
        assert!(out.contains("store i32 0, ptr addrspace(1) %nvicb0.w23"), "the reset clears the sticky refusal: {out}");
    }

    #[test]
    fn an_indexed_draw_takes_its_index_buffer_by_address_in_dgc_mode() {
        let text = format!(
            "define void @f() {{\n  %add.ptr = getelementptr inbounds i16, ptr addrspace(1) %ib, i64 3\n  tail call void @air.draw_indexed_primitives_render_command.p1i16({CB}, i32 3, i32 6, ptr addrspace(1) %add.ptr, i32 1, i32 4, i32 2) #2\n  ret void\n}}\n"
        );
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.paddr = ptrtoint ptr addrspace(1) %ib to i64"), "{out}");
        assert!(out.contains("%nvicb0.addr = add i64 %nvicb0.paddr, 6"), "{out}");
        assert!(out.contains("and i64 %nvicb0.addr, 1"), "index-aligned address: {out}");
        assert!(out.contains("select i1 %nvicb0.dgcm"), "{out}");
        assert!(out.contains("store i32 0, ptr addrspace(1) %nvicb0.w19"), "uint16 index type in the token: {out}");
        assert!(out.contains("%nvicb0.w16"), "{out}");
        assert!(!out.contains("@air.draw_indexed"), "{out}");
    }

    #[test]
    fn a_plain_draw_goes_through_the_identity_index_buffer() {
        let text = format!("  tail call void @air.draw_primitives_render_command({CB}, i32 3, i32 16, i32 6, i32 1, i32 0) #2\n");
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.w28"), "{out}");
        assert!(out.contains("store i32 1, ptr addrspace(1) %nvicb0.w19"), "{out}");
        assert!(out.contains("icmp ule i64"), "first vertex + count inside the identity buffer: {out}");
        assert!(out.contains("store i32 16, ptr addrspace(1) %nvicb0.w4"), "first index = first vertex: {out}");
    }

    #[test]
    fn a_vertex_buffer_is_checked_against_the_offered_mask_and_stored_by_index() {
        let text = format!(
            "define void @f() {{\n  %0 = bitcast ptr addrspace(1) %vb to ptr addrspace(1)\n  tail call void @air.set_vertex_buffer_render_command.p1i8({CB}, ptr addrspace(1) %0, i64 -1, i32 3) #2\n  ret void\n}}\n"
        );
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.addr = ptrtoint ptr addrspace(1) %vb to i64"), "through the bitcast: {out}");
        assert!(out.contains("load i32, ptr addrspace(1) %nvicb0.w21"), "{out}");
        assert!(out.contains("icmp ult i32 3, 31"), "{out}");
        assert!(out.contains("mul i64") && out.contains(", 48"), "{out}");
        assert!(out.contains("i64 41"), "a refused store goes to scratch: {out}");
        assert!(!out.contains("ost"), "stride -1: the CPU's stride stays: {out}");
        let dynamic = text.replace("i64 -1, i32 3", "i64 %s, i32 3");
        let out = on(|| lower(&dynamic).into_owned());
        assert!(out.contains("icmp eq i64 %s, -1"), "{out}");
    }

    #[test]
    fn a_fragment_buffer_through_a_float_gep_is_addressed_from_its_base() {
        let text = format!(
            "define void @f() {{\n  %add.ptr = getelementptr inbounds float, ptr addrspace(1) %vb, i64 4\n  %1 = bitcast ptr addrspace(1) %add.ptr to ptr addrspace(1)\n  tail call void @air.set_fragment_buffer_render_command.p1i8({CB}, ptr addrspace(1) %1, i32 5) #2\n  ret void\n}}\n"
        );
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.addr = add i64 %nvicb0.paddr, 16"), "{out}");
        assert!(out.contains("load i32, ptr addrspace(1) %nvicb0.w22"), "{out}");
        assert!(out.contains(", 172"), "{out}");
    }

    #[test]
    fn a_vector_element_pointer_plus_k_counts_whole_vectors() {
        let text = format!(
            "define void @f() {{\n  %add.ptr = getelementptr inbounds <2 x float>, ptr addrspace(1) %0, i64 6\n  %a3 = getelementptr inbounds <3 x float>, ptr addrspace(1) %1, i64 2\n  tail call void @air.set_vertex_buffer_render_command.p1i8({CB}, ptr addrspace(1) %add.ptr, i64 -1, i32 0) #2\n  tail call void @air.set_vertex_buffer_render_command.p1i8({CB}, ptr addrspace(1) %a3, i64 -1, i32 1) #2\n  ret void\n}}\n"
        );
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.addr = add i64 %nvicb0.paddr, 48"), "{out}");
        assert!(out.contains("%nvicb1.addr = add i64 %nvicb1.paddr, 32"), "{out}");
    }

    #[test]
    fn a_pipeline_is_accepted_only_as_the_one_the_execute_binds() {
        let text = format!("  tail call void @air.set_pipeline_state_render_command({CB}, ptr addrspace(1) readonly %pso) #2\n");
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.addr = ptrtoint ptr addrspace(1) %pso to i64"), "{out}");
        assert!(out.contains("%nvicb0.w24") && out.contains("%nvicb0.w25"), "{out}");
        assert!(out.contains("store i32 %nvicb0.lo"), "{out}");
        assert!(out.contains("%nvicb0.w26"), "{out}");
    }

    #[test]
    fn a_render_barrier_refuses_and_a_cleared_one_does_not() {
        let set = format!("  tail call void @air.set_barrier_render_command({CB}) #2\n");
        let out = on(|| lower(&set).into_owned());
        assert!(out.contains("store i32 3, ptr addrspace(1) %nvicb0.w0"), "{out}");
        let clear = format!("  tail call void @air.clear_barrier_render_command({CB}) #2\n");
        let out = on(|| lower(&clear).into_owned());
        assert!(!out.contains("store i32 3"), "{out}");
        let cset = format!("  tail call void @air.set_barrier_compute_command({CB}) #2\n");
        let out = on(|| lower(&cset).into_owned());
        assert!(out.contains("store i32 1, ptr addrspace(1) %nvicb0.w31") && !out.contains("store i32 3"), "{out}");
    }

    #[test]
    fn a_dispatch_checks_the_threadgroup_size_and_writes_the_grid() {
        let vec = format!("  tail call void @air.concurrent_dispatch_threadgroups_compute_command({CB}, <3 x i32> %g, <3 x i32> <i32 64, i32 1, i32 1>) #2\n");
        let out = on(|| lower(&vec).into_owned());
        assert!(out.contains("extractelement <3 x i32> %g, i64 2"), "{out}");
        assert!(out.contains("store i32 64, ptr addrspace(1) %nvicb0.w35"), "{out}");
        assert!(out.contains("icmp eq i32 64, %nvicb0.r38_"), "{out}");
        assert!(out.contains("%nvicb0.w32") && out.contains("%nvicb0.w34"), "{out}");
        let scalars = format!("  tail call void @air.concurrent_dispatch_threads_compute_command({CB}, i32 %x, i32 1, i32 1, i32 32, i32 1, i32 1) #2\n");
        let out = on(|| lower(&scalars).into_owned());
        assert!(out.contains("udiv i32 %x,") && out.contains("urem i32 %x,"), "{out}");
        let unknown = format!("  tail call void @air.concurrent_dispatch_threadgroups_compute_command({CB}, <3 x i64> %g, <3 x i64> %h) #2\n");
        assert!(matches!(on(|| lower(&unknown)), Cow::Borrowed(_)));
    }

    #[test]
    fn a_kernel_buffer_in_constant_space_is_offered_through_the_stage_mask() {
        let text = format!("  tail call void @air.set_kernel_buffer_compute_command.p2i8({CB}, ptr addrspace(2) %k, i64 -1, i32 2) #2\ndeclare void @air.set_kernel_buffer_compute_command.p2i8(ptr addrspace(1), i32, ptr addrspace(2), i64, i32)\n");
        let out = on(|| lower(&text).into_owned());
        assert!(out.contains("%nvicb0.addr = ptrtoint ptr addrspace(2) %k to i64"), "{out}");
        assert!(out.contains("%nvicb0.w22"), "{out}");
        assert!(!out.contains("declare void @air.set_kernel_buffer"), "{out}");
    }

    #[test]
    fn word_pointers_are_made_once_per_lowering() {
        let text = format!("  tail call void @air.set_pipeline_state_compute_command({CB}, ptr addrspace(1) %p) #2\n");
        let out = on(|| lower(&text).into_owned());
        assert_eq!(out.matches("%nvicb0.w23 = ").count(), 1, "{out}");
        assert_eq!(out.matches("%nvicb0.w0 = ").count(), 1, "{out}");
    }
}
