//! Tile functions as compute kernels, for a GPU without tile memory (render-pass splitting; the plugin's
//! docs/design/DESIGN-tile-shading.md).
//!
//! A Metal tile function is an AIR kernel whose implicit imageblock reads and writes the render pass's colour
//! attachments in tile memory (`air.load.implicit_imageblock.*`, `air.store.implicit_imageblock.*`, their coordinates
//! relative to the tile). On an immediate-mode GPU the plugin ends the render pass at `dispatchThreadsPerTile:`, copies
//! each attachment the function uses into a "plane" (a 2D-array storage texture, one layer per sample, 32-bit
//! channels), runs the function as a compute dispatch with one threadgroup per tile, copies the written planes back and
//! starts the pass again with its attachments loaded.
//!
//! This pass rewrites the tile function's AIR text into such a kernel; the translator then sees an ordinary kernel. It
//! runs only for tile pipelines: no other translation passes through it, so every other module is byte-identical.
//!   - every implicit imageblock access becomes a read or write of the attachment's plane (texture index
//!     `PLANE_TEXTURE_BASE + n`, `texture2d_array<float|uint|int, read_write>`) at the tile origin
//!     (threadgroup_position_in_grid * tile size) plus the coordinate the function gave;
//!   - data rate 0 (the plain read/write) reads sample 0 and writes every sample; rates 1 (colour) and 2 (sample) read
//!     and write the layer the index names; a coverage-mask write writes the samples whose bit is set. Each sample is
//!     its own colour: `get_num_colors` is the sample count and colour i covers sample i only;
//!   - `get_imageblock_width/height` read the tile size from a hidden constant buffer (`TILE_PARAM_BUFFER`, a uint4:
//!     tile width, tile height, render-area width, render-area height), `get_imageblock_samples` / `get_num_colors`
//!     are the pipeline's sample count;
//!   - a threadgroup barrier on threadgroup or imageblock memory also orders texture memory (the planes are images).
//! Explicit imageblocks (`air.imageblock_data`) stay what the translator makes of them in any kernel: per-threadgroup
//! memory for one dispatch.

use std::collections::BTreeSet;

/// Buffer index of the hidden uint4 {tile width, tile height, render width, render height}.
pub const TILE_PARAM_BUFFER: u32 = 30;
/// Texture index of colour attachment 0's plane; attachment n is at `PLANE_TEXTURE_BASE + n`.
pub const PLANE_TEXTURE_BASE: u32 = 120;
const MAX_ATTACHMENTS: u32 = 8;

/// The component type of a colour attachment's pixel format, which is also its plane's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaneClass {
    Float,
    Uint,
    Sint,
}

#[derive(Clone, Debug)]
pub struct TileLowerOptions {
    /// The pipeline's raster sample count (1, 2, 4 or 8).
    pub samples: u32,
    /// The class of each colour attachment's pixel format; `None` where the pipeline has no attachment.
    pub classes: [Option<PlaneClass>; MAX_ATTACHMENTS as usize],
}

#[derive(Clone, Debug)]
pub struct LoweredTile {
    pub ll: String,
    /// Colour attachments the function reads (bit n), including through a coverage-mask write.
    pub reads: u32,
    /// Colour attachments the function writes (bit n).
    pub writes: u32,
    /// The function uses an explicit imageblock (per-threadgroup memory, one cell per thread).
    pub explicit_imageblock: bool,
}

impl PlaneClass {
    fn elem(self) -> &'static str {
        match self {
            PlaneClass::Float => "float",
            PlaneClass::Uint | PlaneClass::Sint => "i32",
        }
    }
    fn vec4(self) -> &'static str {
        match self {
            PlaneClass::Float => "<4 x float>",
            PlaneClass::Uint | PlaneClass::Sint => "<4 x i32>",
        }
    }
    fn suffix(self) -> &'static str {
        match self {
            PlaneClass::Float => "v4f32",
            PlaneClass::Uint => "u.v4i32",
            PlaneClass::Sint => "s.v4i32",
        }
    }
    fn type_name(self) -> &'static str {
        match self {
            PlaneClass::Float => "texture2d_array<float, read_write>",
            PlaneClass::Uint => "texture2d_array<uint, read_write>",
            PlaneClass::Sint => "texture2d_array<int, read_write>",
        }
    }
    fn read_fn(self) -> String {
        format!("air.read_texture_2d_array.{}", self.suffix())
    }
    fn write_fn(self) -> String {
        format!("air.write_texture_2d_array.{}", self.suffix())
    }
    fn read_decl(self) -> String {
        format!(
            "declare {{ {v}, i8 }} @{f}(ptr addrspace(1), ptr addrspace(2), <2 x i32>, i32, <2 x i32>, i32, i32)",
            v = self.vec4(),
            f = self.read_fn()
        )
    }
    fn write_decl(self) -> String {
        format!(
            "declare void @{f}(ptr addrspace(1), <2 x i32>, i32, {v}, i32, i32)",
            f = self.write_fn(),
            v = self.vec4()
        )
    }
}

/// An imageblock value type from an intrinsic suffix: `v4f16` is 4 lanes of half, `i32` one int.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ValueType {
    lanes: u32,
    scalar: Scalar,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Scalar {
    F16,
    F32,
    I8,
    I16,
    I32,
}

impl Scalar {
    fn ll(self) -> &'static str {
        match self {
            Scalar::F16 => "half",
            Scalar::F32 => "float",
            Scalar::I8 => "i8",
            Scalar::I16 => "i16",
            Scalar::I32 => "i32",
        }
    }
    fn is_float(self) -> bool {
        matches!(self, Scalar::F16 | Scalar::F32)
    }
}

