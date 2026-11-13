//! Metal sparse textures, tier 2: per-tile access counters, emulated (`TransformOptions::sparse_access_counters`, the
//! plugin's NVMTL_SPARSE_TIER2=1).
//!
//! Metal's `MTLTextureSparseTier2` adds one thing to tier 1: per-tile access counters, read with the blit encoder's
//! `getTextureAccessCounters:region:mipLevel:slice:resetCounters:countersBuffer:countersBufferOffset:` (one uint32 per
//! tile) and cleared with `resetTextureAccessCounters:`. Vulkan has no such query and Xe2's page tables keep no
//! accessed bit per tile, so every texel read a shader makes of a texture that can be sparse counts itself: the
//! instruction's tile (level, slice, tile x/y/z from its coordinates and the tile shape) gets an atomic add in a counter
//! buffer the plugin owns.
//!
//! Which reads: sample, gather, fetch (`read`) and storage-image read instructions, plain or OpImageSparse* (an M1 Pro
//! counts a plain `sample()` too), of a 2D, 2D-array, 3D, cube or cube-array image, single-sampled, that the module
//! loads from the bindless heap (set 2, binding 0 sampled or 2 storage): the heap slot is the texture's identity at run
//! time, the same for a texture bound directly (bindless-all) and one in an argument buffer. Depth-compare reads are left
//! alone (depth textures cannot be sparse in the plugin), and so is anything not loaded from the heap (a texture at
//! Metal index 128 or above, a directly bound read-write texture, NVMTL_NO_BINDLESS_ALL=1).
//!
//! The plugin's side (nullmoth/plugin/NVMTLSparse.m, nvmtl_sparse_vk.c) keeps heap slot [`COUNTER_HEAP_SLOT`] for itself:
//! - its uniform texel buffer (binding 3, RGBA32UI) is the directory: element `s` describes the texture at heap slot
//!   `s` ([`DIRECTORY_SLOTS`] of them), all zero for a texture without counters:
//!   x the word of the texture's header in the counter buffer (0: not counted), y width | height << 16,
//!   z depth | slices << 16 (slices: array length, x6 for cubes), w levels | log2(tile width) << 5 |
//!   log2(tile height) << 9 | log2(tile depth) << 13;
//! - its storage texel buffer (binding 4, R32UI) is the counter buffer: at the header word, [`HEADER_WORDS`] words
//!   (word m < 15: where level m's counters start, from the end of the header; word 15: how many counters follow), then
//!   the counters, level by level, each level slice by slice, then z, y, x, one uint32 per tile.
//!
//! Each counted instruction calls one helper function (emitted once per module) with the slot, level, slice and
//! coordinates. The helper reads the directory element and returns at once when it is zero (every texture that is not a
//! tier-2 sparse texture); otherwise it finds the counter, and the lanes of the subgroup that hit the same counter add
//! once (a ballot and one atomic for the lanes that agree with the first active lane; the others add 1 each), so the
//! usual case - a subgroup inside one 128x128 tile - is one atomic per subgroup. A count is one per invocation, which is
//! what an M1 Pro's fragment shaders count (one per pixel). Helper invocations count nothing. Coordinates are clamped to
//! the level, the counter index to the texture's counters: a stray value counts a wrong tile of the same texture, never
//! memory outside it.
//!
//! The level of a read: `level()` (rounded), `gradient*()` (log2 of the longest scaled gradient), the implicit LOD of a
//! fragment shader (OpImageQueryLod, + bias, at least min_lod_clamp), level 0 for gathers, storage reads and implicit
//! samples outside fragment shaders, the LOD operand of a fetch. Normalized coordinates are scaled by the level's size
//! and clamped (a repeating sampler's wrapped coordinate counts the edge tile); a pixel-coordinate sampler's coordinates
//! are taken as normalized too. A cube direction picks its face as Vulkan does (major axis).
//!
//! With the option off nothing here runs, and with it on a module that reads no heap texture comes out unchanged.
use crate::fxhash::HashMap;
use crate::spirv_module::{Block, Function, Instruction, Module, Operand};
use spirv::{
    BuiltIn, Capability, Decoration, Dim, ExecutionModel, FunctionControl, GroupOperation, ImageFormat, ImageOperands, Op,
    Scope, SelectionControl, StorageClass, Word,
};

/// The heap slot the plugin keeps for the directory (binding 3) and the counter buffer (binding 4): the last of its
/// 32768 heap slots (NVMTL_BINDLESS_SLOTS - 1 in nullmoth/plugin/nvmtl_vk.c).
pub const COUNTER_HEAP_SLOT: u32 = 32767;
/// Directory elements: one per plugin heap slot. A slot index past the last reads the reserved slot's element (zero).
pub const DIRECTORY_SLOTS: u32 = COUNTER_HEAP_SLOT + 1;
/// Words of a texture's header in the counter buffer: 15 level starts and the counter count.
pub const HEADER_WORDS: u32 = 16;
/// Levels a header describes (a 16384-texel texture has 15).
pub const MAX_LEVELS: u32 = 15;
/// The helper's OpName.
pub const HELPER_NAME: &str = "nvmtl_sparse_count";

const HEAP_SET: u32 = crate::reflect::BINDLESS_HEAP_SET;
const DIRECTORY_BINDING: u32 = crate::reflect::BINDLESS_UTEXEL_BINDING;
const COUNTER_BINDING: u32 = crate::reflect::BINDLESS_STEXEL_BINDING;

// GLSL.std.450
const G_FABS: u32 = 4;
const G_LOG2: u32 = 30;
const G_UMIN: u32 = 38;
const G_FMAX: u32 = 40;
const G_UMAX: u32 = 41;
const G_SMAX: u32 = 42;
const G_LENGTH: u32 = 66;
const G_NCLAMP: u32 = 81;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    /// normalized coordinates through a sampler, the level from the instruction
    Sample { implicit: bool },
    /// normalized coordinates, level 0
    Gather,
    /// texel coordinates, the level from a Lod operand
    Fetch,
    /// texel coordinates of a storage image, level 0
    Read,
}

