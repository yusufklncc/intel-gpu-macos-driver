//! GPU-encoded indirect command buffers: Metal's `render_command` writes (`air.*_render_command`) lowered to plain
//! stores into the slot records the Metal plugin keeps for an MTLIndirectCommandBuffer (plugin/nvmtl_icb_layout.h,
//! which must stay in step with the constants here).
//!
//! The `command_buffer` a shader receives (an argument-buffer field Apple's argument encoder fills with
//! `setIndirectCommandBuffer:atIndex:`, or `gpuResourceID`) is, in the plugin, the device address of that ICB's record
//! array: one 64-byte record per command slot, sixteen 32-bit words:
//!
//! | word  | written by | meaning                                                                 |
//! |-------|------------|-------------------------------------------------------------------------|
//! | 0     | CPU + GPU  | kind: 0 empty, 1 draw, 2 indexed draw, 3 refused (see below)            |
//! | 1     | CPU + GPU  | Metal primitive type of the last draw written                           |
//! | 2..6  | CPU + GPU  | VkDrawIndexedIndirectCommand (count, instances, first index, base vertex, base instance) |
//! | 7..10 | CPU + GPU  | VkDrawIndirectCommand (count, instances, first vertex, base instance)   |
//! | 11    | CPU        | the primitive type the execute will draw this slot with (~0: none)      |
//! | 12,13 | CPU        | device address the execute binds as this slot's index buffer (0: none)  |
//! | 14    | CPU        | bytes from that address to the end of the buffer                        |
//! | 15    | CPU        | log2 of that index buffer's element size (1: uint16, 2: uint32; 0: none) |
//!
//! The execute (`executeCommandsInBuffer:withRange:`) issues one indirect draw per slot, with the topology of word 11:
//! an indexed one from words 2..6 when the slot has an index buffer (word 15 non-zero), else a plain one from words
//! 7..10. (It issued both until 11 Nov 2026: Xe2 ANV loads an indirect draw's parameters one MI_LOAD_REGISTER_MEM at a
//! time, ~2.2 us per draw from VRAM, and the second, zero-count draw doubled an indexed slot's cost; mtl_icb_cost.)
//! A write zeroes the counts of the form it does not use. Vulkan cannot take an index buffer address or a topology from
//! GPU memory (core Vulkan; VK_EXT_device_generated_commands could, see docs/ROADMAP.md), so a GPU-written draw the
//! execute cannot express - an index pointer outside the slot's bound index buffer, a different index type, a
//! misaligned pointer, a plain draw into a slot with an index buffer, or a primitive type other than word 11 - is
//! written with zero counts and kind 3: nothing is drawn rather than something wrong, and the plugin can report the slot.
use std::borrow::Cow;

pub(crate) mod dgc;

/// Bytes per command record (a power of two: the slot index is shifted by RECORD_SHIFT).
pub(crate) const RECORD_WORDS: u32 = 16;
const RECORD_SHIFT: u32 = 4; // words: slot * 16
const W_KIND: u32 = 0;
const W_PRIM: u32 = 1;
const W_IDX: u32 = 2;
const W_DRAW: u32 = 7;
const W_EXEC_PRIM: u32 = 11;
const W_IB_LO: u32 = 12;
const W_IB_HI: u32 = 13;
const W_IB_LIMIT: u32 = 14;
const W_IB_SHIFT: u32 = 15;
const KIND_EMPTY: u32 = 0;
const KIND_DRAW: u32 = 1;
const KIND_INDEXED: u32 = 2;
const KIND_REFUSED: u32 = 3;

const DRAW: &str = "@air.draw_primitives_render_command(";
const DRAW_INDEXED16: &str = "@air.draw_indexed_primitives_render_command.p1i16(";
const DRAW_INDEXED32: &str = "@air.draw_indexed_primitives_render_command.p1i32(";
const RESET: &str = "@air.reset_render_command(";

/// The intrinsic families this pass lowers (declarations of them are dropped once no call is left).
pub(crate) const LOWERED: [&str; 4] = [
    "air.draw_primitives_render_command",
    "air.draw_indexed_primitives_render_command.p1i16",
    "air.draw_indexed_primitives_render_command.p1i32",
    "air.reset_render_command",
];