impl ValueType {
    fn parse(suffix: &str) -> Option<Self> {
        let (lanes, rest) = match suffix.strip_prefix('v') {
            Some(r) => {
                let digits: String = r.chars().take_while(|c| c.is_ascii_digit()).collect();
                (digits.parse().ok()?, &r[digits.len()..])
            }
            None => (1, suffix),
        };
        let scalar = match rest {
            "f16" => Scalar::F16,
            "f32" => Scalar::F32,
            "i8" => Scalar::I8,
            "i16" => Scalar::I16,
            "i32" => Scalar::I32,
            _ => return None,
        };
        (1..=4).contains(&lanes).then_some(Self { lanes, scalar })
    }
    fn ll(self) -> String {
        if self.lanes == 1 {
            self.scalar.ll().to_string()
        } else {
            format!("<{} x {}>", self.lanes, self.scalar.ll())
        }
    }
    fn with_scalar(self, s: &str) -> String {
        if self.lanes == 1 {
            s.to_string()
        } else {
            format!("<{} x {}>", self.lanes, s)
        }
    }
}

/// One `call` line, split into what the rewrite needs.
struct Call {
    indent: String,
    result: Option<String>,
    ret_ty: String,
    callee: String,
    args: Vec<(String, String)>,
}

fn matching_close(s: &str, open_at: usize) -> Option<usize> {
    let mut depth = 0i32;
    for (i, ch) in s[open_at..].char_indices() {
        match ch {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(open_at + i);
                }
            }
            _ => {}
        }
    }
    None
}

fn split_top_level_commas(s: &str) -> Vec<String> {
    let mut items = Vec::new();
    let (mut start, mut angle, mut paren, mut brace) = (0usize, 0i32, 0i32, 0i32);
    for (idx, ch) in s.char_indices() {
        match ch {
            '<' => angle += 1,
            '>' => angle -= 1,
            '(' => paren += 1,
            ')' => paren -= 1,
            '{' => brace += 1,
            '}' => brace -= 1,
            ',' if angle == 0 && paren == 0 && brace == 0 => {
                items.push(s[start..idx].trim().to_string());
                start = idx + 1;
            }
            _ => {}
        }
    }
    let tail = s[start..].trim();
    if !tail.is_empty() {
        items.push(tail.to_string());
    }
    items
}

/// `<2 x i16> %tid` -> ("<2 x i16>", "%tid"); `ptr addrspace(1) captures(none) %t` -> ("ptr addrspace(1)", "%t").
fn split_type_value(arg: &str) -> (String, String) {
    let a = arg.trim();
    let (ty, rest) = if a.starts_with('<') {
        let mut depth = 0i32;
        let mut end = a.len();
        for (i, ch) in a.char_indices() {
            match ch {
                '<' => depth += 1,
                '>' => {
                    depth -= 1;
                    if depth == 0 {
                        end = i + 1;
                        break;
                    }
                }
                _ => {}
            }
        }
        (&a[..end], &a[end..])
    } else if a.starts_with("ptr addrspace(") {
        let end = a.find(')').map(|i| i + 1).unwrap_or(a.len());
        (&a[..end], &a[end..])
    } else {
        match a.find(' ') {
            Some(i) => (&a[..i], &a[i..]),
            None => (a, ""),
        }
    };
    let mut rest = rest.trim();
    for attr in ["noundef ", "readonly ", "captures(none) ", "nonnull "] {
        while let Some(r) = rest.strip_prefix(attr) {
            rest = r.trim_start();
        }
    }
    (ty.trim().to_string(), rest.to_string())
}

fn parse_call(line: &str) -> Option<Call> {
    let at = line.find('@')?;
    let call_at = line[..at].rfind("call ")?;
    let indent: String = line.chars().take_while(|c| c.is_whitespace()).collect();
    let mut head = line[..call_at].trim();
    for kw in ["musttail", "notail", "tail"] {
        if let Some(h) = head.strip_suffix(kw) {
            head = h.trim_end();
            break;
        }
    }
    let result = head
        .strip_suffix('=')
        .map(str::trim)
        .filter(|h| h.starts_with('%') && !h.contains(' '))
        .map(str::to_string);
    let ret_ty = line[call_at + 5..at]
        .split_whitespace()
        .filter(|w| !matches!(*w, "fast" | "nnan" | "ninf" | "nsz" | "arcp" | "contract" | "afn" | "reassoc"))
        .collect::<Vec<_>>()
        .join(" ");
    let open = at + line[at..].find('(')?;
    let callee = line[at + 1..open].to_string();
    let close = matching_close(line, open)?;
    let args = split_top_level_commas(&line[open + 1..close])
        .iter()
        .map(|a| split_type_value(a))
        .collect();
    Some(Call { indent, result, ret_ty, callee, args })
}

fn const_u32(v: &str) -> Option<u32> {
    v.trim().parse().ok()
}

struct Rewriter {
    opts: TileLowerOptions,
    n: u32,
    reads: u32,
    writes: u32,
    classes_used: BTreeSet<u8>,
}

impl Rewriter {
    fn class_of(&self, attachment: u32, what: &str) -> Result<PlaneClass, String> {
        if attachment >= MAX_ATTACHMENTS {
            return Err(format!("{what}: colour attachment {attachment} is out of range"));
        }
        self.opts.classes[attachment as usize].ok_or_else(|| {
            format!(
                "{what}: the tile function uses colour attachment {attachment}, which the tile pipeline descriptor gives no pixel format"
            )
        })
    }

    fn note_class(&mut self, c: PlaneClass) {
        self.classes_used.insert(c as u8);
    }

    fn fresh(&mut self) -> String {
        self.n += 1;
        format!("%nvt.{}", self.n)
    }