fn kind(op: Op) -> Option<Kind> {
    Some(match op {
        Op::ImageSampleImplicitLod | Op::ImageSparseSampleImplicitLod => Kind::Sample { implicit: true },
        Op::ImageSampleExplicitLod | Op::ImageSparseSampleExplicitLod => Kind::Sample { implicit: false },
        Op::ImageGather | Op::ImageSparseGather => Kind::Gather,
        Op::ImageFetch | Op::ImageSparseFetch => Kind::Fetch,
        Op::ImageRead | Op::ImageSparseRead => Kind::Read,
        _ => return None,
    })
}

fn id(o: Option<&Operand>) -> Option<Word> {
    match o {
        Some(Operand::IdRef(v)) => Some(*v),
        _ => None,
    }
}
fn lit(o: Option<&Operand>) -> Option<u32> {
    match o {
        Some(Operand::LiteralBit32(v)) => Some(*v),
        _ => None,
    }
}

/// A scalar component type of a coordinate or operand
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Scalar {
    F32,
    F16,
    I32,
    U32,
}

/// One counted instruction
struct Site {
    function: usize,
    block: usize,
    at: usize,
    kind: Kind,
    slot: Word,
    slot_signed: bool,
    /// the instruction's image operand: a sampled image (sample, gather) or an image (fetch, read)
    image_operand: Word,
    /// the image operand is an OpTypeSampledImage value
    sampled: bool,
    /// the image type (OpTypeImage)
    image_ty: Word,
    dim: Dim,
    arrayed: bool,
    coord: Word,
    coord_scalar: Scalar,
    coord_count: u32,
    bias: Option<Word>,
    lod: Option<(Word, Scalar)>,
    /// dPdx, dPdy and their component count (32-bit float)
    grad: Option<(Word, Word, u32)>,
    min_lod: Option<Word>,
}

/// The ids of the image operands an instruction carries after its mask, by the mask's bits in order
fn image_operand_ids(operands: &[Operand], mask_at: usize) -> Option<(ImageOperands, Vec<Word>)> {
    let Some(Operand::ImageOperands(mask)) = operands.get(mask_at) else { return None };
    let ids = operands[mask_at + 1..].iter().filter_map(|o| id(Some(o))).collect();
    Some((*mask, ids))
}

struct Globals {
    void: Word,
    bool_: Word,
    uint: Word,
    int: Word,
    float: Word,
    v2float: Word,
    v3float: Word,
    glsl: Word,
    helper: Word,
}

/// get-or-create of types and constants, appended to the module's globals
struct Builder<'m> {
    m: &'m mut Module,
    consts: HashMap<(Word, u32), Word>,
}

impl<'m> Builder<'m> {
    fn new(m: &'m mut Module) -> Self {
        let mut consts = HashMap::default();
        for i in &m.types_global_values {
            if let (Op::Constant, Some(ty), Some(r), [Operand::LiteralBit32(v)]) =
                (i.class.opcode, i.result_type, i.result_id, i.operands.as_slice())
            {
                consts.entry((ty, *v)).or_insert(r);
            }
        }
        Self { m, consts }
    }
    fn ty(&mut self, op: Op, operands: Vec<Operand>) -> Word {
        if let Some(r) = self
            .m
            .types_global_values
            .iter()
            .find(|i| i.class.opcode == op && i.result_type.is_none() && i.operands == operands)
            .and_then(|i| i.result_id)
        {
            return r;
        }
        let r = self.m.fresh_id();
        self.m.types_global_values.push(Instruction::new(op, None, Some(r), operands));
        r
    }
    fn vec(&mut self, elem: Word, n: u32) -> Word {
        self.ty(Op::TypeVector, vec![Operand::IdRef(elem), Operand::LiteralBit32(n)])
    }
    fn ptr(&mut self, sc: StorageClass, pointee: Word) -> Word {
        self.ty(Op::TypePointer, vec![Operand::StorageClass(sc), Operand::IdRef(pointee)])
    }
    fn constant(&mut self, ty: Word, bits: u32) -> Word {
        if let Some(r) = self.consts.get(&(ty, bits)) {
            return *r;
        }
        let r = self.m.fresh_id();
        self.m.types_global_values.push(Instruction::new(Op::Constant, Some(ty), Some(r), vec![Operand::LiteralBit32(bits)]));
        self.consts.insert((ty, bits), r);
        r
    }
    fn composite(&mut self, ty: Word, parts: &[Word]) -> Word {
        let r = self.m.fresh_id();
        self.m.types_global_values.push(Instruction::new(
            Op::ConstantComposite,
            Some(ty),
            Some(r),
            parts.iter().map(|p| Operand::IdRef(*p)).collect(),
        ));
        r
    }
    fn boolean(&mut self, ty: Word, v: bool) -> Word {
        let op = if v { Op::ConstantTrue } else { Op::ConstantFalse };
        if let Some(r) = self
            .m
            .types_global_values
            .iter()
            .find(|i| i.class.opcode == op && i.result_type == Some(ty))
            .and_then(|i| i.result_id)
        {
            return r;
        }
        let r = self.m.fresh_id();
        self.m.types_global_values.push(Instruction::new(op, Some(ty), Some(r), vec![]));
        r
    }
    fn capability(&mut self, c: Capability) {
        if !self.m.capabilities.iter().any(|i| i.operands.first() == Some(&Operand::Capability(c))) {
            self.m.capabilities.push(Instruction::new(Op::Capability, None, None, vec![Operand::Capability(c)]));
        }
    }
    fn extension(&mut self, name: &str) {
        if !self.m.extensions.iter().any(|i| matches!(i.operands.first(), Some(Operand::LiteralString(s)) if s == name)) {
            self.m.extensions.push(Instruction::new(Op::Extension, None, None, vec![Operand::LiteralString(name.into())]));
        }
    }
    fn glsl(&mut self) -> Word {
        if let Some(r) = self.m.ext_inst_imports.iter().find_map(|i| match i.operands.first() {
            Some(Operand::LiteralString(s)) if s == "GLSL.std.450" => i.result_id,
            _ => None,
        }) {
            return r;
        }
        let r = self.m.fresh_id();
        self.m.ext_inst_imports.push(Instruction::new(
            Op::ExtInstImport,
            None,
            Some(r),
            vec![Operand::LiteralString("GLSL.std.450".into())],
        ));
        r
    }
    fn decorate(&mut self, target: Word, d: Decoration, extra: Vec<Operand>) {
        let mut operands = vec![Operand::IdRef(target), Operand::Decoration(d)];
        operands.extend(extra);
        self.m.annotations.push(Instruction::new(Op::Decorate, None, None, operands));
    }
    fn variable(&mut self, ptr: Word, sc: StorageClass) -> Word {
        let r = self.m.fresh_id();
        self.m.types_global_values.push(Instruction::new(Op::Variable, Some(ptr), Some(r), vec![Operand::StorageClass(sc)]));
        r
    }
}

