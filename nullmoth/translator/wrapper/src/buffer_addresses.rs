use rspirv::{binary::Assemble, dr::{Instruction, Operand}, spirv::*};
use std::collections::{HashMap, HashSet};

pub const TABLE_BINDING: u32 = metal2vulkan::reflect::SYNTHETIC_BINDING_BASE;

fn inst(op: Op, ty: Option<u32>, id: Option<u32>, args: Vec<Operand>) -> Instruction {
    Instruction::new(op, ty, id, args)
}
fn id(o: &Operand) -> Option<u32> { if let Operand::IdRef(v) = o { Some(*v) } else { None } }
fn lit(o: &Operand) -> Option<u32> { if let Operand::LiteralBit32(v) = o { Some(*v) } else { None } }
fn scalar_align(ty: u32, defs: &HashMap<u32, Instruction>, depth: usize) -> Result<u32, String> {
    if depth > 32 { return Err("recursive buffer type".into()); }
    let t = defs.get(&ty).ok_or("missing buffer type")?;
    match t.class.opcode {
        Op::TypeInt | Op::TypeFloat => Ok(lit(&t.operands[0]).ok_or("scalar width")? / 8),
        Op::TypePointer => Ok(8),
        Op::TypeVector | Op::TypeMatrix | Op::TypeArray | Op::TypeRuntimeArray => scalar_align(id(&t.operands[0]).ok_or("element type")?, defs, depth+1),
        Op::TypeStruct => t.operands.iter().map(|o| scalar_align(id(o).ok_or("member type")?, defs, depth+1)).try_fold(1, |a,b| b.map(|b| a.max(b))),
        _ => Err(format!("unsupported buffer pointee {:?}", t.class.opcode)),
    }
}

fn holds_runtime_array(ty: u32, defs: &HashMap<u32, Instruction>, depth: usize) -> bool {
    let Some(t) = defs.get(&ty).filter(|_| depth < 32) else { return true };
    match t.class.opcode {
        Op::TypeRuntimeArray => true,
        Op::TypeArray | Op::TypeStruct => t.operands.iter().filter_map(id).any(|e| holds_runtime_array(e, defs, depth + 1)),
        _ => false,
    }
}
/// Whether a value depends on data read from memory or an image, as opposed to constants, built-ins, vertex inputs
/// and arithmetic on them.
///
/// `push_not_data`: a load from a push constant is a driver value too (the translator's dispatch region: thread and
/// threadgroup bases, grid sizes; Metal has no push constants of its own, so no app data arrives there). Off by default:
/// `thread_position_in_grid` is GlobalInvocationId plus the pushed thread base, so with it off every `buffer[tid]` of a
/// kernel keeps its descriptor. On for kernels of GPU-encodable ICBs (NVMTL_ICB_DGC=1), whose buffers a shader sets by
/// address: a kept descriptor would be the CPU's buffer (docs/design/DESIGN-icb-dgc.md section 10).
fn data_dependent(v: u32, local: &HashMap<u32, &Instruction>, defs: &HashMap<u32, Instruction>, value_types: &HashMap<u32, u32>, seen: &mut HashSet<u32>, budget: &mut u32, push_not_data: bool) -> bool {
    if !seen.insert(v) { return false; }
    if *budget == 0 { return true; }
    *budget -= 1;
    let Some(d) = local.get(&v) else { return false };
    match d.class.opcode {
        Op::Load => match d.operands.first().and_then(id).and_then(|p| value_types.get(&p)).and_then(|t| defs.get(t))
            .and_then(|t| t.operands.first()) {
            Some(Operand::StorageClass(StorageClass::Input)) => false,
            Some(Operand::StorageClass(StorageClass::PushConstant)) => !push_not_data,
            _ => true,
        },
        Op::ImageRead | Op::ImageFetch | Op::ImageSampleImplicitLod | Op::ImageSampleExplicitLod | Op::ImageSampleDrefImplicitLod
        | Op::ImageSampleDrefExplicitLod | Op::ImageGather | Op::ImageDrefGather | Op::AtomicLoad | Op::FunctionCall => true,
        // sparse_color residency reads (images/residency.rs): image reads as well
        Op::ImageSparseRead | Op::ImageSparseFetch | Op::ImageSparseSampleImplicitLod | Op::ImageSparseSampleExplicitLod
        | Op::ImageSparseSampleDrefImplicitLod | Op::ImageSparseSampleDrefExplicitLod | Op::ImageSparseGather
        | Op::ImageSparseDrefGather => true,
        Op::Phi => d.operands.iter().step_by(2).filter_map(id).any(|o| data_dependent(o, local, defs, value_types, seen, budget, push_not_data)),
        _ => d.operands.iter().filter_map(id).any(|o| data_dependent(o, local, defs, value_types, seen, budget, push_not_data)),
    }
}
/// The buffers that stay descriptors: roots whose block holds a runtime array that an access chain indexes with a
/// data-dependent value, and whose pointers reach nothing but access chains, loads, stores and OpArrayLength (a call
/// or a pointer used as a value would need the pointer in both forms, so such a root is lowered as before). Returns
/// the kept roots and every pointer derived from them.
fn kept_roots(m: &rspirv::dr::Module, defs: &HashMap<u32, Instruction>, sb_types: &HashMap<u32, u32>, roots: &[(u32, u32, u32)], value_types: &HashMap<u32, u32>, push_not_data: bool) -> (HashSet<u32>, HashSet<u32>) {
    let local: HashMap<u32, &Instruction> = m.functions.iter().flat_map(|f| f.blocks.iter()).flat_map(|b| b.instructions.iter()).filter_map(|i| Some((i.result_id?, i))).collect();
    let mut owner: HashMap<u32, u32> = roots.iter().filter(|r| sb_types.get(&r.1).is_some_and(|&p| holds_runtime_array(p, defs, 0))).map(|r| (r.0, r.0)).collect();
    let (mut needs, mut bad) = (HashSet::new(), HashSet::new());
    for i in m.functions.iter().flat_map(|f| f.blocks.iter()).flat_map(|b| b.instructions.iter()) {
        let Some(&root) = i.operands.first().and_then(id).and_then(|v| owner.get(&v)) else {
            bad.extend(i.operands.iter().filter_map(id).filter_map(|v| owner.get(&v).copied())); continue };
        if i.operands.iter().skip(1).filter_map(id).any(|v| owner.contains_key(&v)) { bad.insert(root); }
        match i.class.opcode {
            Op::AccessChain | Op::InBoundsAccessChain | Op::PtrAccessChain | Op::CopyObject => {
                if let Some(r) = i.result_id { owner.insert(r, root); }
                let (mut seen, mut budget) = (HashSet::new(), 96u32);
                if i.operands.iter().skip(1).filter_map(id).any(|x| data_dependent(x, &local, defs, value_types, &mut seen, &mut budget, push_not_data)) { needs.insert(root); }
            }
            Op::Load | Op::Store | Op::ArrayLength => {}
            _ => { bad.insert(root); }
        }
    }
    let kept: HashSet<u32> = needs.difference(&bad).copied().collect();
    let derived = owner.into_iter().filter(|(_, o)| kept.contains(o)).map(|(v, _)| v).collect();
    (kept, derived)
}
/// Gives the variables in `seeds`, which stay StorageBuffer, and every pointer derived from them their own pointer
/// types, so the types they shared with lowered buffers can become PhysicalStorageBuffer. Each new type is placed right
/// after the one it copies.
fn keep_on_storage_buffer(m: &mut rspirv::dr::Module, seeds: &[u32], sb_types: &HashMap<u32, u32>, value_types: &mut HashMap<u32, u32>, fresh: &mut dyn FnMut() -> u32) {
    let mut dup: HashMap<u32, u32> = HashMap::new(); let mut derived: HashSet<u32> = seeds.iter().copied().collect();
    for v in &mut m.types_global_values {
        if v.class.opcode == Op::Variable && v.result_id.is_some_and(|n| derived.contains(&n)) {
            if let Some(t) = v.result_type.filter(|t| sb_types.contains_key(t)) { let n = *dup.entry(t).or_insert_with(&mut *fresh); v.result_type = Some(n); value_types.insert(v.result_id.unwrap(), n); }
        }
    }
    for i in m.functions.iter_mut().flat_map(|f| f.blocks.iter_mut()).flat_map(|b| b.instructions.iter_mut()) {
        if !matches!(i.class.opcode, Op::AccessChain | Op::InBoundsAccessChain | Op::PtrAccessChain | Op::CopyObject) { continue; }
        if !i.operands.first().and_then(id).is_some_and(|v| derived.contains(&v)) { continue; }
        let (Some(r), Some(t)) = (i.result_id, i.result_type) else { continue };
        derived.insert(r);
        if sb_types.contains_key(&t) { let n = *dup.entry(t).or_insert_with(&mut *fresh); i.result_type = Some(n); value_types.insert(r, n); }
    }
    let mut places: Vec<(usize, Instruction)> = dup.iter().map(|(&t, &n)| (m.types_global_values.iter().position(|i| i.result_id == Some(t)).unwrap() + 1,
        inst(Op::TypePointer, None, Some(n), vec![Operand::StorageClass(StorageClass::StorageBuffer), Operand::IdRef(sb_types[&t])]))).collect();
    places.sort_by(|a, b| b.0.cmp(&a.0));
    for (pos, t) in places { m.types_global_values.insert(pos, t); }
}