    fn coord(&mut self, out: &mut Vec<String>, ind: &str, ty: &str, val: &str) -> Result<String, String> {
        if ty != "<2 x i16>" {
            return Err(format!("imageblock coordinate of type {ty} (want <2 x i16>)"));
        }
        let c = self.fresh();
        out.push(format!("{ind}{c}.c = zext <2 x i16> {val} to <2 x i32>"));
        out.push(format!("{ind}{c}.p = add <2 x i32> {c}.c, %nvt.org"));
        Ok(format!("{c}.p"))
    }

    fn layer_of(&mut self, out: &mut Vec<String>, ind: &str, rate: u32, index: &(String, String)) -> Result<String, String> {
        if rate == 0 || self.opts.samples <= 1 {
            return Ok("0".to_string());
        }
        if rate > 2 {
            return Err(format!("imageblock data rate {rate} is not colour (1) or sample (2)"));
        }
        if index.0 != "i16" {
            return Err(format!("imageblock colour index of type {} (want i16)", index.0));
        }
        let l = self.fresh();
        out.push(format!("{ind}{l} = zext i16 {} to i32", index.1));
        Ok(l)
    }

    fn read_plane(&mut self, out: &mut Vec<String>, ind: &str, class: PlaneClass, a: u32, coord: &str, layer: &str) -> String {
        let v = self.fresh();
        out.push(format!(
            "{ind}{v}.r = tail call {{ {t}, i8 }} @{f}(ptr addrspace(1) %nvt.plane.{a}, ptr addrspace(2) %nvt.smp, <2 x i32> {coord}, i32 {layer}, <2 x i32> zeroinitializer, i32 0, i32 3)",
            t = class.vec4(),
            f = class.read_fn()
        ));
        out.push(format!("{ind}{v}.e = extractvalue {{ {t}, i8 }} {v}.r, 0", t = class.vec4()));
        format!("{v}.e")
    }

    fn write_plane(&mut self, out: &mut Vec<String>, ind: &str, class: PlaneClass, a: u32, coord: &str, layer: &str, value: &str) {
        out.push(format!(
            "{ind}tail call void @{f}(ptr addrspace(1) %nvt.plane.{a}, <2 x i32> {coord}, i32 {layer}, {t} {value}, i32 0, i32 3)",
            f = class.write_fn(),
            t = class.vec4()
        ));
    }

    /// A plane texel (<4 x float> / <4 x i32>) narrowed to the value type, the last step defining `result`.
    fn narrow(&mut self, out: &mut Vec<String>, ind: &str, class: PlaneClass, texel: &str, vt: ValueType, result: &str) -> Result<(), String> {
        if vt.scalar.is_float() != (class == PlaneClass::Float) {
            return Err(format!(
                "the tile function reads a {} value from a colour attachment whose pixel format is {}",
                vt.ll(),
                if class == PlaneClass::Float { "a float/normalized one" } else { "an integer one" }
            ));
        }
        let e4 = class.elem();
        let mut steps: Vec<(String, String)> = Vec::new(); // (instruction with {dst}, type it defines)
        let mut cur = texel.to_string();
        match vt.lanes {
            4 => {}
            1 => steps.push((format!("extractelement <4 x {e4}> {cur}, i64 0"), e4.to_string())),
            n => {
                let idx = (0..n).map(|i| format!("i32 {i}")).collect::<Vec<_>>().join(", ");
                steps.push((format!("shufflevector <4 x {e4}> {cur}, <4 x {e4}> poison, <{n} x i32> <{idx}>"), vt.with_scalar(e4)));
            }
        }
        let wide = vt.with_scalar(e4);
        match vt.scalar {
            Scalar::F16 => steps.push((format!("fptrunc {wide} {{cur}} to {}", vt.ll()), vt.ll())),
            Scalar::I8 | Scalar::I16 => steps.push((format!("trunc {wide} {{cur}} to {}", vt.ll()), vt.ll())),
            Scalar::F32 | Scalar::I32 => {}
        }
        if steps.is_empty() {
            // the texel itself: copy it into the result name with an identity shuffle
            out.push(format!("{ind}{result} = shufflevector <4 x {e4}> {texel}, <4 x {e4}> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>"));
            return Ok(());
        }
        let last = steps.len() - 1;
        for (i, (inst, _ty)) in steps.into_iter().enumerate() {
            let inst = inst.replace("{cur}", &cur);
            let dst = if i == last { result.to_string() } else { self.fresh() };
            out.push(format!("{ind}{dst} = {inst}"));
            cur = dst;
        }
        Ok(())
    }

    /// A value widened to a plane texel.
    fn widen(&mut self, out: &mut Vec<String>, ind: &str, class: PlaneClass, value: &str, vt: ValueType) -> Result<String, String> {
        if vt.scalar.is_float() != (class == PlaneClass::Float) {
            return Err(format!(
                "the tile function writes a {} value to a colour attachment whose pixel format is {}",
                vt.ll(),
                if class == PlaneClass::Float { "a float/normalized one" } else { "an integer one" }
            ));
        }
        let e4 = class.elem();
        let wide = vt.with_scalar(e4);
        let mut cur = value.to_string();
        let conv = match vt.scalar {
            Scalar::F16 => Some("fpext"),
            Scalar::I8 | Scalar::I16 => Some(if class == PlaneClass::Sint { "sext" } else { "zext" }),
            Scalar::F32 | Scalar::I32 => None,
        };
        if let Some(op) = conv {
            let d = self.fresh();
            out.push(format!("{ind}{d} = {op} {} {cur} to {wide}", vt.ll()));
            cur = d;
        }
        match vt.lanes {
            4 => Ok(cur),
            1 => {
                let d = self.fresh();
                out.push(format!("{ind}{d} = insertelement <4 x {e4}> zeroinitializer, {e4} {cur}, i64 0"));
                Ok(d)
            }
            n => {
                let d = self.fresh();
                let idx = (0..4).map(|i| format!("i32 {}", if i < n { i } else { n })).collect::<Vec<_>>().join(", ");
                out.push(format!("{ind}{d} = shufflevector {wide} {cur}, {wide} zeroinitializer, <4 x i32> <{idx}>"));
                Ok(d)
            }
        }
    }