/// Straight-line code: instructions with fresh ids
struct Emit<'a, 'm> {
    b: &'a mut Builder<'m>,
    out: Vec<Instruction>,
}

impl Emit<'_, '_> {
    fn op(&mut self, op: Op, ty: Word, operands: Vec<Operand>) -> Word {
        let r = self.b.m.fresh_id();
        self.out.push(Instruction::new(op, Some(ty), Some(r), operands));
        r
    }
    fn ids(&mut self, op: Op, ty: Word, ids: &[Word]) -> Word {
        self.op(op, ty, ids.iter().map(|i| Operand::IdRef(*i)).collect())
    }
    fn ext(&mut self, glsl: Word, ty: Word, n: u32, args: &[Word]) -> Word {
        let mut operands = vec![Operand::IdRef(glsl), Operand::LiteralExtInstInteger(n)];
        operands.extend(args.iter().map(|a| Operand::IdRef(*a)));
        self.op(Op::ExtInst, ty, operands)
    }
    fn extract(&mut self, ty: Word, v: Word, i: u32) -> Word {
        self.op(Op::CompositeExtract, ty, vec![Operand::IdRef(v), Operand::LiteralBit32(i)])
    }
    fn none(&mut self, op: Op, operands: Vec<Operand>) {
        self.out.push(Instruction::new(op, None, None, operands));
    }
}

/// Counts the texel reads of heap textures (see the module comment). Returns how many instructions now count; 0 leaves
/// the module as it was.
pub(crate) fn instrument(m: &mut Module, fragment: bool) -> Result<usize, String> {
    let sites = find_sites(m);
    if sites.is_empty() {
        return Ok(0);
    }
    let fragment = fragment
        && m.entry_points.iter().any(|e| e.operands.first() == Some(&Operand::ExecutionModel(ExecutionModel::Fragment)));
    let mut b = Builder::new(m);
    let g = globals(&mut b, fragment, &sites)?;
    let n = sites.len();
    // later sites of a block first: the earlier indices stay valid
    let mut sites = sites;
    sites.sort_by(|x, y| (x.function, x.block, std::cmp::Reverse(x.at)).cmp(&(y.function, y.block, std::cmp::Reverse(y.at))));
    for site in &sites {
        let code = call_site(&mut b, &g, site, fragment)?;
        let block = &mut b.m.functions[site.function].blocks[site.block];
        block.instructions.splice(site.at..site.at, code);
    }
    Ok(n)
}