/// The ptrtoint results this pass emits for an index pointer's device address (`%nvicbN.addr`, `%nvicbN.paddr`): the
/// emitter gives their sources an address payload (a loaded pointer, an entry buffer parameter) instead of the
/// placeholder 0 it uses for other ptrtoints.
pub(crate) fn is_address_name(name: &str) -> bool {
    let name = name.strip_prefix('%').unwrap_or(name);
    name.starts_with("nvicb") && (name.ends_with(".addr") || name.ends_with(".paddr"))
}

/// `%x = getelementptr [inbounds] iN, ptr addrspace(1) %base, i64|i32 IDX` in the current function: an index pointer
/// formed as `p + k` is addressed as the address of `p` plus k elements (the address of a GEP result has no payload of
/// its own in the emitter).
#[derive(Clone)]
struct ElementGep {
    base: String,
    idx_ty: String,
    idx: String,
    bytes: u32,
}

fn element_gep(body: &str) -> Option<(String, ElementGep)> {
    let (lhs, rhs) = body.trim().split_once(" = getelementptr ")?;
    let rhs = rhs.strip_prefix("inbounds ").unwrap_or(rhs);
    let rhs = rhs.strip_prefix("nuw ").unwrap_or(rhs);
    let parts: Vec<&str> = rhs.split(", ").collect();
    if parts.len() != 3 || !lhs.starts_with('%') {
        return None;
    }
    let bytes = match parts[0].trim() {
        "i8" => 1,
        "i16" => 2,
        "i32" => 4,
        "i64" => 8,
        _ => return None,
    };
    let base = parts[1].trim().strip_prefix("ptr addrspace(1) ")?;
    let (idx_ty, idx) = parts[2].trim().split_once(' ')?;
    if !base.starts_with('%') || !matches!(idx_ty, "i64" | "i32") || idx.contains(' ') {
        return None;
    }
    Some((lhs.to_string(), ElementGep { base: base.to_string(), idx_ty: idx_ty.to_string(), idx: idx.to_string(), bytes }))
}

pub(crate) fn lower_render_commands(san_ll: &str) -> Cow<'_, str> {
    // NVMTL_ICB_DGC=1: the extended record and the state commands (render_command/dgc.rs); otherwise exactly as before
    if dgc::enabled() {
        return dgc::lower(san_ll);
    }
    if !LOWERED.iter().any(|name| san_ll.contains(&format!("@{name}("))) {
        return Cow::Borrowed(san_ll);
    }
    let mut out = String::with_capacity(san_ll.len() + 4096);
    let mut serial = 0usize;
    let mut changed = false;
    let mut geps: std::collections::HashMap<String, ElementGep> = std::collections::HashMap::new();
    for line in san_ll.split_inclusive('\n') {
        let (body, nl) = match line.strip_suffix('\n') {
            Some(body) => (body, "\n"),
            None => (line, ""),
        };
        if body.starts_with("define ") {
            geps.clear();
        } else if let Some((name, gep)) = element_gep(body) {
            geps.insert(name, gep);
        }
        match lower_line(body, serial, &geps) {
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
    Cow::Owned(drop_unused_declarations(out))
}

pub(crate) fn lower_render_commands_owned(san_ll: String) -> String {
    match lower_render_commands(&san_ll) {
        Cow::Borrowed(_) => san_ll,
        Cow::Owned(lowered) => lowered,
    }
}

fn drop_unused_declarations(text: String) -> String {
    drop_unused_declarations_of(text, &LOWERED)
}

fn drop_unused_declarations_of(text: String, lowered: &[&str]) -> String {
    let unused: Vec<&str> = lowered
        .iter()
        .copied()
        .filter(|name| {
            let call = format!("@{name}(");
            !text.lines().any(|line| line.contains(&call) && !line.trim_start().starts_with("declare "))
        })
        .collect();
    if unused.is_empty() {
        return text;
    }
    let mut out = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        let trimmed = line.trim_start();
        if trimmed.starts_with("declare ") && unused.iter().any(|name| trimmed.contains(&format!("@{name}("))) {
            continue;
        }
        out.push_str(line);
    }
    out
}