    fn rewrite(&mut self, line: &str) -> Result<Option<Vec<String>>, String> {
        if !line.contains('@') || !line.contains("call ") {
            return Ok(None);
        }
        let Some(call) = parse_call(line) else { return Ok(None) };
        let name = call.callee.as_str();
        let ind = call.indent.clone();
        let mut out = Vec::new();
        if let Some(sfx) = name.strip_prefix("air.load.implicit_imageblock.") {
            let vt = ValueType::parse(sfx).ok_or_else(|| format!("{name}: unsupported imageblock value type"))?;
            let [a, c, r, i] = <[(String, String); 4]>::try_from(call.args.clone())
                .map_err(|_| format!("{name} expects attachment, coordinate, data rate and index"))?;
            let att = const_u32(&a.1).ok_or_else(|| format!("{name}: the attachment is not a constant"))?;
            let rate = const_u32(&r.1).ok_or_else(|| format!("{name}: the data rate is not a constant"))?;
            let class = self.class_of(att, name)?;
            self.note_class(class);
            let result = call.result.clone().ok_or_else(|| format!("{name} has no result"))?;
            let p = self.coord(&mut out, &ind, &c.0, &c.1)?;
            let layer = self.layer_of(&mut out, &ind, rate, &i)?;
            let texel = self.read_plane(&mut out, &ind, class, att, &p, &layer);
            self.narrow(&mut out, &ind, class, &texel, vt, &result)?;
            self.reads |= 1 << att;
            return Ok(Some(out));
        }
        if let Some(sfx) = name.strip_prefix("air.store.implicit_imageblock.mask.") {
            let vt = ValueType::parse(sfx).ok_or_else(|| format!("{name}: unsupported imageblock value type"))?;
            let [v, a, c, m] = <[(String, String); 4]>::try_from(call.args.clone())
                .map_err(|_| format!("{name} expects value, attachment, coordinate and coverage mask"))?;
            let att = const_u32(&a.1).ok_or_else(|| format!("{name}: the attachment is not a constant"))?;
            let class = self.class_of(att, name)?;
            self.note_class(class);
            let p = self.coord(&mut out, &ind, &c.0, &c.1)?;
            let w = self.widen(&mut out, &ind, class, &v.1, vt)?;
            for s in 0..self.opts.samples.max(1) {
                let old = self.read_plane(&mut out, &ind, class, att, &p, &s.to_string());
                let b = self.fresh();
                out.push(format!("{ind}{b}.m = and {} {}, {}", m.0, m.1, 1u32 << s));
                out.push(format!("{ind}{b}.c = icmp ne {} {b}.m, 0", m.0));
                out.push(format!("{ind}{b}.v = select i1 {b}.c, {t} {w}, {t} {old}", t = class.vec4()));
                self.write_plane(&mut out, &ind, class, att, &p, &s.to_string(), &format!("{b}.v"));
            }
            self.reads |= 1 << att;
            self.writes |= 1 << att;
            return Ok(Some(out));
        }
        if let Some(sfx) = name.strip_prefix("air.store.implicit_imageblock.") {
            let vt = ValueType::parse(sfx).ok_or_else(|| format!("{name}: unsupported imageblock value type"))?;
            let [v, a, c, r, i] = <[(String, String); 5]>::try_from(call.args.clone())
                .map_err(|_| format!("{name} expects value, attachment, coordinate, data rate and index"))?;
            let att = const_u32(&a.1).ok_or_else(|| format!("{name}: the attachment is not a constant"))?;
            let rate = const_u32(&r.1).ok_or_else(|| format!("{name}: the data rate is not a constant"))?;
            let class = self.class_of(att, name)?;
            self.note_class(class);
            let p = self.coord(&mut out, &ind, &c.0, &c.1)?;
            let w = self.widen(&mut out, &ind, class, &v.1, vt)?;
            if rate == 0 || self.opts.samples <= 1 {
                for s in 0..self.opts.samples.max(1) {
                    self.write_plane(&mut out, &ind, class, att, &p, &s.to_string(), &w);
                }
            } else {
                let layer = self.layer_of(&mut out, &ind, rate, &i)?;
                self.write_plane(&mut out, &ind, class, att, &p, &layer, &w);
            }
            self.writes |= 1 << att;
            return Ok(Some(out));
        }
        let samples = self.opts.samples.max(1);
        let const_result = |out: &mut Vec<String>, call: &Call, value: String| -> Result<(), String> {
            let r = call.result.clone().ok_or_else(|| format!("{} has no result", call.callee))?;
            out.push(format!("{}{r} = add {} {value}, 0", call.indent, call.ret_ty));
            Ok(())
        };
        match name {
            "air.get_imageblock_samples" | "air.get_imageblock_num_colors" => {
                const_result(&mut out, &call, samples.to_string())?;
                Ok(Some(out))
            }
            "air.get_color_coverage_mask" => {
                let r = call.result.clone().ok_or_else(|| format!("{name} has no result"))?;
                let idx = call.args.get(1).ok_or_else(|| format!("{name} expects a coordinate and a colour index"))?;
                if idx.0 != call.ret_ty {
                    return Err(format!("{name}: index type {} differs from the result type {}", idx.0, call.ret_ty));
                }
                out.push(format!("{ind}{r} = shl {} 1, {}", call.ret_ty, idx.1));
                Ok(Some(out))
            }
            "air.get_imageblock_width" | "air.get_imageblock_height" => {
                let r = call.result.clone().ok_or_else(|| format!("{name} has no result"))?;
                let src = if name.ends_with("width") { "%nvt.tw" } else { "%nvt.th" };
                match call.ret_ty.as_str() {
                    "i32" => out.push(format!("{ind}{r} = add i32 {src}, 0")),
                    "i16" | "i8" => out.push(format!("{ind}{r} = trunc i32 {src} to {}", call.ret_ty)),
                    t => return Err(format!("{name} returns {t}")),
                }
                Ok(Some(out))
            }
            "air.wg.barrier" => {
                let flags = call.args.first().and_then(|a| const_u32(&a.1)).unwrap_or(0);
                if flags & (2 | 8) == 0 || flags & 4 != 0 {
                    return Ok(None);
                }
                let from = format!("@air.wg.barrier(i32 {flags},");
                let to = format!("@air.wg.barrier(i32 {},", flags | 4);
                if !line.contains(&from) {
                    return Ok(None);
                }
                Ok(Some(vec![line.replacen(&from, &to, 1)]))
            }
            n if n.contains("implicit_imageblock") || n.starts_with("air.get_imageblock") => Err(format!(
                "the tile function calls {n}, which the tile emulation does not lower"
            )),
            _ => Ok(None),
        }
    }
}