fn find_sites(m: &Module) -> Vec<Site> {
    let mut binding: HashMap<Word, (Option<u32>, Option<u32>)> = HashMap::default();
    for a in &m.annotations {
        if a.class.opcode != Op::Decorate {
            continue;
        }
        let (Some(target), Some(Operand::Decoration(d))) = (id(a.operands.first()), a.operands.get(1)) else { continue };
        let e = binding.entry(target).or_default();
        match d {
            Decoration::DescriptorSet => e.0 = lit(a.operands.get(2)),
            Decoration::Binding => e.1 = lit(a.operands.get(2)),
            _ => {}
        }
    }
    let heap = |v: Word| {
        matches!(binding.get(&v), Some((Some(s), Some(bd))) if *s == HEAP_SET
            && (*bd == crate::reflect::BINDLESS_HEAP_BINDING || *bd == crate::reflect::BINDLESS_STORAGE_BINDING))
    };
    let globals: HashMap<Word, &Instruction> =
        m.types_global_values.iter().filter_map(|i| Some((i.result_id?, i))).collect();
    let scalar_of = |ty: Word| -> Option<Scalar> {
        let t = globals.get(&ty)?;
        match (t.class.opcode, lit(t.operands.first()), lit(t.operands.get(1))) {
            (Op::TypeFloat, Some(32), _) => Some(Scalar::F32),
            (Op::TypeFloat, Some(16), _) => Some(Scalar::F16),
            (Op::TypeInt, Some(32), Some(0)) => Some(Scalar::U32),
            (Op::TypeInt, Some(32), Some(_)) => Some(Scalar::I32),
            _ => None,
        }
    };
    let shape_of = |ty: Word| -> Option<(Scalar, u32)> {
        let t = globals.get(&ty)?;
        if t.class.opcode == Op::TypeVector {
            Some((scalar_of(id(t.operands.first())?)?, lit(t.operands.get(1))?))
        } else {
            Some((scalar_of(ty)?, 1))
        }
    };
    let mut sites = Vec::new();
    for (fi, f) in m.functions.iter().enumerate() {
        let local: HashMap<Word, &Instruction> =
            f.all_inst_iter().filter_map(|i| Some((i.result_id?, i))).collect();
        let type_of = |v: Word| local.get(&v).or_else(|| globals.get(&v)).and_then(|i| i.result_type);
        for (bi, block) in f.blocks.iter().enumerate() {
            for (ii, inst) in block.instructions.iter().enumerate() {
                let Some(kind) = kind(inst.class.opcode) else { continue };
                let (Some(image_operand), Some(coord)) = (id(inst.operands.first()), id(inst.operands.get(1))) else {
                    continue;
                };
                // the image operand back to `OpLoad (OpAccessChain heap slot)`
                let mut v = image_operand;
                let mut found = None;
                for _ in 0..8 {
                    let Some(d) = local.get(&v) else { break };
                    match d.class.opcode {
                        Op::SampledImage | Op::Image | Op::CopyObject => {
                            let Some(next) = id(d.operands.first()) else { break };
                            v = next;
                        }
                        Op::Load => {
                            let Some(chain) = id(d.operands.first()).and_then(|p| local.get(&p)) else { break };
                            if matches!(chain.class.opcode, Op::AccessChain | Op::InBoundsAccessChain)
                                && chain.operands.len() == 2
                                && id(chain.operands.first()).is_some_and(heap)
                            {
                                found = id(chain.operands.get(1)).zip(d.result_type);
                            }
                            break;
                        }
                        _ => break,
                    }
                }
                let Some((slot, image_ty)) = found else { continue };
                let Some(slot_scalar) = type_of(slot).and_then(scalar_of) else { continue };
                if !matches!(slot_scalar, Scalar::U32 | Scalar::I32) {
                    continue;
                }
                let Some(t) = globals.get(&image_ty).filter(|t| t.class.opcode == Op::TypeImage) else { continue };
                let Some(Operand::Dim(dim)) = t.operands.get(1) else { continue };
                let (arrayed, ms) = (lit(t.operands.get(3)) == Some(1), lit(t.operands.get(4)) == Some(1));
                if ms || !matches!(dim, Dim::Dim2D | Dim::Dim3D | Dim::DimCube) || (*dim == Dim::Dim3D && arrayed) {
                    continue;
                }
                let need = match dim {
                    Dim::Dim2D => 2,
                    _ => 3,
                } + u32::from(arrayed);
                let Some((coord_scalar, coord_count)) = type_of(coord).and_then(shape_of) else { continue };
                if coord_count < need {
                    continue;
                }
                let texel = matches!(kind, Kind::Fetch | Kind::Read);
                if texel != matches!(coord_scalar, Scalar::I32 | Scalar::U32) {
                    continue;
                }
                if *dim == Dim::DimCube && texel {
                    continue;
                }
                let mask_at = if kind == Kind::Gather { 3 } else { 2 };
                let (mut bias, mut lod, mut grad, mut min_lod) = (None, None, None, None);
                if let Some((mask, ids)) = image_operand_ids(&inst.operands, mask_at) {
                    let mut at = 0usize;
                    let mut take = |n: usize| {
                        let s = ids.get(at..at + n).map(<[Word]>::to_vec);
                        at += n;
                        s
                    };
                    let order: [(ImageOperands, usize); 10] = [
                        (ImageOperands::BIAS, 1),
                        (ImageOperands::LOD, 1),
                        (ImageOperands::GRAD, 2),
                        (ImageOperands::CONST_OFFSET, 1),
                        (ImageOperands::OFFSET, 1),
                        (ImageOperands::CONST_OFFSETS, 1),
                        (ImageOperands::SAMPLE, 1),
                        (ImageOperands::MIN_LOD, 1),
                        (ImageOperands::MAKE_TEXEL_AVAILABLE, 1),
                        (ImageOperands::MAKE_TEXEL_VISIBLE, 1),
                    ];
                    for (bit, n) in order {
                        if !mask.contains(bit) {
                            continue;
                        }
                        let Some(v) = take(n) else { break };
                        if bit == ImageOperands::BIAS {
                            bias = Some(v[0]);
                        } else if bit == ImageOperands::LOD {
                            lod = type_of(v[0]).and_then(scalar_of).map(|s| (v[0], s));
                        } else if bit == ImageOperands::GRAD {
                            grad = Some((v[0], v[1]));
                        } else if bit == ImageOperands::MIN_LOD {
                            min_lod = Some(v[0]);
                        } else if bit == ImageOperands::SAMPLE {
                            // a multisampled read: not a texture that can be sparse
                            lod = None;
                        }
                    }
                }
                // a float operand that is not 32-bit (half bias/LOD) is not taken apart here
                let f32_or_none = |v: Option<Word>| v.is_none_or(|v| type_of(v).and_then(scalar_of) == Some(Scalar::F32));
                if !f32_or_none(bias) || !f32_or_none(min_lod) || lod.is_some_and(|(_, s)| s == Scalar::F16) {
                    continue;
                }
                let grad = match grad {
                    Some((dx, dy)) => match (type_of(dx).and_then(shape_of), type_of(dy).and_then(shape_of)) {
                        (Some((Scalar::F32, n)), Some((Scalar::F32, n2))) if n == n2 => Some((dx, dy, n)),
                        _ => continue,
                    },
                    None => None,
                };
                let sampled = type_of(image_operand)
                    .and_then(|t| globals.get(&t))
                    .is_some_and(|t| t.class.opcode == Op::TypeSampledImage);
                if matches!(kind, Kind::Sample { .. } | Kind::Gather) != sampled {
                    continue;
                }
                sites.push(Site {
                    function: fi,
                    block: bi,
                    at: ii,
                    kind,
                    slot,
                    slot_signed: slot_scalar == Scalar::I32,
                    image_operand,
                    sampled,
                    image_ty,
                    dim: *dim,
                    arrayed,
                    coord,
                    coord_scalar,
                    coord_count,
                    bias,
                    lod,
                    grad,
                    min_lod,
                });
            }
        }
    }
    sites
}