/// The call's arguments: (type, value) for each, split at the top-level commas. None for a form this pass does not
/// read (a constant-expression operand, a metadata argument, an unbalanced line).
fn call_arguments(body: &str, callee: &str) -> Option<(String, Vec<(String, String)>)> {
    let trimmed = body.trim_start();
    let indent = &body[..body.len() - trimmed.len()];
    // only `[tail|notail|musttail] call void @...(...)` with nothing bound to a result
    let rest = trimmed
        .strip_prefix("tail ")
        .or_else(|| trimmed.strip_prefix("notail "))
        .or_else(|| trimmed.strip_prefix("musttail "))
        .unwrap_or(trimmed);
    let rest = rest.strip_prefix("call ")?;
    let at = rest.find(callee)?;
    if rest[..at].trim() != "void" {
        return None;
    }
    let args = &rest[at + callee.len()..];
    let mut depth = 0i32;
    let mut end = None;
    for (i, ch) in args.char_indices() {
        match ch {
            '(' => depth += 1,
            ')' if depth == 0 => {
                end = Some(i);
                break;
            }
            ')' => depth -= 1,
            _ => {}
        }
    }
    let inner = &args[..end?];
    let mut pieces = Vec::new();
    let mut depth = 0i32;
    let mut start = 0;
    for (i, ch) in inner.char_indices() {
        match ch {
            '(' | '[' | '{' | '<' => depth += 1,
            ')' | ']' | '}' | '>' => depth -= 1,
            ',' if depth == 0 => {
                pieces.push(&inner[start..i]);
                start = i + 1;
            }
            _ => {}
        }
    }
    pieces.push(&inner[start..]);
    let mut out = Vec::with_capacity(pieces.len());
    for piece in pieces {
        let piece = piece.trim();
        // a vector (`<3 x i32> %v`, `<3 x i32> <i32 1, i32 2, i32 3>`): the type is everything up to the first `>`
        if piece.starts_with('<') {
            let close = piece.find('>')?;
            let (ty, value) = (&piece[..=close], piece[close + 1..].trim());
            if value.is_empty() || value.contains('(') || value.starts_with('!') {
                return None;
            }
            out.push((ty.to_string(), value.to_string()));
            continue;
        }
        let (ty, value) = piece.rsplit_once(' ')?;
        if value.contains('(') || value.contains(')') || value.starts_with('!') {
            return None;
        }
        // the type is the leading `ptr addrspace(N)` / `iN` / typed pointer; parameter attributes follow it
        let ty = if let Some(after) = ty.strip_prefix("ptr addrspace(") {
            let close = after.find(')')?;
            format!("ptr addrspace({})", &after[..close])
        } else if ty.starts_with("ptr") {
            "ptr".to_string()
        } else {
            let first = ty.split_whitespace().next()?;
            if first.ends_with('*') || ty.contains(" addrspace(") {
                // a typed pointer (`i8 addrspace(1)*`): keep the whole spelling up to the star
                let star = ty.find('*')?;
                ty[..=star].to_string()
            } else {
                first.to_string()
            }
        };
        out.push((ty, value.to_string()));
    }
    Some((indent.to_string(), out))
}

struct Emit {
    out: String,
    indent: String,
    prefix: String,
    cb_ty: String,
    cb: String,
}

impl Emit {
    fn line(&mut self, text: &str) {
        self.out.push_str(&self.indent);
        self.out.push_str(text);
        self.out.push('\n');
    }
    fn name(&self, what: &str) -> String {
        format!("%{}.{what}", self.prefix)
    }
    /// The address of word `word` of the slot record (`%<prefix>.base` is slot * 16).
    fn word_ptr(&mut self, word: u32) -> String {
        let idx = self.name(&format!("i{word}"));
        let ptr = self.name(&format!("w{word}"));
        let base = self.name("base");
        let (cb_ty, cb) = (self.cb_ty.clone(), self.cb.clone());
        self.line(&format!("{idx} = add i64 {base}, {word}"));
        self.line(&format!("{ptr} = getelementptr inbounds i32, {cb_ty} {cb}, i64 {idx}"));
        ptr
    }
    fn store(&mut self, word: u32, value: &str) {
        let ptr = self.word_ptr(word);
        let ty = self.cb_ty.clone();
        self.line(&format!("store i32 {value}, {ty} {ptr}, align 4"));
    }
    fn load(&mut self, word: u32) -> String {
        let ptr = self.word_ptr(word);
        let value = self.name(&format!("r{word}"));
        let ty = self.cb_ty.clone();
        self.line(&format!("{value} = load i32, {ty} {ptr}, align 4"));
        value
    }
    /// `ok ? value : 0`
    fn gated(&mut self, ok: &str, value: &str, what: &str) -> String {
        let name = self.name(what);
        self.line(&format!("{name} = select i1 {ok}, i32 {value}, i32 0"));
        name
    }
}