fn metadata_number(line: &str) -> Option<u32> {
    let rest = line.strip_prefix('!')?;
    let (n, _) = rest.split_once(" = ")?;
    n.trim().parse().ok()
}

fn node_refs(body: &str) -> Vec<u32> {
    let mut v = Vec::new();
    let b = body.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'!' && i + 1 < b.len() && b[i + 1].is_ascii_digit() {
            let s = i + 1;
            let mut e = s;
            while e < b.len() && b[e].is_ascii_digit() {
                e += 1;
            }
            v.push(body[s..e].parse().unwrap());
            i = e;
        } else {
            i += 1;
        }
    }
    v
}

fn location_index(body: &str) -> Option<u32> {
    let at = body.find("!\"air.location_index\", i32 ")?;
    let rest = &body[at + "!\"air.location_index\", i32 ".len()..];
    rest.split(|c: char| !c.is_ascii_digit()).next()?.parse().ok()
}

/// Lower tile function `entry` of module `ll` into a kernel (see the module comment).
pub fn lower_tile(ll: &str, entry: &str, opts: &TileLowerOptions) -> Result<LoweredTile, String> {
    if !matches!(opts.samples, 0 | 1 | 2 | 4 | 8) {
        return Err(format!("raster sample count {} is not 1, 2, 4 or 8", opts.samples));
    }
    let mut lines: Vec<String> = ll.lines().map(str::to_string).collect();
    let def_plain = format!("@{entry}(");
    let def_quoted = format!("@\"{entry}\"(");
    let d = lines
        .iter()
        .position(|l| l.starts_with("define ") && (l.contains(&def_plain) || l.contains(&def_quoted)))
        .ok_or_else(|| format!("tile function {entry} is not defined in its module"))?;
    let end = d + lines[d..].iter().position(|l| l == "}").ok_or_else(|| format!("{entry}: no end of function"))?;
    if ll.contains("air.alias_implicit_imageblock") {
        return Err(format!(
            "{entry}: an explicit imageblock aliased onto the colour attachments ([[alias_implicit_imageblock]]) is not supported by the tile emulation"
        ));
    }
    for (k, l) in lines.iter().enumerate() {
        if (k < d || k > end) && !l.starts_with("declare ") && (l.contains("@air.load.implicit_imageblock.") || l.contains("@air.store.implicit_imageblock.")) {
            return Err(format!("{entry}: an implicit imageblock access outside the tile function (not inlined)"));
        }
    }

    // the kernel's metadata node and its argument list
    let kernel_node = lines
        .iter()
        .find(|l| {
            l.starts_with('!') && (l.contains(&format!("!{{ptr @{entry}, ")) || l.contains(&format!("!{{ptr @\"{entry}\", ")))
        })
        .cloned()
        .ok_or_else(|| format!("{entry}: no kernel metadata (a tile function is an AIR kernel)"))?;
    if !lines.iter().any(|l| l.starts_with("!air.kernel = ") && node_refs(l).contains(&metadata_number(&kernel_node).unwrap_or(u32::MAX))) {
        return Err(format!("{entry} is not listed in !air.kernel"));
    }
    let knum = metadata_number(&kernel_node).ok_or_else(|| format!("{entry}: unreadable kernel metadata"))?;
    let krefs = node_refs(kernel_node.split_once(" = ").map(|x| x.1).unwrap_or(""));
    let args_node = *krefs.get(1).ok_or_else(|| format!("{entry}: kernel metadata has no argument list"))?;
    let find_node = |lines: &[String], n: u32| lines.iter().position(|l| metadata_number(l) == Some(n));
    let args_at = find_node(&lines, args_node).ok_or_else(|| format!("{entry}: argument list !{args_node} is missing"))?;
    let arg_nodes = node_refs(lines[args_at].split_once(" = ").map(|x| x.1).unwrap_or(""));
    for n in &arg_nodes {
        let Some(at) = find_node(&lines, *n) else { continue };
        let body = &lines[at];
        let loc = location_index(body);
        if body.contains("!\"air.texture\"") && loc.is_some_and(|x| x >= PLANE_TEXTURE_BASE) {
            return Err(format!(
                "{entry} binds a texture at index {} - indices {PLANE_TEXTURE_BASE}..{} hold the colour attachments in the tile emulation",
                loc.unwrap_or(0),
                PLANE_TEXTURE_BASE + MAX_ATTACHMENTS - 1
            ));
        }
        if body.contains("!\"air.buffer\"") && !body.contains("!\"air.address_space\", i32 3") && loc == Some(TILE_PARAM_BUFFER) {
            return Err(format!("{entry} binds a buffer at index {TILE_PARAM_BUFFER}, which the tile emulation keeps for the tile size"));
        }
    }

    // rewrite the body
    let mut rw = Rewriter { opts: opts.clone(), n: 0, reads: 0, writes: 0, classes_used: BTreeSet::new() };
    let mut body: Vec<String> = Vec::new();
    for l in &lines[d + 1..end] {
        match rw.rewrite(l)? {
            Some(v) => body.extend(v),
            None => body.push(l.clone()),
        }
    }
    let used = rw.reads | rw.writes;

    // the define line's own parameters
    let def = lines[d].clone();
    let open = def.find(&def_plain).map(|i| i + def_plain.len() - 1).or_else(|| def.find(&def_quoted).map(|i| i + def_quoted.len() - 1)).unwrap();
    let close = matching_close(&def, open).ok_or_else(|| format!("{entry}: unreadable parameter list"))?;
    let own = split_top_level_commas(&def[open + 1..close]);

    // the threadgroup position: the function's own [[threadgroup_position_in_grid]] when it has one (a second input
    // with the same builtin is not valid SPIR-V), else a parameter added below
    let mut tg_from: Option<(String, String)> = None;
    for n in &arg_nodes {
        let Some(at) = find_node(&lines, *n) else { continue };
        if !lines[at].contains("!\"air.threadgroup_position_in_grid\"") {
            continue;
        }
        let idx: usize = lines[at]
            .split_once("!{i32 ")
            .and_then(|(_, r)| r.split(|c: char| !c.is_ascii_digit()).next())
            .and_then(|x| x.parse().ok())
            .ok_or_else(|| format!("{entry}: unreadable threadgroup_position_in_grid argument"))?;
        let param = own.get(idx).ok_or_else(|| format!("{entry}: no parameter {idx}"))?;
        let (ty, val) = split_type_value(param);
        let name = val.split_whitespace().last().unwrap_or("").to_string();
        if !name.starts_with('%') {
            return Err(format!("{entry}: unreadable parameter {idx}"));
        }
        lines[at] = lines[at].replace(", !\"air.arg_unused\"", "");
        tg_from = Some((ty, name));
        break;
    }
    let mut tg_lines: Vec<String> = Vec::new();
    if let Some((ty, name)) = &tg_from {
        match ty.as_str() {
            "<2 x i32>" => tg_lines.push(format!("  %nvt.tg = shufflevector <2 x i32> {name}, <2 x i32> poison, <2 x i32> <i32 0, i32 1>")),
            "<2 x i16>" => tg_lines.push(format!("  %nvt.tg = zext <2 x i16> {name} to <2 x i32>")),
            "<3 x i32>" => tg_lines.push(format!("  %nvt.tg = shufflevector <3 x i32> {name}, <3 x i32> poison, <2 x i32> <i32 0, i32 1>")),
            "<3 x i16>" => {
                tg_lines.push(format!("  %nvt.tg16 = shufflevector <3 x i16> {name}, <3 x i16> poison, <2 x i32> <i32 0, i32 1>"));
                tg_lines.push("  %nvt.tg = zext <2 x i16> %nvt.tg16 to <2 x i32>".to_string());
            }
            t => return Err(format!("{entry}: a {t} threadgroup_position_in_grid (the tile emulation needs both coordinates)")),
        }
    }

    // prologue after the entry label
    let mut prologue = tg_lines;
    prologue.extend([
        "  %nvt.tp = load <4 x i32>, ptr addrspace(2) %nvt.tile, align 16".to_string(),
        "  %nvt.tw = extractelement <4 x i32> %nvt.tp, i64 0".to_string(),
        "  %nvt.th = extractelement <4 x i32> %nvt.tp, i64 1".to_string(),
        "  %nvt.tsz = shufflevector <4 x i32> %nvt.tp, <4 x i32> poison, <2 x i32> <i32 0, i32 1>".to_string(),
        "  %nvt.org = mul <2 x i32> %nvt.tg, %nvt.tsz".to_string(),
    ]);
    if used != 0 {
        prologue.push("  %nvt.smp = tail call ptr addrspace(2) @air.get_read_sampler()".to_string());
    }
    let first = body.iter().position(|l| !l.trim().is_empty()).unwrap_or(0);
    let label_line = body.get(first).is_some_and(|l| {
        let t = l.split(';').next().unwrap_or("").trim();
        t.ends_with(':') && !t.contains(' ')
    });
    let insert_at = if label_line { first + 1 } else { 0 };
    for (k, p) in prologue.into_iter().enumerate() {
        body.insert(insert_at + k, p);
    }

    // the define line: new parameters after the function's own
    let mut nparam = own.len() as u32;
    let mut new_params: Vec<String> = Vec::new();
    let mut new_nodes: Vec<String> = Vec::new();
    let planes: Vec<u32> = (0..MAX_ATTACHMENTS).filter(|a| used & (1 << a) != 0).collect();
    for &a in &planes {
        let class = opts.classes[a as usize].expect("checked when the access was lowered");
        new_params.push(format!("ptr addrspace(1) %nvt.plane.{a}"));
        new_nodes.push(format!(
            "!{{i32 {nparam}, !\"air.texture\", !\"air.location_index\", i32 {}, i32 1, !\"air.read_write\", !\"air.arg_type_name\", !\"{}\", !\"air.arg_name\", !\"nvt_plane{a}\"}}",
            PLANE_TEXTURE_BASE + a,
            class.type_name()
        ));
        nparam += 1;
    }
    if tg_from.is_none() {
        new_params.push("<2 x i32> %nvt.tg".to_string());
        new_nodes.push(format!(
            "!{{i32 {nparam}, !\"air.threadgroup_position_in_grid\", !\"air.arg_type_name\", !\"uint2\", !\"air.arg_name\", !\"nvt_tg\"}}"
        ));
        nparam += 1;
    }
    new_params.push("ptr addrspace(2) %nvt.tile".to_string());
    new_nodes.push(format!(
        "!{{i32 {nparam}, !\"air.buffer\", !\"air.buffer_size\", i32 16, !\"air.location_index\", i32 {TILE_PARAM_BUFFER}, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 16, !\"air.arg_type_align_size\", i32 16, !\"air.arg_type_name\", !\"uint4\", !\"air.arg_name\", !\"nvt_tile\"}}"
    ));
    let joined = if own.is_empty() { new_params.join(", ") } else { format!("{}, {}", def[open + 1..close].trim_end(), new_params.join(", ")) };
    let new_def = format!("{}{}{}", &def[..open + 1], joined, &def[close..]);

    // metadata: a fresh argument list (the old one may be shared) holding the old arguments and the new ones
    let mut next = lines.iter().filter_map(|l| metadata_number(l)).max().unwrap_or(0) + 1;
    let mut node_lines: Vec<String> = Vec::new();
    let mut new_refs: Vec<String> = arg_nodes.iter().map(|n| format!("!{n}")).collect();
    for body in new_nodes {
        node_lines.push(format!("!{next} = {body}"));
        new_refs.push(format!("!{next}"));
        next += 1;
    }
    let list_num = next;
    node_lines.push(format!("!{list_num} = !{{{}}}", new_refs.join(", ")));
    let kernel_at = find_node(&lines, knum).expect("found above");
    let old_kernel = lines[kernel_at].clone();
    let needle = format!("!{args_node}");
    // replace the third element (the argument list) of the kernel node
    let kbody_start = old_kernel.find(" = ").unwrap() + 3;
    let kbody = &old_kernel[kbody_start..];
    let elems = split_top_level_commas(kbody.trim().trim_start_matches("!{").trim_end_matches('}'));
    let replaced: Vec<String> = elems
        .iter()
        .enumerate()
        .map(|(i, e)| if i == 2 && e.trim() == needle { format!("!{list_num}") } else { e.clone() })
        .collect();
    lines[kernel_at] = format!("{}!{{{}}}", &old_kernel[..kbody_start], replaced.join(", "));

    // declarations: drop the lowered intrinsics, add the texture ones
    let lowered_decl = |l: &str| {
        l.starts_with("declare ")
            && ["@air.load.implicit_imageblock.", "@air.store.implicit_imageblock.", "@air.get_imageblock_num_colors(",
                "@air.get_color_coverage_mask(", "@air.get_imageblock_width(", "@air.get_imageblock_height(",
                "@air.get_imageblock_samples("]
                .iter()
                .any(|n| l.contains(n))
    };
    let mut decls: Vec<String> = Vec::new();
    let has_decl = |lines: &[String], name: &str| lines.iter().any(|l| l.starts_with("declare ") && l.contains(&format!("@{name}(")));
    for c in [PlaneClass::Float, PlaneClass::Uint, PlaneClass::Sint] {
        if !rw.classes_used.contains(&(c as u8)) {
            continue;
        }
        if !has_decl(&lines, &c.read_fn()) {
            decls.push(c.read_decl());
        }
        if !has_decl(&lines, &c.write_fn()) {
            decls.push(c.write_decl());
        }
    }
    if used != 0 && !has_decl(&lines, "air.get_read_sampler") {
        decls.push("declare ptr addrspace(2) @air.get_read_sampler()".to_string());
    }

    let mut out = String::with_capacity(ll.len() + 4096);
    for (k, l) in lines.iter().enumerate() {
        if k == d {
            out.push_str(&new_def);
            out.push('\n');
            for b in &body {
                out.push_str(b);
                out.push('\n');
            }
            continue;
        }
        if k > d && k < end {
            continue;
        }
        if k == end {
            out.push_str("}\n");
            for dcl in &decls {
                out.push_str(dcl);
                out.push('\n');
            }
            continue;
        }
        if lowered_decl(l) {
            continue;
        }
        out.push_str(l);
        out.push('\n');
    }
    for n in node_lines {
        out.push_str(&n);
        out.push('\n');
    }
    Ok(LoweredTile {
        ll: out,
        reads: rw.reads,
        writes: rw.writes,
        explicit_imageblock: ll.contains("!\"air.imageblock\", !\"explicit\""),
    })
}