fn keep_stated_align(scalar: u32, stated: Option<&Operand>) -> u32 {
    if std::env::var_os("NVMTL_NO_AIR_ALIGN").is_some() { return scalar; }
    match stated { Some(Operand::LiteralBit32(a)) if a.is_power_of_two() && *a <= 16 => scalar.max(*a), _ => scalar }
}

pub fn lower(bytes: &[u8]) -> Result<Vec<u8>, String> { lower_with(bytes, 0) }
/// `residue`: the device's storage-buffer offset alignment (a power of two; 0 = none). A buffer kept as a descriptor
/// (an index that depends on loaded data, see kept_roots) whose block is one runtime array is then also indexed from the
/// descriptor's offset rounded down to `residue`: the plugin binds it there whenever the binding's own offset is not a
/// multiple of `residue` (an OpenCL copy at a byte offset), and the shader adds the remainder, read from the binding's
/// address-table entry (address + offset), divided by the array stride. A remainder that is not a multiple of the
/// stride is an offset Metal leaves undefined; the access then reads from the rounded index, still inside the buffer.
pub fn lower_with(bytes: &[u8], residue: u32) -> Result<Vec<u8>, String> { lower_with_opts(bytes, residue, false) }
/// `lower_with`, and with `push_not_data` an index computed from push constants (the dispatch region) does not keep a
/// buffer's descriptor (see data_dependent).
pub fn lower_with_opts(bytes: &[u8], residue: u32, push_not_data: bool) -> Result<Vec<u8>, String> {
    let residue = if residue.is_power_of_two() && residue > 1 && residue <= 256 { residue } else { 0 };
    match crate::linearize::apply(bytes) {
        Ok((lin, _why)) => lower_addresses(&lin, residue, push_not_data),
        Err(_) => lower_addresses(bytes, residue, push_not_data),
    }
}
/// A kept root whose block is exactly one runtime array: (variable, binding, array stride in bytes).
fn residue_roots(m: &rspirv::dr::Module, defs: &HashMap<u32, Instruction>, kept: &HashSet<u32>, roots: &[(u32,u32,u32)]) -> Vec<(u32,u32,u32)> {
    let stride_of = |t: u32| m.annotations.iter().find_map(|d| match d.operands.as_slice() {
        [Operand::IdRef(n), Operand::Decoration(Decoration::ArrayStride), Operand::LiteralBit32(v)] if *n == t => Some(*v), _ => None });
    roots.iter().filter(|r| kept.contains(&r.0) && r.2 < 32).filter_map(|&(v, ty, b)| {
        let ptr = defs.get(&ty).filter(|p| p.class.opcode == Op::TypePointer)?;
        let block = defs.get(&id(ptr.operands.get(1)?)?).filter(|s| s.class.opcode == Op::TypeStruct && s.operands.len() == 1)?;
        let arr = id(&block.operands[0])?;
        defs.get(&arr).filter(|a| a.class.opcode == Op::TypeRuntimeArray)?;
        let st = stride_of(arr).filter(|s| *s > 0)?;
        Some((v, b, st))
    }).collect()
}
/// A table the translator declared at TABLE_BINDING that buffer addresses can be read from: a StorageBuffer variable
/// in set 0 whose block holds one array of 8-byte entries, ulong or uvec2. Returns (block, entry type, entry is uvec2).
fn existing_table(defs: &HashMap<u32, Instruction>, m: &rspirv::dr::Module, tv: u32, sets: &HashMap<u32, u32>) -> Option<(u32, u32, bool)> {
    let var = defs.get(&tv).filter(|i| i.class.opcode == Op::Variable && i.operands.first() == Some(&Operand::StorageClass(StorageClass::StorageBuffer)))?;
    if sets.get(&tv) != Some(&0) { return None; }
    let ptr = defs.get(&var.result_type?).filter(|p| p.class.opcode == Op::TypePointer)?;
    let block = id(ptr.operands.get(1)?)?;
    let s = defs.get(&block).filter(|s| s.class.opcode == Op::TypeStruct && s.operands.len() == 1)?;
    // An 8-byte array alone is not enough: address entry b must really be at byte 8*b in this block.
    if !m.annotations.iter().any(|a| a.class.opcode == Op::Decorate && a.operands == [Operand::IdRef(block), Operand::Decoration(Decoration::Block)])
        || !m.annotations.iter().any(|a| a.class.opcode == Op::MemberDecorate && a.operands == [Operand::IdRef(block), Operand::LiteralBit32(0), Operand::Decoration(Decoration::Offset), Operand::LiteralBit32(0)]) {
        return None;
    }
    let arr = id(&s.operands[0])?;
    let a = defs.get(&arr).filter(|a| matches!(a.class.opcode, Op::TypeRuntimeArray | Op::TypeArray))?;
    let stride = m.annotations.iter().find_map(|d| match d.operands.as_slice() {
        [Operand::IdRef(n), Operand::Decoration(Decoration::ArrayStride), Operand::LiteralBit32(v)] if *n == arr => Some(*v), _ => None })?;
    if stride != 8 { return None; }
    let elem = id(a.operands.first()?)?;
    let e = defs.get(&elem)?;
    let is_u32 = |t: u32| defs.get(&t).is_some_and(|i| i.class.opcode == Op::TypeInt && i.operands == vec![Operand::LiteralBit32(32), Operand::LiteralBit32(0)]);
    match e.class.opcode {
        Op::TypeInt if e.operands == vec![Operand::LiteralBit32(64), Operand::LiteralBit32(0)] => Some((block, elem, false)),
        Op::TypeVector if e.operands.len() == 2 && id(&e.operands[0]).is_some_and(is_u32) && e.operands[1] == Operand::LiteralBit32(2) => Some((block, elem, true)),
        _ => None,
    }
}