fn lower_line(body: &str, serial: usize, geps: &std::collections::HashMap<String, ElementGep>) -> Option<String> {
    let (callee, shape) = if body.contains(DRAW) {
        (DRAW, Shape::Draw)
    } else if body.contains(DRAW_INDEXED16) {
        (DRAW_INDEXED16, Shape::Indexed { shift: 1 })
    } else if body.contains(DRAW_INDEXED32) {
        (DRAW_INDEXED32, Shape::Indexed { shift: 2 })
    } else if body.contains(RESET) {
        (RESET, Shape::Reset)
    } else {
        return None;
    };
    let (indent, args) = call_arguments(body, callee)?;
    let want = match shape {
        Shape::Draw => 7,
        Shape::Indexed { .. } => 8,
        Shape::Reset => 2,
    };
    if args.len() != want {
        return None;
    }
    let (cb_ty, cb) = args[0].clone();
    if !cb_ty.contains("addrspace(1)") || args[1].0 != "i32" {
        return None;
    }
    if args[2..].iter().enumerate().any(|(k, (ty, _))| {
        let pointer = matches!(shape, Shape::Indexed { .. }) && k == 2;
        if pointer {
            !ty.contains("addrspace(1)")
        } else {
            ty != "i32"
        }
    }) {
        return None;
    }
    let mut e = Emit {
        out: String::new(),
        indent,
        prefix: format!("nvicb{serial}"),
        cb_ty: if cb_ty.starts_with("ptr") { "ptr addrspace(1)".to_string() } else { cb_ty },
        cb,
    };
    if !e.cb_ty.starts_with("ptr") {
        // typed pointers: the record is addressed as i32 words
        let cast = e.name("cb");
        let (ty, cb) = (e.cb_ty.clone(), e.cb.clone());
        e.line(&format!("{cast} = bitcast {ty} {cb} to i32 addrspace(1)*"));
        e.cb_ty = "i32 addrspace(1)*".to_string();
        e.cb = cast;
    }
    let slot64 = e.name("slot");
    let base = e.name("base");
    let slot = args[1].1.clone();
    e.line(&format!("{slot64} = zext i32 {slot} to i64"));
    e.line(&format!("{base} = shl i64 {slot64}, {RECORD_SHIFT}"));
    let _ = RECORD_WORDS;
    match shape {
        Shape::Reset => {
            e.store(W_KIND, &KIND_EMPTY.to_string());
            e.store(W_IDX, "0");
            e.store(W_IDX + 1, "0");
            e.store(W_DRAW, "0");
            e.store(W_DRAW + 1, "0");
        }
        Shape::Draw => {
            let prim = args[2].1.clone();
            let (start, count, instances, base_instance) =
                (args[3].1.clone(), args[4].1.clone(), args[5].1.clone(), args[6].1.clone());
            let exec = e.load(W_EXEC_PRIM);
            // the execute draws a slot with an index buffer (word 15) from its indexed form only
            let have_shift = e.load(W_IB_SHIFT);
            let (primok, noib, ok) = (e.name("primok"), e.name("noib"), e.name("ok"));
            e.line(&format!("{primok} = icmp eq i32 {exec}, {prim}"));
            e.line(&format!("{noib} = icmp eq i32 {have_shift}, 0"));
            e.line(&format!("{ok} = and i1 {primok}, {noib}"));
            let kind = e.name("kind");
            e.line(&format!("{kind} = select i1 {ok}, i32 {KIND_DRAW}, i32 {KIND_REFUSED}"));
            let count = e.gated(&ok, &count, "count");
            let instances = e.gated(&ok, &instances, "inst");
            e.store(W_KIND, &kind);
            e.store(W_PRIM, &prim);
            e.store(W_IDX, "0");
            e.store(W_IDX + 1, "0");
            e.store(W_DRAW, &count);
            e.store(W_DRAW + 1, &instances);
            e.store(W_DRAW + 2, &start);
            e.store(W_DRAW + 3, &base_instance);
        }
        Shape::Indexed { shift } => {
            let prim = args[2].1.clone();
            let count = args[3].1.clone();
            let (ib_ty, ib) = args[4].clone();
            let (instances, base_vertex, base_instance) = (args[5].1.clone(), args[6].1.clone(), args[7].1.clone());
            let exec = e.load(W_EXEC_PRIM);
            let lo = e.load(W_IB_LO);
            let hi = e.load(W_IB_HI);
            let limit = e.load(W_IB_LIMIT);
            let have_shift = e.load(W_IB_SHIFT);
            let ib_ty = if ib_ty.starts_with("ptr") { "ptr addrspace(1)".to_string() } else { ib_ty };
            let n = |what: &str| format!("%nvicb{serial}.{what}");
            // `p + j + k` is a chain of GEPs (WebKit's vsICBIndirectIndexed*: `indexBuffer + slotData[2] + args.indexStart`):
            // the root's address plus every link's terms (a GEP result has no address payload of its own)
            let mut chain: Vec<&ElementGep> = Vec::new();
            if ib_ty == "ptr addrspace(1)" {
                let mut at = ib.as_str();
                while let Some(g) = geps.get(at).filter(|_| chain.len() < 16) {
                    chain.push(g);
                    at = g.base.as_str();
                }
            }
            if chain.len() >= 2 {
                e.line(&format!("{} = ptrtoint ptr addrspace(1) {} to i64", n("paddr"), chain[chain.len() - 1].base));
                let mut acc = n("paddr");
                for (k, g) in chain.iter().enumerate() {
                    let next = if k + 1 == chain.len() { n("addr") } else { n(&format!("pa{k}")) };
                    if let Ok(c) = g.idx.parse::<i64>() {
                        e.line(&format!("{next} = add i64 {acc}, {}", c * g.bytes as i64));
                    } else {
                        let idx = if g.idx_ty == "i32" {
                            e.line(&format!("{} = sext i32 {} to i64", n(&format!("pi{k}")), g.idx));
                            n(&format!("pi{k}"))
                        } else {
                            g.idx.clone()
                        };
                        e.line(&format!("{} = mul i64 {idx}, {}", n(&format!("po{k}")), g.bytes));
                        e.line(&format!("{next} = add i64 {acc}, {}", n(&format!("po{k}"))));
                    }
                    acc = next;
                }
            } else {
            match geps.get(&ib).filter(|_| ib_ty == "ptr addrspace(1)") {
                Some(g) => {
                    e.line(&format!("{} = ptrtoint ptr addrspace(1) {} to i64", n("paddr"), g.base));
                    if let Ok(k) = g.idx.parse::<i64>() {
                        e.line(&format!("{} = add i64 {}, {}", n("addr"), n("paddr"), k * g.bytes as i64));
                    } else {
                    let idx = if g.idx_ty == "i32" {
                        e.line(&format!("{} = sext i32 {} to i64", n("pidx"), g.idx));
                        n("pidx")
                    } else {
                        g.idx.clone()
                    };
                    e.line(&format!("{} = mul i64 {idx}, {}", n("poff"), g.bytes));
                    e.line(&format!("{} = add i64 {}, {}", n("addr"), n("paddr"), n("poff")));
                    }
                }
                None => e.line(&format!("{} = ptrtoint {ib_ty} {ib} to i64", n("addr"))),
            }
            }
            for text in [
                format!("{} = zext i32 {lo} to i64", n("lo64")),
                format!("{} = zext i32 {hi} to i64", n("hi64")),
                format!("{} = shl i64 {}, 32", n("hish"), n("hi64")),
                format!("{} = or i64 {}, {}", n("bound"), n("hish"), n("lo64")),
                format!("{} = zext i32 {limit} to i64", n("limit64")),
                format!("{} = sub i64 {}, {}", n("delta"), n("addr"), n("bound")),
                format!("{} = icmp eq i32 {exec}, {prim}", n("primok")),
                format!("{} = icmp eq i32 {have_shift}, {shift}", n("typeok")),
                format!("{} = icmp ne i64 {}, 0", n("haveib"), n("bound")),
                format!("{} = icmp uge i64 {}, {}", n("above"), n("addr"), n("bound")),
                format!("{} = icmp ule i64 {}, {}", n("inside"), n("delta"), n("limit64")),
                format!("{} = and i64 {}, {}", n("misalign"), n("delta"), (1u32 << shift) - 1),
                format!("{} = icmp eq i64 {}, 0", n("aligned"), n("misalign")),
                format!("{} = and i1 {}, {}", n("ok1"), n("primok"), n("typeok")),
                format!("{} = and i1 {}, {}", n("ok2"), n("ok1"), n("haveib")),
                format!("{} = and i1 {}, {}", n("ok3"), n("ok2"), n("above")),
                format!("{} = and i1 {}, {}", n("ok4"), n("ok3"), n("inside")),
                format!("{} = and i1 {}, {}", n("ok"), n("ok4"), n("aligned")),
                format!("{} = lshr i64 {}, {shift}", n("first64"), n("delta")),
                format!("{} = trunc i64 {} to i32", n("first32"), n("first64")),
                format!("{} = select i1 {}, i32 {KIND_INDEXED}, i32 {KIND_REFUSED}", n("kind"), n("ok")),
            ] {
                e.line(&text);
            }
            let ok = n("ok");
            let count = e.gated(&ok, &count, "count");
            let instances = e.gated(&ok, &instances, "inst");
            let first = e.gated(&ok, &n("first32"), "first");
            e.store(W_KIND, &n("kind"));
            e.store(W_PRIM, &prim);
            e.store(W_IDX, &count);
            e.store(W_IDX + 1, &instances);
            e.store(W_IDX + 2, &first);
            e.store(W_IDX + 3, &base_vertex);
            e.store(W_IDX + 4, &base_instance);
            e.store(W_DRAW, "0");
            e.store(W_DRAW + 1, "0");
        }
    }
    Some(e.out.trim_end_matches('\n').to_string())
}