/// The helper and everything it reads: types, the directory and counter variables, the HelperInvocation input of a
/// fragment shader, capabilities.
fn globals(b: &mut Builder, fragment: bool, sites: &[Site]) -> Result<Globals, String> {
    let void = b.ty(Op::TypeVoid, vec![]);
    let bool_ = b.ty(Op::TypeBool, vec![]);
    let uint = b.ty(Op::TypeInt, vec![Operand::LiteralBit32(32), Operand::LiteralBit32(0)]);
    let int = b.ty(Op::TypeInt, vec![Operand::LiteralBit32(32), Operand::LiteralBit32(1)]);
    let float = b.ty(Op::TypeFloat, vec![Operand::LiteralBit32(32)]);
    let v2float = b.vec(float, 2);
    let v3float = b.vec(float, 3);
    let v3uint = b.vec(uint, 3);
    let v3bool = b.vec(bool_, 3);
    let v4uint = b.vec(uint, 4);
    let glsl = b.glsl();
    for c in [
        Capability::SampledBuffer,
        Capability::ImageBuffer,
        Capability::GroupNonUniform,
        Capability::GroupNonUniformBallot,
        Capability::RuntimeDescriptorArray,
    ] {
        b.capability(c);
    }
    if b.m.header.as_ref().is_some_and(|h| h.version() < (1, 5)) {
        b.extension("SPV_EXT_descriptor_indexing");
    }
    if sites.iter().any(|s| s.grad.is_some() || (fragment && s.kind == Kind::Sample { implicit: true })) {
        b.capability(Capability::ImageQuery);
    }
    // the directory (uniform texel, RGBA32UI) and the counters (storage texel, R32UI) of the reserved heap slot
    let dir_img = b.ty(
        Op::TypeImage,
        vec![
            Operand::IdRef(uint),
            Operand::Dim(Dim::DimBuffer),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(1),
            Operand::ImageFormat(ImageFormat::Unknown),
        ],
    );
    let cnt_img = b.ty(
        Op::TypeImage,
        vec![
            Operand::IdRef(uint),
            Operand::Dim(Dim::DimBuffer),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(0),
            Operand::LiteralBit32(2),
            Operand::ImageFormat(ImageFormat::R32ui),
        ],
    );
    let dir_arr = b.ty(Op::TypeRuntimeArray, vec![Operand::IdRef(dir_img)]);
    let cnt_arr = b.ty(Op::TypeRuntimeArray, vec![Operand::IdRef(cnt_img)]);
    let dir_arr_ptr = b.ptr(StorageClass::UniformConstant, dir_arr);
    let cnt_arr_ptr = b.ptr(StorageClass::UniformConstant, cnt_arr);
    let dir_ptr = b.ptr(StorageClass::UniformConstant, dir_img);
    let cnt_ptr = b.ptr(StorageClass::UniformConstant, cnt_img);
    let texel_ptr = b.ptr(StorageClass::Image, uint);
    let dir_var = b.variable(dir_arr_ptr, StorageClass::UniformConstant);
    let cnt_var = b.variable(cnt_arr_ptr, StorageClass::UniformConstant);
    for (v, binding) in [(dir_var, DIRECTORY_BINDING), (cnt_var, COUNTER_BINDING)] {
        b.decorate(v, Decoration::DescriptorSet, vec![Operand::LiteralBit32(HEAP_SET)]);
        b.decorate(v, Decoration::Binding, vec![Operand::LiteralBit32(binding)]);
    }
    let helper_var = if fragment {
        let p = b.ptr(StorageClass::Input, bool_);
        let v = b.variable(p, StorageClass::Input);
        b.decorate(v, Decoration::BuiltIn, vec![Operand::BuiltIn(BuiltIn::HelperInvocation)]);
        Some(v)
    } else {
        None
    };
    // every entry point lists the globals its call tree uses (SPIR-V 1.4)
    let interface: Vec<Word> = [Some(dir_var), Some(cnt_var), helper_var].into_iter().flatten().collect();
    for e in &mut b.m.entry_points {
        let fragment_entry = e.operands.first() == Some(&Operand::ExecutionModel(ExecutionModel::Fragment));
        for v in &interface {
            if Some(*v) == helper_var && !fragment_entry {
                continue;
            }
            if !e.operands.iter().skip(3).any(|o| o == &Operand::IdRef(*v)) {
                e.operands.push(Operand::IdRef(*v));
            }
        }
    }

    let fn_ty = b.ty(
        Op::TypeFunction,
        vec![
            Operand::IdRef(void),
            Operand::IdRef(uint),
            Operand::IdRef(uint),
            Operand::IdRef(uint),
            Operand::IdRef(v3float),
            Operand::IdRef(bool_),
        ],
    );
    let c = |b: &mut Builder, v: u32| b.constant(uint, v);
    let (c0, c1, c5, c9, c13, c15, c16, c31, c_ffff) =
        (c(b, 0), c(b, 1), c(b, 5), c(b, 9), c(b, 13), c(b, 15), c(b, 16), c(b, 31), c(b, 0xffff));
    let c_slot = c(b, COUNTER_HEAP_SLOT);
    let c_last_level = c(b, MAX_LEVELS - 1);
    let c_last_word = c(b, MAX_LEVELS);
    let c_header = c(b, HEADER_WORDS);
    let subgroup = c(b, Scope::Subgroup as u32);
    let device = c(b, Scope::Device as u32);
    let f0 = b.constant(float, 0f32.to_bits());
    let fmax = b.constant(float, 65535f32.to_bits());
    let v3f0 = b.composite(v3float, &[f0, f0, f0]);
    let v3fmax = b.composite(v3float, &[fmax, fmax, fmax]);

    let helper = b.m.fresh_id();
    let (p_slot, p_level, p_slice, p_coord, p_norm) =
        (b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id());
    let (l_entry, l_body, l_atom, l_join, l_end) =
        (b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id(), b.m.fresh_id());

    // entry: the directory element of the slot; past it when zero (or a helper invocation)
    let mut e = Emit { b, out: Vec::new() };
    let s = e.ext(glsl, uint, G_UMIN, &[p_slot, c_slot]);
    let dp = e.ids(Op::AccessChain, dir_ptr, &[dir_var, c_slot]);
    let di = e.ids(Op::Load, dir_img, &[dp]);
    let rec = e.ids(Op::ImageFetch, v4uint, &[di, s]);
    let base = e.extract(uint, rec, 0);
    let mut go = e.ids(Op::INotEqual, bool_, &[base, c0]);
    if let Some(hv) = helper_var {
        let h = e.ids(Op::Load, bool_, &[hv]);
        let live = e.ids(Op::LogicalNot, bool_, &[h]);
        go = e.ids(Op::LogicalAnd, bool_, &[go, live]);
    }
    e.none(Op::SelectionMerge, vec![Operand::IdRef(l_end), Operand::SelectionControl(SelectionControl::NONE)]);
    e.none(Op::BranchConditional, vec![Operand::IdRef(go), Operand::IdRef(l_body), Operand::IdRef(l_end)]);
    let entry = std::mem::take(&mut e.out);

    // body: the counter of (level, slice, tile), and the subgroup's lanes that share it add once
    let ry = e.extract(uint, rec, 1);
    let rz = e.extract(uint, rec, 2);
    let rw = e.extract(uint, rec, 3);
    let w0 = e.ids(Op::BitwiseAnd, uint, &[ry, c_ffff]);
    let h0 = e.ids(Op::ShiftRightLogical, uint, &[ry, c16]);
    let d0 = e.ids(Op::BitwiseAnd, uint, &[rz, c_ffff]);
    let slices = e.ids(Op::ShiftRightLogical, uint, &[rz, c16]);
    let levels = e.ids(Op::BitwiseAnd, uint, &[rw, c31]);
    let lt = |e: &mut Emit, sh: Word| {
        let x = e.ids(Op::ShiftRightLogical, uint, &[rw, sh]);
        e.ids(Op::BitwiseAnd, uint, &[x, c15])
    };
    let (ltw, lth, ltd) = (lt(&mut e, c5), lt(&mut e, c9), lt(&mut e, c13));
    let levels1 = e.ext(glsl, uint, G_UMAX, &[levels, c1]);
    let last_level = e.ids(Op::ISub, uint, &[levels1, c1]);
    let m0 = e.ext(glsl, uint, G_UMIN, &[p_level, last_level]);
    let m = e.ext(glsl, uint, G_UMIN, &[m0, c_last_level]);
    let level_size = |e: &mut Emit, s0: Word| {
        let x = e.ids(Op::ShiftRightLogical, uint, &[s0, m]);
        e.ext(glsl, uint, G_UMAX, &[x, c1])
    };
    let (wm, hm, dm) = (level_size(&mut e, w0), level_size(&mut e, h0), level_size(&mut e, d0));
    let grid = |e: &mut Emit, size: Word, l2: Word| {
        let one = e.ids(Op::ShiftLeftLogical, uint, &[c1, l2]);
        let round = e.ids(Op::ISub, uint, &[one, c1]);
        let x = e.ids(Op::IAdd, uint, &[size, round]);
        e.ids(Op::ShiftRightLogical, uint, &[x, l2])
    };
    let (gx, gy, gz) = (grid(&mut e, wm, ltw), grid(&mut e, hm, lth), grid(&mut e, dm, ltd));
    let dims = e.ids(Op::CompositeConstruct, v3uint, &[wm, hm, dm]);
    let fdims = e.ids(Op::ConvertUToF, v3float, &[dims]);
    let scaled = e.ids(Op::FMul, v3float, &[p_coord, fdims]);
    let norm3 = e.ids(Op::CompositeConstruct, v3bool, &[p_norm, p_norm, p_norm]);
    let texel = e.ids(Op::Select, v3float, &[norm3, scaled, p_coord]);
    let clamped = e.ext(glsl, v3float, G_NCLAMP, &[texel, v3f0, v3fmax]);
    let ut = e.ids(Op::ConvertFToU, v3uint, &[clamped]);
    let tile = |e: &mut Emit, i: u32, size: Word, l2: Word| {
        let x = e.extract(uint, ut, i);
        let last = e.ids(Op::ISub, uint, &[size, c1]);
        let x = e.ext(glsl, uint, G_UMIN, &[x, last]);
        e.ids(Op::ShiftRightLogical, uint, &[x, l2])
    };
    let (tx, ty, tz) = (tile(&mut e, 0, wm, ltw), tile(&mut e, 1, hm, lth), tile(&mut e, 2, dm, ltd));
    let slices1 = e.ext(glsl, uint, G_UMAX, &[slices, c1]);
    let last_slice = e.ids(Op::ISub, uint, &[slices1, c1]);
    let sl = e.ext(glsl, uint, G_UMIN, &[p_slice, last_slice]);
    let cp = e.ids(Op::AccessChain, cnt_ptr, &[cnt_var, c_slot]);
    let ci = e.ids(Op::Load, cnt_img, &[cp]);
    let at_start = e.ids(Op::IAdd, uint, &[base, m]);
    let start4 = e.ids(Op::ImageRead, v4uint, &[ci, at_start]);
    let start = e.extract(uint, start4, 0);
    let at_count = e.ids(Op::IAdd, uint, &[base, c_last_word]);
    let count4 = e.ids(Op::ImageRead, v4uint, &[ci, at_count]);
    let count = e.extract(uint, count4, 0);
    // start + ((slice * gz + tz) * gy + ty) * gx + tx, at most count - 1
    let a = e.ids(Op::IMul, uint, &[sl, gz]);
    let a = e.ids(Op::IAdd, uint, &[a, tz]);
    let a = e.ids(Op::IMul, uint, &[a, gy]);
    let a = e.ids(Op::IAdd, uint, &[a, ty]);
    let a = e.ids(Op::IMul, uint, &[a, gx]);
    let a = e.ids(Op::IAdd, uint, &[a, tx]);
    let local = e.ids(Op::IAdd, uint, &[start, a]);
    let count1 = e.ext(glsl, uint, G_UMAX, &[count, c1]);
    let last = e.ids(Op::ISub, uint, &[count1, c1]);
    let local = e.ext(glsl, uint, G_UMIN, &[local, last]);
    let first_counter = e.ids(Op::IAdd, uint, &[base, c_header]);
    let index = e.ids(Op::IAdd, uint, &[first_counter, local]);
    let first = e.op(Op::GroupNonUniformBroadcastFirst, uint, vec![Operand::IdScope(subgroup), Operand::IdRef(index)]);
    let same = e.ids(Op::IEqual, bool_, &[index, first]);
    let ballot = e.op(Op::GroupNonUniformBallot, v4uint, vec![Operand::IdScope(subgroup), Operand::IdRef(same)]);
    let n = e.op(
        Op::GroupNonUniformBallotBitCount,
        uint,
        vec![Operand::IdScope(subgroup), Operand::GroupOperation(GroupOperation::Reduce), Operand::IdRef(ballot)],
    );
    let lead = e.op(Op::GroupNonUniformElect, bool_, vec![Operand::IdScope(subgroup)]);
    let lead_add = e.ids(Op::Select, uint, &[lead, n, c0]);
    let add = e.ids(Op::Select, uint, &[same, lead_add, c1]);
    let adds = e.ids(Op::INotEqual, bool_, &[add, c0]);
    e.none(Op::SelectionMerge, vec![Operand::IdRef(l_join), Operand::SelectionControl(SelectionControl::NONE)]);
    e.none(Op::BranchConditional, vec![Operand::IdRef(adds), Operand::IdRef(l_atom), Operand::IdRef(l_join)]);
    let body = std::mem::take(&mut e.out);

    let tp = e.ids(Op::ImageTexelPointer, texel_ptr, &[cp, index, c0]);
    e.op(
        Op::AtomicIAdd,
        uint,
        vec![Operand::IdRef(tp), Operand::IdScope(device), Operand::IdMemorySemantics(c0), Operand::IdRef(add)],
    );
    e.none(Op::Branch, vec![Operand::IdRef(l_join)]);
    let atom = std::mem::take(&mut e.out);
    e.none(Op::Branch, vec![Operand::IdRef(l_end)]);
    let join = std::mem::take(&mut e.out);
    e.none(Op::Return, vec![]);
    let end = std::mem::take(&mut e.out);

    let label = |l: Word| Some(Instruction::new(Op::Label, None, Some(l), vec![]));
    let param = |p: Word, t: Word| Instruction::new(Op::FunctionParameter, Some(t), Some(p), vec![]);
    let function = Function {
        def: Some(Instruction::new(
            Op::Function,
            Some(void),
            Some(helper),
            vec![Operand::FunctionControl(FunctionControl::NONE), Operand::IdRef(fn_ty)],
        )),
        end: Some(Instruction::new(Op::FunctionEnd, None, None, vec![])),
        parameters: vec![
            param(p_slot, uint),
            param(p_level, uint),
            param(p_slice, uint),
            param(p_coord, v3float),
            param(p_norm, bool_),
        ],
        blocks: vec![
            Block { label: label(l_entry), instructions: entry },
            Block { label: label(l_body), instructions: body },
            Block { label: label(l_atom), instructions: atom },
            Block { label: label(l_join), instructions: join },
            Block { label: label(l_end), instructions: end },
        ],
    };
    b.m.functions.push(function);
    b.m.debug_names.push(Instruction::new(
        Op::Name,
        None,
        None,
        vec![Operand::IdRef(helper), Operand::LiteralString(HELPER_NAME.into())],
    ));
    Ok(Globals { void, bool_, uint, int, float, v2float, v3float, glsl, helper })
}