#[cfg(test)]
mod tile_lower_tests {
    use super::*;
    const LIGHT: &str = include_str!("testdata/tile_light.ll");
    const MS: &str = include_str!("testdata/tile_ms.ll");
    const F32: &str = include_str!("testdata/tile_f32.ll");
    const TG: &str = include_str!("testdata/tile_tg.ll");
    const EXPLICIT: &str = include_str!("testdata/tile_explicit.ll");
    const MISC: &str = include_str!("testdata/tile_misc.ll");

    fn opts(samples: u32, classes: &[(u32, PlaneClass)]) -> TileLowerOptions {
        let mut c = [None; 8];
        for (a, k) in classes {
            c[*a as usize] = Some(*k);
        }
        TileLowerOptions { samples, classes: c }
    }

    fn translates(ll: &str) {
        assert_eq!(ll.matches("!\"air.threadgroup_position_in_grid\"").count(), 1, "one threadgroup position input:\n{ll}");
        let spv = crate::translate_native_no_retry(ll, crate::passes::Stage::Kernel)
            .unwrap_or_else(|e| panic!("the lowered tile kernel does not translate: {e}\n{ll}"));
        assert!(spv.len() > 20);
        // spirv-val where it is installed (the duplicate-builtin rule the translator does not check itself)
        let path = std::env::temp_dir().join(format!("tile_lower_{}_{}.spv", std::process::id(), spv.len()));
        if std::fs::write(&path, &spv).is_ok() {
            if let Ok(out) = std::process::Command::new("spirv-val").args(["--target-env", "vulkan1.2"]).arg(&path).output() {
                assert!(out.status.success(), "spirv-val: {}", String::from_utf8_lossy(&out.stderr));
            }
            let _ = std::fs::remove_file(&path);
        }
        // every plane is a storage image at the storage-texture binding of its texture index
        let refl = crate::reflect_sanitized(ll, crate::passes::Stage::Kernel, crate::passes::TransformOptions::default())
            .expect("the lowered tile kernel reflects");
        for b in &refl.bindings {
            if b.metal_index >= PLANE_TEXTURE_BASE && matches!(b.kind, crate::reflect::ResourceKind::StorageImage) {
                assert_eq!(b.descriptor.as_ref().map(|d| d.binding), Some(480 + b.metal_index), "{b:?}");
            }
        }
        assert!(refl.implicit_imageblock_attachments.is_empty(), "no implicit imageblock plane is left for the plugin");
    }