#[derive(Clone, Copy)]
enum Shape {
    Draw,
    Indexed { shift: u32 },
    Reset,
}

#[cfg(test)]
mod tests {
    use super::*;

    const INDEXED: &str = "  tail call void @air.draw_indexed_primitives_render_command.p1i32(ptr addrspace(1) captures(none) %cb, i32 %slot, i32 %prim, i32 0, ptr addrspace(1) %ib, i32 %n, i32 %bv, i32 %bi) #4\n";

    #[test]
    fn a_chain_of_index_pointer_geps_is_addressed_from_its_root() {
        // WebKit's vsICBIndirectIndexedUint: indexBuffer + slotData[2] + args.indexStart (two GEPs on an entry parameter)
        let call = INDEXED.replace("%ib,", "%add.ptr3,");
        let text = format!("define void @f() {{\n  %add.ptr = getelementptr inbounds i32, ptr addrspace(1) %indexBuffer, i64 %idx.ext\n  %add.ptr3 = getelementptr inbounds i32, ptr addrspace(1) %add.ptr, i64 %idx.ext2\n{call}  ret void\n}}\n");
        let out = lower_render_commands(&text).into_owned();
        assert!(out.contains("%nvicb0.paddr = ptrtoint ptr addrspace(1) %indexBuffer to i64"), "{out}");
        assert!(out.contains("%nvicb0.po0 = mul i64 %idx.ext2, 4") && out.contains("%nvicb0.po1 = mul i64 %idx.ext, 4"), "{out}");
        assert!(out.contains("%nvicb0.pa0 = add i64 %nvicb0.paddr, %nvicb0.po0"), "{out}");
        assert!(out.contains("%nvicb0.addr = add i64 %nvicb0.pa0, %nvicb0.po1"), "{out}");
        assert!(!out.contains("ptrtoint ptr addrspace(1) %add.ptr"), "{out}");
    }