fn lower_addresses(bytes: &[u8], residue: u32, push_not_data: bool) -> Result<Vec<u8>, String> {
    let mut m = rspirv::dr::load_bytes(bytes).map_err(|e| e.to_string())?;
    let defs: HashMap<_,_> = m.types_global_values.iter().filter_map(|i| i.result_id.map(|n|(n,i.clone()))).collect();
    let sb_types: HashMap<u32,u32> = defs.iter().filter_map(|(&n,i)| {
        (i.class.opcode == Op::TypePointer && i.operands.first() == Some(&Operand::StorageClass(StorageClass::StorageBuffer)))
            .then(|| (n, id(&i.operands[1]).unwrap()))
    }).collect();
    let mut bindings = HashMap::new(); let mut sets = HashMap::new();
    for a in &m.annotations {
        if a.class.opcode == Op::Decorate && a.operands.len() >= 3 {
            if let (Some(n), Some(v)) = (id(&a.operands[0]), lit(&a.operands[2])) {
                match a.operands[1] { Operand::Decoration(Decoration::Binding) => { bindings.insert(n,v); }, Operand::Decoration(Decoration::DescriptorSet) => { sets.insert(n,v); }, _ => {} }
            }
        }
    }
    let roots: Vec<(u32,u32,u32)> = m.types_global_values.iter().filter_map(|i| {
        (i.class.opcode == Op::Variable && i.operands.first() == Some(&Operand::StorageClass(StorageClass::StorageBuffer)))
            .then(|| (i.result_id.unwrap(),i.result_type.unwrap(),*bindings.get(&i.result_id.unwrap()).unwrap_or(&u32::MAX)))
    }).collect();
    // The translator may have made the table itself (texture sizes, argument buffers). Its buffers are still lowered,
    // through that table, when it is a storage buffer of 8-byte entries; any other table leaves the shader as it is.
    let table_var = bindings.iter().find(|(_, b)| **b == TABLE_BINDING).map(|(n, _)| *n);
    let existing = match table_var {
        None => None,
        Some(tv) => match existing_table(&defs, &m, tv, &sets) { Some(t) => Some(t), None => return Ok(bytes.to_vec()) },
    };
    let roots: Vec<(u32,u32,u32)> = roots.into_iter().filter(|r| Some(r.0) != table_var).collect();
    // A buffer whose block holds a runtime array is read wherever its data points (a light list, a froxel grid) and
    // past its end whenever the data says so. Apple's GPUs return garbage for such a read; through a physical address
    // it is an MMU fault that takes the device away. These buffers keep their descriptor, which the device
    // bounds-checks with robustBufferAccess; fixed-size blocks still move to addresses.
    let mut value_types = HashMap::new();
    for i in m.all_inst_iter() { if let (Some(n),Some(t)) = (i.result_id,i.result_type) { value_types.insert(n,t); } }
    let (kept, kept_derived) = kept_roots(&m, &defs, &sb_types, &roots, &value_types, push_not_data);
    let resroots = if residue != 0 { residue_roots(&m, &defs, &kept, &roots) } else { Vec::new() };
    if resroots.iter().any(|r| sets.get(&r.0) != Some(&0)) { return Err("a kept buffer outside descriptor set 0".into()); }
    let roots: Vec<(u32,u32,u32)> = roots.into_iter().filter(|r| !kept.contains(&r.0)).collect();
    if roots.is_empty() && resroots.is_empty() { return Ok(bytes.to_vec()); }
    for &(n,_,b) in &roots {
        if b >= 32 || sets.get(&n) != Some(&0) { return Err(format!("unsupported source buffer descriptor {n}, binding {b}")); }
    }
    let root_ids: HashSet<_> = roots.iter().map(|r|r.0).collect();
    for i in m.all_inst_iter() {
        if matches!(i.class.opcode, Op::ArrayLength | Op::PtrEqual | Op::PtrNotEqual | Op::PtrDiff | Op::CopyMemory | Op::CopyMemorySized) && i.operands.iter().filter_map(id).any(|n| !kept_derived.contains(&n) && value_types.get(&n).is_some_and(|t|sb_types.contains_key(t))) {
            return Err(format!("buffer address lowering does not yet support {:?}",i.class.opcode));
        }
        if i.class.opcode == Op::ConstantNull && i.result_type.is_some_and(|t|sb_types.contains_key(&t)) { return Err("logical null buffer pointer requires explicit lowering".into()); }
    }
    let mut next = m.header.as_ref().ok_or("no SPIR-V header")?.bound;
    let mut fresh = || { let n=next; next+=1; n };
    let uint = defs.iter().find_map(|(&n,i)| (i.class.opcode == Op::TypeInt && i.operands == vec![Operand::LiteralBit32(32),Operand::LiteralBit32(0)]).then_some(n)).unwrap_or_else(|| { let n=fresh(); m.types_global_values.push(inst(Op::TypeInt,None,Some(n),vec![Operand::LiteralBit32(32),Operand::LiteralBit32(0)])); n });
    let ulong = defs.iter().find_map(|(&n,i)| (i.class.opcode == Op::TypeInt && i.operands == vec![Operand::LiteralBit32(64),Operand::LiteralBit32(0)]).then_some(n)).unwrap_or_else(|| { let n=fresh(); m.types_global_values.push(inst(Op::TypeInt,None,Some(n),vec![Operand::LiteralBit32(64),Operand::LiteralBit32(0)])); n });
    for t in &mut m.types_global_values {
        if t.result_id.is_some_and(|n|sb_types.contains_key(&n)) { t.operands[0]=Operand::StorageClass(StorageClass::PhysicalStorageBuffer); }
    }
    let seeds: Vec<u32> = kept.iter().copied().chain(table_var).collect();
    keep_on_storage_buffer(&mut m, &seeds, &sb_types, &mut value_types, &mut fresh);
    let wide_hints = crate::wide_loads::hints(&m, &defs, &sb_types, &value_types);
    m.types_global_values.retain(|i|!i.result_id.is_some_and(|n|root_ids.contains(&n)));
    m.annotations.retain(|i| !i.operands.first().and_then(id).is_some_and(|n|root_ids.contains(&n)));
    m.debug_names.retain(|i| !i.operands.first().and_then(id).is_some_and(|n|root_ids.contains(&n)));
    let (table, ptr_ulong, elem_uvec2) = if let (Some(tv), Some((_, elem, uvec2))) = (table_var, existing) {
        // the table keeps its StorageBuffer types (keep_on_storage_buffer above); its entries are read through this one
        let ptr_elem = fresh();
        m.types_global_values.push(inst(Op::TypePointer,None,Some(ptr_elem),vec![Operand::StorageClass(StorageClass::StorageBuffer),Operand::IdRef(elem)]));
        (tv, ptr_elem, uvec2)
    } else {
    let arr=fresh(); let block=fresh(); let ptr_block=fresh(); let ptr_ulong=fresh(); let table=fresh();
    m.types_global_values.extend([
        inst(Op::TypeRuntimeArray,None,Some(arr),vec![Operand::IdRef(ulong)]),
        inst(Op::TypeStruct,None,Some(block),vec![Operand::IdRef(arr)]),
        inst(Op::TypePointer,None,Some(ptr_block),vec![Operand::StorageClass(StorageClass::StorageBuffer),Operand::IdRef(block)]),
        inst(Op::TypePointer,None,Some(ptr_ulong),vec![Operand::StorageClass(StorageClass::StorageBuffer),Operand::IdRef(ulong)]),
        inst(Op::Variable,Some(ptr_block),Some(table),vec![Operand::StorageClass(StorageClass::StorageBuffer)]),
    ]);
    m.annotations.extend([
        inst(Op::Decorate,None,None,vec![Operand::IdRef(arr),Operand::Decoration(Decoration::ArrayStride),Operand::LiteralBit32(8)]),
        inst(Op::Decorate,None,None,vec![Operand::IdRef(block),Operand::Decoration(Decoration::Block)]),
        inst(Op::MemberDecorate,None,None,vec![Operand::IdRef(block),Operand::LiteralBit32(0),Operand::Decoration(Decoration::Offset),Operand::LiteralBit32(0)]),
        inst(Op::Decorate,None,None,vec![Operand::IdRef(table),Operand::Decoration(Decoration::DescriptorSet),Operand::LiteralBit32(0)]),
        inst(Op::Decorate,None,None,vec![Operand::IdRef(table),Operand::Decoration(Decoration::Binding),Operand::LiteralBit32(TABLE_BINDING)]),
        inst(Op::Decorate,None,None,vec![Operand::IdRef(table),Operand::Decoration(Decoration::NonWritable)]),
    ]);
    (table, ptr_ulong, false)
    };
    let mut constants=HashMap::new();
    for b in std::iter::once(0).chain(roots.iter().map(|r|r.2)).chain(resroots.iter().map(|r|r.1)) {
        constants.entry(b).or_insert_with(|| { let n=fresh(); m.types_global_values.push(inst(Op::Constant,Some(uint),Some(n),vec![Operand::LiteralBit32(b)])); n });
    }
    for e in &mut m.entry_points {
        e.operands.retain(|o|!id(o).is_some_and(|n|root_ids.contains(&n)));
        if m.header.as_ref().unwrap().version >= 0x00010400 && !e.operands.contains(&Operand::IdRef(table)) { e.operands.push(Operand::IdRef(table)); }
    }
    for cap in [Capability::Int64,Capability::PhysicalStorageBufferAddresses] {
        if !m.capabilities.iter().any(|i|i.operands==vec![Operand::Capability(cap)]) { m.capabilities.push(inst(Op::Capability,None,None,vec![Operand::Capability(cap)])); }
    }
    let ext=Operand::LiteralString("SPV_KHR_physical_storage_buffer".into());
    if !m.extensions.iter().any(|i|i.operands==vec![ext.clone()]) { m.extensions.push(inst(Op::Extension,None,None,vec![ext])); }
    m.memory_model.as_mut().ok_or("missing memory model")?.operands[0]=Operand::AddressingModel(AddressingModel::PhysicalStorageBuffer64);
    for f in &mut m.functions {
        if f.blocks.is_empty() { continue; }
        let mut replace=HashMap::new(); let mut prelude=Vec::new();
        for &(root,ty,b) in &roots {
            let p=fresh(); let addr=fresh(); let physical=fresh();
            if elem_uvec2 {   // the translator's table holds uvec2 entries: the same 8 bytes, read as one 64-bit address
                let (pair, uvec2) = (fresh(), m.types_global_values.iter().find_map(|i| (i.result_id.is_some() && i.class.opcode == Op::TypeVector
                    && i.operands == vec![Operand::IdRef(uint), Operand::LiteralBit32(2)]).then(|| i.result_id.unwrap())).ok_or("table uvec2 type missing")?);
                prelude.extend([
                    inst(Op::AccessChain,Some(ptr_ulong),Some(p),vec![Operand::IdRef(table),Operand::IdRef(constants[&0]),Operand::IdRef(constants[&b])]),
                    inst(Op::Load,Some(uvec2),Some(pair),vec![Operand::IdRef(p)]),
                    inst(Op::Bitcast,Some(ulong),Some(addr),vec![Operand::IdRef(pair)]),
                    inst(Op::ConvertUToPtr,Some(ty),Some(physical),vec![Operand::IdRef(addr)]),
                ]);
            } else {
                prelude.extend([
                    inst(Op::AccessChain,Some(ptr_ulong),Some(p),vec![Operand::IdRef(table),Operand::IdRef(constants[&0]),Operand::IdRef(constants[&b])]),
                    inst(Op::Load,Some(ulong),Some(addr),vec![Operand::IdRef(p)]),
                    inst(Op::ConvertUToPtr,Some(ty),Some(physical),vec![Operand::IdRef(addr)]),
                ]);
            }
            replace.insert(root,physical);
        }
        for param in &f.parameters {
            if param.result_type.is_some_and(|t|sb_types.contains_key(&t)) {
                let n=param.result_id.unwrap();
                m.annotations.retain(|i| !(i.class.opcode==Op::Decorate && i.operands.first()==Some(&Operand::IdRef(n)) && matches!(i.operands.get(1),Some(Operand::Decoration(Decoration::Restrict|Decoration::Aliased)))));
                m.annotations.push(inst(Op::Decorate,None,None,vec![Operand::IdRef(n),Operand::Decoration(Decoration::Aliased)]));
            }
        }
        for b in &mut f.blocks { for i in &mut b.instructions {
            let mem = match i.class.opcode { Op::Load => Some(1),Op::Store => Some(2), _=>None };
            if let Some(at)=mem {
                if let Some(ty)=i.operands.first().and_then(id).and_then(|n|value_types.get(&n)).and_then(|t|sb_types.get(t)) {
                    let align=scalar_align(*ty,&defs,0)?;
                    if let Some(Operand::MemoryAccess(flags))=i.operands.get(at).cloned() {
                        if flags.contains(MemoryAccess::ALIGNED) { i.operands[at+1]=Operand::LiteralBit32(keep_stated_align(align,i.operands.get(at+1))); }
                        else { i.operands[at]=Operand::MemoryAccess(flags|MemoryAccess::ALIGNED); i.operands.insert(at+1,Operand::LiteralBit32(align)); }
                    } else { i.operands.extend([Operand::MemoryAccess(MemoryAccess::ALIGNED),Operand::LiteralBit32(align)]); }
                }
            }
            for o in &mut i.operands { if let Operand::IdRef(n)=o { if let Some(r)=replace.get(n) { *n=*r; } } }
        }}
        // descriptor-kept buffers indexed from the rounded-down descriptor offset (lower_with)
        let mut rem: HashMap<u32, u32> = HashMap::new();
        for &(var, b, stride) in &resroots {
            let used = f.blocks.iter().flat_map(|bl| bl.instructions.iter()).any(|i| i.operands.first() == Some(&Operand::IdRef(var)));
            if !used { continue; }
            let (p, lo, masked, q) = (fresh(), fresh(), fresh(), fresh());
            prelude.push(inst(Op::AccessChain,Some(ptr_ulong),Some(p),vec![Operand::IdRef(table),Operand::IdRef(constants[&0]),Operand::IdRef(constants[&b])]));
            if elem_uvec2 {
                let (pair, uvec2) = (fresh(), m.types_global_values.iter().find_map(|i| (i.result_id.is_some() && i.class.opcode == Op::TypeVector
                    && i.operands == vec![Operand::IdRef(uint), Operand::LiteralBit32(2)]).then(|| i.result_id.unwrap())).ok_or("table uvec2 type missing")?);
                prelude.push(inst(Op::Load,Some(uvec2),Some(pair),vec![Operand::IdRef(p)]));
                prelude.push(inst(Op::CompositeExtract,Some(uint),Some(lo),vec![Operand::IdRef(pair),Operand::LiteralBit32(0)]));
            } else {
                let e = fresh();
                prelude.push(inst(Op::Load,Some(ulong),Some(e),vec![Operand::IdRef(p)]));
                prelude.push(inst(Op::UConvert,Some(uint),Some(lo),vec![Operand::IdRef(e)]));
            }
            let cmask = fresh(); let cstride = fresh();
            m.types_global_values.push(inst(Op::Constant,Some(uint),Some(cmask),vec![Operand::LiteralBit32(residue - 1)]));
            m.types_global_values.push(inst(Op::Constant,Some(uint),Some(cstride),vec![Operand::LiteralBit32(stride)]));
            prelude.push(inst(Op::BitwiseAnd,Some(uint),Some(masked),vec![Operand::IdRef(lo),Operand::IdRef(cmask)]));
            prelude.push(inst(Op::UDiv,Some(uint),Some(q),vec![Operand::IdRef(masked),Operand::IdRef(cstride)]));
            rem.insert(var, q);
        }
        if !rem.is_empty() {
            // the runtime array of a kept root: chains [root, member, index, ...] and [root, member] -> [array, index, ...]
            let mut arrays: HashMap<u32, u32> = HashMap::new();   // pointer to the whole array -> its root's remainder
            let int_info = |t: u32| m.types_global_values.iter().find(|i| i.result_id == Some(t) && i.class.opcode == Op::TypeInt)
                .and_then(|i| Some((lit(&i.operands[0])?, lit(&i.operands[1])?)));
            let mut fixes: Vec<(usize, usize, usize, u32, u32)> = Vec::new();   // (block, instruction, operand, index, remainder)
            for (bi, bl) in f.blocks.iter().enumerate() { for (ii, i) in bl.instructions.iter().enumerate() {
                match i.class.opcode {
                    Op::AccessChain | Op::InBoundsAccessChain => {
                        let Some(base) = i.operands.first().and_then(id) else { continue };
                        if let Some(&q) = rem.get(&base) {
                            if i.operands.len() >= 3 { if let Some(ix) = id(&i.operands[2]) { fixes.push((bi, ii, 2, ix, q)); } }
                            else if i.operands.len() == 2 { if let Some(r) = i.result_id { arrays.insert(r, q); } }
                        } else if let Some(&q) = arrays.get(&base) {
                            if i.operands.len() >= 2 { if let Some(ix) = id(&i.operands[1]) { fixes.push((bi, ii, 1, ix, q)); } }
                        }
                    }
                    Op::ArrayLength => {
                        // the length is counted from the rounded-down offset: the remainder elements are not the buffer's
                        if let (Some(base), Some(r)) = (i.operands.first().and_then(id), i.result_id) {
                            if let Some(&q) = rem.get(&base) { fixes.push((bi, ii, usize::MAX, r, q)); }
                        }
                    }
                    _ => {}
                }
            }}
            // apply from the last instruction backwards so the recorded positions stay valid
            fixes.sort_by(|a, b| (b.0, b.1).cmp(&(a.0, a.1)));
            for (bi, ii, op, val, q) in fixes {
                let ty = value_types.get(&val).copied().unwrap_or(uint);
                let (w, signed) = int_info(ty).unwrap_or((32, 0));
                let mut add: Vec<Instruction> = Vec::new();
                let qv = if ty == uint { q } else if w == 32 { let n = fresh(); add.push(inst(Op::Bitcast,Some(ty),Some(n),vec![Operand::IdRef(q)])); n }
                    else { let n = fresh(); add.push(inst(if signed == 1 { Op::SConvert } else { Op::UConvert },Some(ty),Some(n),vec![Operand::IdRef(q)])); n };
                if op == usize::MAX {
                    // OpArrayLength result: len - q, placed after it, and every later use of the old result renamed
                    let n = fresh(); add.push(inst(Op::ISub,Some(ty),Some(n),vec![Operand::IdRef(val),Operand::IdRef(qv)]));
                    for b2 in f.blocks.iter_mut() { for i2 in b2.instructions.iter_mut() {
                        if i2.result_id == Some(val) { continue; }
                        for o in i2.operands.iter_mut() { if *o == Operand::IdRef(val) { *o = Operand::IdRef(n); } }
                    }}
                    // the ISub itself must read the original length
                    if let Some(last) = add.last_mut() { last.operands[0] = Operand::IdRef(val); }
                    let at = ii + 1; f.blocks[bi].instructions.splice(at..at, add);
                } else {
                    let n = fresh(); add.push(inst(Op::IAdd,Some(ty),Some(n),vec![Operand::IdRef(val),Operand::IdRef(qv)]));
                    f.blocks[bi].instructions[ii].operands[op] = Operand::IdRef(n);
                    f.blocks[bi].instructions.splice(ii..ii, add);
                }
            }
        }
        let at=f.blocks[0].instructions.iter().position(|i|i.class.opcode!=Op::Variable).unwrap_or(f.blocks[0].instructions.len());
        f.blocks[0].instructions.splice(at..at,prelude);
    }
    if std::env::var_os("NVMTL_NO_WIDE_WORD_LOAD").is_none() { crate::wide_loads::fuse(&mut m, &mut next, uint, ulong, &wide_hints); }
    m.header.as_mut().unwrap().bound=next;
    let result:Vec<u8>=m.assemble().iter().flat_map(|w|w.to_le_bytes()).collect();
    metal2vulkan::canonicalize_spirv_bytes(&result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use spirv_tools::{assembler::Assembler, val::Validator};
    const FIXTURE: &str = r#"
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buf
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %arr ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%arr = OpTypeRuntimeArray %uint
%block = OpTypeStruct %arr
%pb = OpTypePointer StorageBuffer %block
%pu = OpTypePointer StorageBuffer %uint
%buf = OpVariable %pb StorageBuffer
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%label = OpLabel
%p = OpAccessChain %pu %buf %zero %one
%x = OpLoad %uint %p
%y = OpIAdd %uint %x %one
OpStore %p %y
OpReturn
OpFunctionEnd
"#;
    fn source(text: &str) -> Vec<u8> {
        spirv_tools::assembler::create(Some(spirv_tools::TargetEnv::Vulkan_1_2)).assemble(text,Default::default()).unwrap().as_bytes().to_vec()
    }
    fn validate(b: &[u8]) {
        let w:Vec<u32>=b.chunks_exact(4).map(|w|u32::from_le_bytes(w.try_into().unwrap())).collect();
        spirv_tools::val::create(Some(spirv_tools::TargetEnv::Vulkan_1_2)).validate(&w,None).unwrap();
    }
    #[test] fn loads_and_stores_use_exact_address_table() {
        let input=source(FIXTURE); validate(&input); let output=lower(&input).unwrap(); validate(&output);
        let m=rspirv::dr::load_bytes(&output).unwrap();
        assert!(m.all_inst_iter().any(|i|i.class.opcode==Op::ConvertUToPtr));
        assert!(m.annotations.iter().any(|i|i.operands.ends_with(&[Operand::Decoration(Decoration::Binding),Operand::LiteralBit32(640)])));
    }
    #[test] fn keeps_memory_access_flags() {
        let input=source(&FIXTURE.replace("%x = OpLoad %uint %p","%x = OpLoad %uint %p Volatile|Aligned 16").replace("OpStore %p %y","OpStore %p %y Volatile"));
        let output=lower(&input).unwrap(); validate(&output);
        let m=rspirv::dr::load_bytes(&output).unwrap();
        assert!(m.all_inst_iter().any(|i|i.operands.contains(&Operand::MemoryAccess(MemoryAccess::VOLATILE|MemoryAccess::ALIGNED))));
    }
    #[test] fn a_module_that_already_has_the_translators_table_is_left_byte_identical() {
        let input=source(&FIXTURE.replace("Binding 0","Binding 640")); assert_eq!(lower(&input).unwrap(), input);
    }
    // A fragment shader of Apple's GL reads a texture size from the translator's own table (uvec2 entries) and a
    // uniform buffer at binding 0. The buffer is lowered through that table; the table's reads keep their own
    // StorageBuffer pointer types although the buffer shares %pu with them, and the module still converts to the
    // Uniform-block table.
    const WITH_TABLE: &str = r#"
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buf %tab
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %arr ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 0
OpDecorate %tarr ArrayStride 8
OpDecorate %tblock Block
OpMemberDecorate %tblock 0 Offset 0
OpDecorate %tab DescriptorSet 0
OpDecorate %tab Binding 640
OpDecorate %tab NonWritable
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%v2uint = OpTypeVector %uint 2
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%c64 = OpConstant %uint 64
%arr = OpTypeRuntimeArray %uint
%block = OpTypeStruct %arr
%pb = OpTypePointer StorageBuffer %block
%pu = OpTypePointer StorageBuffer %uint
%buf = OpVariable %pb StorageBuffer
%tarr = OpTypeRuntimeArray %v2uint
%tblock = OpTypeStruct %tarr
%pt = OpTypePointer StorageBuffer %tblock
%tab = OpVariable %pt StorageBuffer
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%label = OpLabel
%size_p = OpAccessChain %pu %tab %zero %c64 %zero
%size = OpLoad %uint %size_p
%src = OpAccessChain %pu %buf %zero %one
%v = OpLoad %uint %src
%sum = OpIAdd %uint %size %v
%dst = OpAccessChain %pu %buf %zero %zero
OpStore %dst %sum
OpReturn
OpFunctionEnd
"#;
    #[test] fn buffers_are_lowered_through_a_table_the_translator_made() {
        let input=source(WITH_TABLE); let out=lower(&input).unwrap();
        assert_ne!(out, input); validate(&out);
        let m=rspirv::dr::load_bytes(&out).unwrap();
        let bindings: Vec<u32>=m.annotations.iter().filter_map(|a| match a.operands.as_slice() {
            [_, Operand::Decoration(Decoration::Binding), Operand::LiteralBit32(b)] => Some(*b), _ => None }).collect();
        assert_eq!(bindings, vec![TABLE_BINDING], "only the table keeps a descriptor");
        assert!(m.all_inst_iter().any(|i| i.class.opcode == Op::Bitcast), "the uvec2 entry is read as one 64-bit address");
        // the Uniform-block table is laid out like the translator's (scalar), as table_ubo's own tests check it
        let ubo=crate::table_ubo::convert(&out).unwrap();
        let w: Vec<u32>=ubo.chunks_exact(4).map(|w| u32::from_le_bytes(w.try_into().unwrap())).collect();
        let mut o=spirv_tools::val::ValidatorOptions::default(); o.scalar_block_layout=true;
        spirv_tools::val::create(Some(spirv_tools::TargetEnv::Vulkan_1_2)).validate(&w, Some(o)).unwrap();
    }
    #[test] fn a_table_with_a_nonzero_block_offset_is_not_an_address_table() {
        let input=source(&WITH_TABLE.replace("OpMemberDecorate %tblock 0 Offset 0", "OpMemberDecorate %tblock 0 Offset 16"));
        assert_eq!(lower(&input).unwrap(), input, "a different table layout must keep the descriptor path");
    }
    #[test] #[ignore] fn corpus_lowered_shaders_still_validate() {
        use metal2vulkan::passes::Stage; use std::collections::BTreeMap;
        let check=|b:&[u8]|->Result<(),String>{ let w:Vec<u32>=b.chunks_exact(4).map(|w|u32::from_le_bytes(w.try_into().unwrap())).collect();
            spirv_tools::val::create(Some(spirv_tools::TargetEnv::Vulkan_1_2)).validate(&w,None).map_err(|e|e.to_string()) };
        let mut files:Vec<_>=std::fs::read_dir("/Library/GPUBundles/nvmtl/aircache").unwrap().filter_map(|e|e.ok()).map(|e|e.path()).collect(); files.sort();
        let t0=std::time::Instant::now();
        let (mut units,mut lowered,mut refused,mut passthrough,mut ok_ok,mut bad_bad,mut fixed,mut broke)=(0,0,0,0,0,0,0,0);
        let mut broke_why:BTreeMap<String,(u32,String)>=BTreeMap::new();
        for f in files.iter().step_by(4) {
            if t0.elapsed().as_secs()>480 { println!("STOPPED EARLY at the 480 s cap"); break; }
            let ll=match std::fs::read_to_string(f){Ok(s)=>s,Err(_)=>continue};
            for (tag,st) in [("!air.vertex =",Stage::Vertex),("!air.fragment =",Stage::Fragment),("!air.kernel =",Stage::Kernel)] {
                if !ll.contains(tag) { continue; }
                let Ok(spv)=metal2vulkan::translate_native_no_retry(&ll,st) else { continue };
                units+=1;
                let out=match lower(&spv){Ok(o)=>o,Err(_)=>{refused+=1;continue}};
                if out==spv { passthrough+=1; continue; }
                lowered+=1;
                match (check(&spv).is_ok(),check(&out)) {
                    (true,Ok(()))=>ok_ok+=1, (false,Err(_))=>bad_bad+=1, (false,Ok(()))=>fixed+=1,
                    (true,Err(e))=>{ broke+=1; let k:String=e.chars().filter(|c|!c.is_ascii_digit()).take(140).collect();
                        let v=broke_why.entry(k).or_insert((0,f.file_name().unwrap().to_string_lossy().into())); v.0+=1; }
                }
            }
        }
        println!("CORPUS stride 4: units {units}  lowered {lowered}  refused {refused}  passthrough {passthrough}  in {} s",t0.elapsed().as_secs());
        println!("  valid->valid {ok_ok}   invalid->invalid {bad_bad} (translator baseline)   invalid->valid {fixed}   valid->INVALID {broke}");
        for (k,(n,eg)) in &broke_why { println!("    {n:5}  {k}   e.g. {eg}"); }
        assert!(lowered>100,"the corpus arm lowered only {lowered} shaders — it measured nothing");
        assert_eq!(broke,0,"{broke} shaders were valid before lowering and INVALID after");
    }
    // mtl_gpu_icb2's writer `o[t] = ...` with t = thread_position_in_grid: the translator adds the pushed thread base
    // (dispatch region member 3) to GlobalInvocationId. Binding 1 is an index buffer.
    const PUSH_INDEXED: &str = r#"
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buf %ibuf %gid %pc
OpExecutionMode %main LocalSize 32 1 1
OpDecorate %arr ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 0
OpDecorate %ibuf DescriptorSet 0
OpDecorate %ibuf Binding 1
OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %pcb Block
OpMemberDecorate %pcb 0 Offset 0
OpMemberDecorate %pcb 1 Offset 4
OpMemberDecorate %pcb 2 Offset 8
OpMemberDecorate %pcb 3 Offset 12
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%v3uint = OpTypeVector %uint 3
%zero = OpConstant %uint 0
%three = OpConstant %uint 3
%arr = OpTypeRuntimeArray %uint
%block = OpTypeStruct %arr
%pb = OpTypePointer StorageBuffer %block
%pu = OpTypePointer StorageBuffer %uint
%buf = OpVariable %pb StorageBuffer
%ibuf = OpVariable %pb StorageBuffer
%pin = OpTypePointer Input %v3uint
%gid = OpVariable %pin Input
%pcb = OpTypeStruct %uint %uint %uint %uint
%ppcb = OpTypePointer PushConstant %pcb
%ppcu = OpTypePointer PushConstant %uint
%pc = OpVariable %ppcb PushConstant
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%label = OpLabel
%g = OpLoad %v3uint %gid
%gx = OpCompositeExtract %uint %g 0
%basep = OpAccessChain %ppcu %pc %three
%base = OpLoad %uint %basep
%t = OpIAdd %uint %gx %base
%dst = OpAccessChain %pu %buf %zero %t
OpStore %dst %t
OpReturn
OpFunctionEnd
"#;
    fn kept_bindings(out: &[u8]) -> Vec<u32> {
        let m=rspirv::dr::load_bytes(out).unwrap();
        let mut b: Vec<u32>=m.annotations.iter().filter_map(|a| match a.operands.as_slice() {
            [_, Operand::Decoration(Decoration::Binding), Operand::LiteralBit32(b)] if *b != TABLE_BINDING => Some(*b), _ => None }).collect();
        b.sort(); b
    }
    #[test] fn an_index_from_push_constants_keeps_the_descriptor_unless_push_constants_are_not_data() {
        let input=source(PUSH_INDEXED); validate(&input);
        let before=lower_with_opts(&input, 0, false).unwrap(); validate(&before);
        assert!(kept_bindings(&before).contains(&0), "default: the pushed thread base makes o[t] data-dependent, o keeps its descriptor");
        let icb=lower_with_opts(&input, 0, true).unwrap(); validate(&icb);
        assert!(!kept_bindings(&icb).contains(&0), "push_not_data: o[t] is reached by address (a GPU-encoded ICB can set it)");
        assert_eq!(lower(&input).unwrap(), before, "lower() is unchanged");
    }
    #[test] fn an_index_read_from_a_buffer_keeps_the_descriptor_either_way() {
        let text=PUSH_INDEXED.replace("%dst = OpAccessChain %pu %buf %zero %t",
            "%ip = OpAccessChain %pu %ibuf %zero %t\n%i = OpLoad %uint %ip\n%dst = OpAccessChain %pu %buf %zero %i");
        let input=source(&text); validate(&input);
        let before=lower_with_opts(&input, 0, false).unwrap(); validate(&before);
        assert_eq!(kept_bindings(&before), vec![0, 1]);
        let icb=lower_with_opts(&input, 0, true).unwrap(); validate(&icb);
        assert_eq!(kept_bindings(&icb), vec![0], "idx[t] moves to an address; o[idx[t]] is data-driven and keeps its descriptor");
    }
    #[test] fn rejects_unknown_descriptor_abi() {
        assert!(lower(&source(&FIXTURE.replace("Binding 0","Binding 32"))).unwrap_err().contains("unsupported source buffer"));
    }
}