/// The code before a counted instruction: level, slice and coordinates of its read, and the call
fn call_site(b: &mut Builder, g: &Globals, s: &Site, fragment: bool) -> Result<Vec<Instruction>, String> {
    let (uint, float) = (g.uint, g.float);
    let f0 = b.constant(float, 0f32.to_bits());
    let fhalf = b.constant(float, 0.5f32.to_bits());
    let fone = b.constant(float, 1f32.to_bits());
    let f_max_level = b.constant(float, (MAX_LEVELS as f32 - 1.0).to_bits());
    let f_max_slice = b.constant(float, 65535f32.to_bits());
    let c0 = b.constant(uint, 0);
    let c6 = b.constant(uint, 6);
    let i0 = b.constant(g.int, 0);
    let (c1, c2, c3, c4, c5) =
        (b.constant(uint, 1), b.constant(uint, 2), b.constant(uint, 3), b.constant(uint, 4), b.constant(uint, 5));
    let texel = matches!(s.kind, Kind::Fetch | Kind::Read);
    let mut e = Emit { b, out: Vec::new() };
    let glsl = g.glsl;

    // the coordinate's components as 32-bit floats
    let scalar_ty = match s.coord_scalar {
        Scalar::F32 => float,
        Scalar::F16 => e.b.ty(Op::TypeFloat, vec![Operand::LiteralBit32(16)]),
        Scalar::I32 => g.int,
        Scalar::U32 => uint,
    };
    let mut comp = Vec::new();
    for i in 0..s.coord_count.min(4) {
        let c = if s.coord_count == 1 { s.coord } else { e.extract(scalar_ty, s.coord, i) };
        comp.push(match s.coord_scalar {
            Scalar::F32 => c,
            Scalar::F16 => e.ids(Op::FConvert, float, &[c]),
            Scalar::I32 => e.ids(Op::ConvertSToF, float, &[c]),
            Scalar::U32 => e.ids(Op::ConvertUToF, float, &[c]),
        });
    }
    // a float layer index rounded and clamped to [0, 65535] as Vulkan picks the layer (texel reads give integers)
    let layer_of = |e: &mut Emit, v: Word| {
        let r = e.ids(Op::FAdd, float, &[v, fhalf]);
        let r = e.ext(glsl, float, G_NCLAMP, &[r, f0, f_max_slice]);
        e.ids(Op::ConvertFToU, uint, &[r])
    };
    let (x, y, z, slice) = match (s.dim, s.arrayed) {
        (Dim::Dim2D, false) => (comp[0], comp[1], f0, c0),
        (Dim::Dim2D, true) if texel => {
            // an integer layer, clamped at 0 (the helper clamps it to the slices)
            let raw = e.extract(scalar_ty, s.coord, 2);
            let layer = match s.coord_scalar {
                Scalar::I32 => {
                    let m = e.ext(glsl, g.int, G_SMAX, &[raw, i0]);
                    e.ids(Op::Bitcast, uint, &[m])
                }
                _ => raw,
            };
            (comp[0], comp[1], f0, layer)
        }
        (Dim::Dim2D, true) => (comp[0], comp[1], f0, layer_of(&mut e, comp[2])),
        (Dim::Dim3D, _) => (comp[0], comp[1], comp[2], c0),
        (Dim::DimCube, arrayed) => {
            // the face of the direction (major axis, Vulkan's table) and its (s, t) in [0, 1]
            let (rx, ry, rz) = (comp[0], comp[1], comp[2]);
            let ax = e.ext(glsl, float, G_FABS, &[rx]);
            let ay = e.ext(glsl, float, G_FABS, &[ry]);
            let az = e.ext(glsl, float, G_FABS, &[rz]);
            let xy = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[ax, ay]);
            let xz = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[ax, az]);
            let xmaj = e.ids(Op::LogicalAnd, g.bool_, &[xy, xz]);
            let ymaj = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[ay, az]);
            let xp = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[rx, f0]);
            let yp = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[ry, f0]);
            let zp = e.ids(Op::FOrdGreaterThanEqual, g.bool_, &[rz, f0]);
            let nx = e.ids(Op::FNegate, float, &[rx]);
            let ny = e.ids(Op::FNegate, float, &[ry]);
            let nz = e.ids(Op::FNegate, float, &[rz]);
            let sel = |e: &mut Emit, ty: Word, c: Word, a: Word, b: Word| e.ids(Op::Select, ty, &[c, a, b]);
            // +x: (-z, -y)  -x: (z, -y)  +y: (x, z)  -y: (x, -z)  +z: (x, -y)  -z: (-x, -y)
            let sc_x = sel(&mut e, float, xp, nz, rz);
            let sc_z = sel(&mut e, float, zp, rx, nx);
            let sc_yz = sel(&mut e, float, ymaj, rx, sc_z);
            let sc = sel(&mut e, float, xmaj, sc_x, sc_yz);
            let tc_y = sel(&mut e, float, yp, rz, nz);
            let tc_yz = sel(&mut e, float, ymaj, tc_y, ny);
            let tc = sel(&mut e, float, xmaj, ny, tc_yz);
            let ma_yz = sel(&mut e, float, ymaj, ay, az);
            let ma = sel(&mut e, float, xmaj, ax, ma_yz);
            let face_x = sel(&mut e, uint, xp, c0, c1);
            let face_y = sel(&mut e, uint, yp, c2, c3);
            let face_z = sel(&mut e, uint, zp, c4, c5);
            let face_yz = sel(&mut e, uint, ymaj, face_y, face_z);
            let face = sel(&mut e, uint, xmaj, face_x, face_yz);
            let st = |e: &mut Emit, c: Word| {
                let q = e.ids(Op::FDiv, float, &[c, ma]);
                let q = e.ids(Op::FAdd, float, &[q, fone]);
                e.ids(Op::FMul, float, &[q, fhalf])
            };
            let (u, v) = (st(&mut e, sc), st(&mut e, tc));
            let slice = if arrayed {
                let layer = layer_of(&mut e, comp[3]);
                let l6 = e.ids(Op::IMul, uint, &[layer, c6]);
                e.ids(Op::IAdd, uint, &[l6, face])
            } else {
                face
            };
            (u, v, f0, slice)
        }
        _ => return Err("sparse counters: an image shape that cannot be sparse".into()),
    };

    // the level
    let round_level = |e: &mut Emit, lod: Word| {
        let r = e.ids(Op::FAdd, float, &[lod, fhalf]);
        let r = e.ext(glsl, float, G_NCLAMP, &[r, f0, f_max_level]);
        e.ids(Op::ConvertFToU, uint, &[r])
    };
    let level = match s.kind {
        Kind::Gather | Kind::Read => c0,
        Kind::Fetch => match s.lod {
            Some((l, Scalar::I32)) => {
                let m = e.ext(glsl, g.int, G_SMAX, &[l, i0]);
                e.ids(Op::Bitcast, uint, &[m])
            }
            Some((l, Scalar::U32)) => l,
            _ => c0,
        },
        Kind::Sample { implicit } => {
            let lod = if let Some((dx, dy, cnt)) = s.grad {
                // log2 of the longer gradient in texels of level 0
                let img = if s.sampled { e.ids(Op::Image, s.image_ty, &[s.image_operand]) } else { s.image_operand };
                let n = if s.dim == Dim::Dim3D { 3 } else { 2 } + u32::from(s.arrayed);
                let size_ty = e.b.vec(g.int, n);
                let size = e.op(Op::ImageQuerySizeLod, size_ty, vec![Operand::IdRef(img), Operand::IdRef(i0)]);
                let w = e.extract(g.int, size, 0);
                let w = e.ids(Op::ConvertSToF, float, &[w]);
                let h = e.extract(g.int, size, 1);
                let h = e.ids(Op::ConvertSToF, float, &[h]);
                let grad_len = |e: &mut Emit, gv: Word| {
                    let gx = if cnt == 1 { gv } else { e.extract(float, gv, 0) };
                    let gx = e.ids(Op::FMul, float, &[gx, w]);
                    let gy = if cnt >= 2 { e.extract(float, gv, 1) } else { f0 };
                    let gy = e.ids(Op::FMul, float, &[gy, h]);
                    let v = e.ids(Op::CompositeConstruct, g.v2float, &[gx, gy]);
                    e.ext(glsl, float, G_LENGTH, &[v])
                };
                let (lx, ly) = (grad_len(&mut e, dx), grad_len(&mut e, dy));
                let rho = e.ext(glsl, float, G_FMAX, &[lx, ly]);
                Some(e.ext(glsl, float, G_LOG2, &[rho]))
            } else if let Some((l, Scalar::F32)) = s.lod {
                Some(l)
            } else if implicit && fragment {
                // the implicit LOD of the fragment (the level Vulkan's query reports), + bias
                let n = if s.dim == Dim::Dim2D { 2 } else { 3 };
                let q = if n == 2 {
                    e.ids(Op::CompositeConstruct, g.v2float, &[comp[0], comp[1]])
                } else {
                    e.ids(Op::CompositeConstruct, g.v3float, &[comp[0], comp[1], comp[2]])
                };
                let ql = e.ids(Op::ImageQueryLod, g.v2float, &[s.image_operand, q]);
                let l = e.extract(float, ql, 0);
                Some(match s.bias {
                    Some(bias) => e.ids(Op::FAdd, float, &[l, bias]),
                    None => l,
                })
            } else {
                None
            };
            match lod {
                Some(l) => {
                    let l = match s.min_lod {
                        Some(min) => e.ext(glsl, float, G_FMAX, &[l, min]),
                        None => l,
                    };
                    round_level(&mut e, l)
                }
                None => c0,
            }
        }
    };
    let coord3 = e.ids(Op::CompositeConstruct, g.v3float, &[x, y, z]);
    let norm = e.b.boolean(g.bool_, !texel);
    let slot = if s.slot_signed { e.ids(Op::Bitcast, uint, &[s.slot]) } else { s.slot };
    e.op(
        Op::FunctionCall,
        g.void,
        vec![
            Operand::IdRef(g.helper),
            Operand::IdRef(slot),
            Operand::IdRef(level),
            Operand::IdRef(slice),
            Operand::IdRef(coord3),
            Operand::IdRef(norm),
        ],
    );
    Ok(e.out)
}