    #[test]
    fn an_index_pointer_plus_k_is_addressed_from_its_base() {
        let call = INDEXED.replace("%ib,", "%add.ptr,");
        let text = format!("define void @f() {{\n  %add.ptr = getelementptr inbounds i32, ptr addrspace(1) %0, i64 6\n{call}  ret void\n}}\ndefine void @g() {{\n{call}  ret void\n}}\n");
        let out = lower_render_commands(&text).into_owned();
        assert!(out.contains("%nvicb0.paddr = ptrtoint ptr addrspace(1) %0 to i64"), "{out}");
        assert!(out.contains("%nvicb0.addr = add i64 %nvicb0.paddr, 24"), "{out}");
        // another function's %add.ptr is not this one's
        assert!(out.contains("%nvicb1.addr = ptrtoint ptr addrspace(1) %add.ptr to i64"), "{out}");
        let dynamic = text.replace("i64 6\n", "i32 %k\n");
        let out = lower_render_commands(&dynamic).into_owned();
        assert!(out.contains("%nvicb0.pidx = sext i32 %k to i64"), "{out}");
        assert!(out.contains("%nvicb0.poff = mul i64 %nvicb0.pidx, 4"), "{out}");
        assert!(is_address_name("%nvicb3.addr") && is_address_name("nvicb0.paddr") && !is_address_name("%nvicb0.delta"));
    }