    #[test]
    fn deferred_lighting_reads_two_planes_and_writes_three() {
        use PlaneClass::Float;
        let t = lower_tile(LIGHT, "tile_light", &opts(1, &[(0, Float), (1, Float), (2, Float)])).expect("lowers");
        assert!(!t.ll.contains("implicit_imageblock"), "every implicit access is lowered:\n{}", t.ll);
        assert_eq!(t.writes, 0b111);
        assert_eq!(t.reads & 0b11, 0b11);
        for a in 0..3 {
            assert!(t.ll.contains(&format!("i32 {}, i32 1, !\"air.read_write\"", PLANE_TEXTURE_BASE + a)), "plane {a} metadata");
        }
        assert!(t.ll.contains("!\"air.threadgroup_position_in_grid\""));
        assert!(t.ll.contains(&format!("i32 {TILE_PARAM_BUFFER}, i32 1, !\"air.read\"")));
        translates(&t.ll);
    }

    #[test]
    fn per_sample_colours_and_coverage_masks_unroll_over_the_samples() {
        let t = lower_tile(MS, "tile_ms", &opts(4, &[(0, PlaneClass::Float)])).expect("lowers");
        assert!(!t.ll.contains("@air.get_imageblock_num_colors("));
        assert_eq!(t.ll.matches("@air.write_texture_2d_array.v4f32(").count(), 4 + 1, "four masked writes and the declaration");
        assert!(t.ll.contains("= add i16 4, 0"), "num_colors is the sample count");
        assert!(t.ll.contains("= shl i16 1, %i.017"), "colour i covers sample i");
        translates(&t.ll);
    }