    #[test]
    fn an_indexed_draw_becomes_record_stores() {
        let text = format!("define void @f() {{\n{INDEXED}  ret void\n}}\ndeclare void @air.draw_indexed_primitives_render_command.p1i32(ptr addrspace(1) captures(none), i32, i32, i32, ptr addrspace(1), i32, i32, i32)\n");
        let out = lower_render_commands(&text).into_owned();
        assert!(!out.contains("@air.draw_indexed_primitives_render_command"), "{out}");
        assert!(out.contains("%nvicb0.addr = ptrtoint ptr addrspace(1) %ib to i64"), "{out}");
        assert!(out.contains("%nvicb0.base = shl i64 %nvicb0.slot, 4"), "{out}");
        assert!(out.contains("%nvicb0.typeok = icmp eq i32 %nvicb0.r15, 2"), "{out}");
        assert!(out.contains("%nvicb0.misalign = and i64 %nvicb0.delta, 3"), "{out}");
        // eleven words written: kind, prim, five indexed words, two zeroed plain-draw counts
        assert_eq!(out.matches("store i32").count(), 9, "{out}");
        assert!(out.contains("store i32 %nvicb0.count, ptr addrspace(1) %nvicb0.w2"), "{out}");
    }

    #[test]
    fn uint16_indices_check_a_shift_of_one() {
        let text = INDEXED.replace(".p1i32(", ".p1i16(");
        let out = lower_render_commands(&text).into_owned();
        assert!(out.contains("%nvicb0.typeok = icmp eq i32 %nvicb0.r15, 1"), "{out}");
        assert!(out.contains("%nvicb0.misalign = and i64 %nvicb0.delta, 1"), "{out}");
    }

    #[test]
    fn a_plain_draw_and_a_reset_are_lowered_with_distinct_names() {
        let text = "  tail call void @air.draw_primitives_render_command(ptr addrspace(1) captures(none) %cb, i32 %t, i32 4, i32 7, i32 8, i32 9, i32 10) #2\n  tail call void @air.reset_render_command(ptr addrspace(1) captures(none) %cb, i32 %t) #2\n";
        let out = lower_render_commands(text).into_owned();
        assert!(!out.contains("render_command("), "{out}");
        assert!(out.contains("%nvicb0.primok = icmp eq i32 %nvicb0.r11, 4"), "{out}");
        assert!(out.contains("%nvicb0.noib = icmp eq i32 %nvicb0.r15, 0"), "{out}");
        assert!(out.contains("%nvicb0.ok = and i1 %nvicb0.primok, %nvicb0.noib"), "{out}");
        assert!(out.contains("store i32 %nvicb0.count, ptr addrspace(1) %nvicb0.w7"), "{out}");
        assert!(out.contains("store i32 7, ptr addrspace(1) %nvicb0.w9"), "{out}");
        assert!(out.contains("%nvicb1.base = shl i64 %nvicb1.slot, 4"), "{out}");
        assert!(out.contains("store i32 0, ptr addrspace(1) %nvicb1.w0"), "{out}");
    }

    #[test]
    fn other_command_families_are_left_for_the_refusal() {
        let text = "  tail call void @air.set_vertex_buffer_render_command.p1i8(ptr addrspace(1) captures(none) %cb, i32 %t, ptr addrspace(1) %0, i64 -1, i32 3) #2\n";
        assert!(matches!(lower_render_commands(text), Cow::Borrowed(_)));
    }

    #[test]
    fn a_constant_expression_operand_is_not_read() {
        let text = "  tail call void @air.draw_primitives_render_command(ptr addrspace(1) getelementptr (i8, ptr addrspace(1) null, i64 8), i32 0, i32 3, i32 0, i32 3, i32 1, i32 0)\n";
        assert!(matches!(lower_render_commands(text), Cow::Borrowed(_)));
    }

    #[test]
    fn a_declaration_stays_while_a_call_is_left() {
        let text = "  tail call void @air.reset_render_command(ptr addrspace(1) captures(none) %cb, i32 %t)\n  tail call void @air.reset_render_command(ptr addrspace(1) getelementptr (i8, ptr addrspace(1) null, i64 8), i32 0)\ndeclare void @air.reset_render_command(ptr addrspace(1) captures(none), i32)\n";
        let out = lower_render_commands(text).into_owned();
        assert!(out.contains("declare void @air.reset_render_command"), "{out}");
    }
}