    #[test]
    fn integer_and_float_attachments_keep_their_classes() {
        let t = lower_tile(F32, "tile_f32", &opts(1, &[(0, PlaneClass::Float), (1, PlaneClass::Uint)])).expect("lowers");
        assert!(t.ll.contains("@air.read_texture_2d_array.u.v4i32("));
        assert!(t.ll.contains("@air.read_texture_2d_array.v4f32("));
        assert!(t.ll.contains("trunc i32 %nvt.tw to i16"), "get_imageblock_width is the tile width");
        translates(&t.ll);
        let wrong = lower_tile(F32, "tile_f32", &opts(1, &[(0, PlaneClass::Uint), (1, PlaneClass::Uint)]));
        assert!(wrong.is_err(), "a half/float value on an integer attachment is refused");
        let missing = lower_tile(F32, "tile_f32", &opts(1, &[(0, PlaneClass::Float)]));
        assert!(missing.unwrap_err().contains("colour attachment 1"));
    }

    #[test]
    fn threadgroup_memory_and_barriers_order_the_planes() {
        let t = lower_tile(TG, "tile_tg", &opts(1, &[(0, PlaneClass::Float)])).expect("lowers");
        assert!(t.ll.contains("@air.wg.barrier(i32 6, i32 5, i32 1)"), "threadgroup barriers also order images:\n{}", t.ll);
        translates(&t.ll);
    }

    #[test]
    fn explicit_imageblocks_stay_per_threadgroup() {
        let t = lower_tile(EXPLICIT, "tile_explicit", &opts(1, &[])).expect("lowers");
        assert!(t.explicit_imageblock);
        assert_eq!(t.reads | t.writes, 0);
        assert!(t.ll.contains("@air.wg.barrier(i32 12, i32 5, i32 1)"));
        translates(&t.ll);
    }

    #[test]
    fn narrow_types_sample_rates_and_sample_count() {
        use PlaneClass::*;
        let t = lower_tile(MISC, "t_misc", &opts(2, &[(0, Float), (1, Float), (2, Sint)])).expect("lowers");
        assert!(t.ll.contains("= add i16 2, 0"), "get_imageblock_samples is the sample count");
        assert!(t.ll.contains("sext i16"), "a short attachment is sign-extended into an int plane");
        translates(&t.ll);
    }

    #[test]
    fn reserved_indices_are_refused() {
        let ll = LIGHT.replace("!\"air.location_index\", i32 0, i32 1, !\"air.read\"", "!\"air.location_index\", i32 30, i32 1, !\"air.read\"");
        let e = lower_tile(&ll, "tile_light", &opts(1, &[(0, PlaneClass::Float), (1, PlaneClass::Float), (2, PlaneClass::Float)]))
            .unwrap_err();
        assert!(e.contains("index 30"), "{e}");
    }
}
