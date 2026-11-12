//! Sparse-texture residency (`sparse_color<T>`, Metal 2.2+ `sparse_sample`, `sparse_read`, `sparse_gather`,
//! `sparse_sample_compare`, `sparse_gather_compare`).
//!
//! Apple's frontend emits no separate intrinsic for the sparse forms. Every texture sample, read and gather already
//! returns `{ T, i8 }` (e.g. `{ <4 x float>, i8 } @air.sample_texture_2d.v4f32`); the plain call ignores member 1 and the
//! sparse one extracts it: nonzero when the texels were NOT resident (the header passes a `bool *nack` to the builtin and
//! returns `{v, !nack}`). The lowerings fill member 1 with OpUndef, so a shader that reads it is detected here, after the
//! call is lowered, by its `extractvalue ..., 1` (or by the aggregate escaping whole into a phi, store or call).
//!
//! With `TransformOptions::sparse_residency` the lowered image instruction that produced the colour becomes its
//! OpImageSparse* twin (same operands, result `{ int code, T }`, Vulkan spec 15.x "Sparse Image Operations"), the colour
//! is member 1 of it and the AIR flag is `OpImageSparseTexelsResident(code) ? 0 : 1`. Without it (the device binds no
//! sparse textures) every texture is fully resident, which is what Metal reports for a texture that is not sparse, and
//! the flag is the constant 0. A colour the lowering computes from several texels or guards (pixel-coordinate samplers,
//! bicubic filtering, out-of-bounds guards) has no single image instruction whose residency is the call's, and is refused.
use super::*;

/// Whether `name` is a texture call whose `{ T, i8 }` result carries the residency flag.
pub(in crate::passes) fn carries_residency(name: &str) -> bool {
    [
        "air.sample_texture",
        "air.sample_depth",
        "air.sample_compare_depth",
        "air.gather_texture",
        "air.gather_depth",
        "air.gather_compare_depth",
        "air.read_texture",
        "air.read_depth",
    ]
    .iter()
    .any(|prefix| name.starts_with(prefix))
}

/// Whether the call returns `{ T, i8 }` (older AIR, and hand-written modules, return T alone) and member 1 (the
/// residency flag) of its result `res` is read anywhere in the function being lowered: an `extractvalue res, 1`, or the
/// aggregate used whole (phi, store, call argument, insert), which may reach a read of it.
pub(in crate::passes) fn residency_is_read(ctx: &Ctx, res: Word, rty: Word) -> bool {
    let flag_pair = type_def_of(ctx, rty).is_some_and(|def| {
        def.class.opcode == Op::TypeStruct
            && def.operands.len() == 2
            && matches!(def.operands[1], Operand::IdRef(member) if type_def_of(ctx, member).is_some_and(|m| {
                m.class.opcode == Op::TypeInt && m.operands.first() == Some(&Operand::LiteralBit32(8))
            }))
    });
    if !flag_pair {
        return false;
    }
    let Some(function) = ctx.air_call_body.as_ref().map(|body| body.function) else {
        return false;
    };
    ctx.module.functions[function].blocks.iter().any(|block| {
        block.instructions.iter().any(|inst| {
            if inst.result_id == Some(res) || !inst.operands.contains(&Operand::IdRef(res)) {
                return false;
            }
            match inst.class.opcode {
                Op::CompositeExtract => {
                    inst.operands.first() == Some(&Operand::IdRef(res))
                        && inst.operands.get(1) == Some(&Operand::LiteralBit32(1))
                }
                _ => true,
            }
        })
    })
}

fn sparse_twin(op: Op) -> Option<Op> {
    Some(match op {
        Op::ImageSampleImplicitLod => Op::ImageSparseSampleImplicitLod,
        Op::ImageSampleExplicitLod => Op::ImageSparseSampleExplicitLod,
        Op::ImageSampleDrefImplicitLod => Op::ImageSparseSampleDrefImplicitLod,
        Op::ImageSampleDrefExplicitLod => Op::ImageSparseSampleDrefExplicitLod,
        Op::ImageFetch => Op::ImageSparseFetch,
        Op::ImageGather => Op::ImageSparseGather,
        Op::ImageDrefGather => Op::ImageSparseDrefGather,
        Op::ImageRead => Op::ImageSparseRead,
        _ => return None,
    })
}

/// Fills the residency member of the lowered call `res` (see the module comment). `out` is the call's lowering, ending
/// in the `CompositeConstruct { colour, undef i8 }` of `res`; anything else is a lowering this does not know and is
/// refused rather than answered with a residency it did not compute.
pub(in crate::passes) fn lower_residency(
    ctx: &mut Ctx,
    name: &str,
    res: Word,
    mut out: Vec<Instruction>,
) -> Result<Vec<Instruction>, String> {
    let refuse = |why: &str| {
        Err(format!(
            "{name}: sparse_color::resident() is read, and {why}; its residency cannot be expressed"
        ))
    };
    let Some(at) = out.iter().rposition(|inst| {
        inst.result_id == Some(res)
            && inst.class.opcode == Op::CompositeConstruct
            && inst.operands.len() == 2
    }) else {
        return refuse("the call was not lowered to a { colour, flag } pair");
    };
    let (Operand::IdRef(colour), Operand::IdRef(flag)) =
        (out[at].operands[0].clone(), out[at].operands[1].clone())
    else {
        return refuse("the call was not lowered to a { colour, flag } pair");
    };
    let i8u = ctx.ty_int8();
    if !ctx.sparse_residency {
        let resident = ctx.const_int_of(i8u, 0);
        out[at].operands[1] = Operand::IdRef(resident);
        out.retain(|inst| !(inst.result_id == Some(flag) && inst.class.opcode == Op::Undef));
        return Ok(out);
    }
    // the instruction that made the colour: through the conversions the lowering adds to match the AIR result type
    let mut value = colour;
    let image_at = loop {
        let Some(i) = out.iter().position(|inst| inst.result_id == Some(value)) else {
            return refuse("its colour comes from outside the lowered call");
        };
        let inst = &out[i];
        if sparse_twin(inst.class.opcode).is_some() {
            break i;
        }
        match (inst.class.opcode, inst.operands.first()) {
            (
                Op::FConvert
                | Op::Bitcast
                | Op::CopyObject
                | Op::UConvert
                | Op::SConvert
                | Op::CompositeExtract,
                Some(Operand::IdRef(source)),
            ) => value = *source,
            (op, _) => {
                return refuse(&format!(
                    "the colour is computed ({op:?}) rather than read by one image instruction"
                ))
            }
        }
    };
    let image = out[image_at].clone();
    let Some(texel_ty) = image.result_type else {
        return refuse("its image instruction has no result type");
    };
    let int_ty = ctx.ty_uint();
    let pair_ty = ctx.get_or_create(
        Op::TypeStruct,
        None,
        vec![Operand::IdRef(int_ty), Operand::IdRef(texel_ty)],
    );
    let (pair, code, resident, nack) = (
        ctx.module.fresh_id(),
        ctx.module.fresh_id(),
        ctx.module.fresh_id(),
        ctx.module.fresh_id(),
    );
    let bool_ty = ctx.ty_bool();
    let (zero, one) = (ctx.const_int_of(i8u, 0), ctx.const_int_of(i8u, 1));
    let sparse_op = sparse_twin(image.class.opcode).expect("checked above");
    let replacement = vec![
        Instruction::new(sparse_op, Some(pair_ty), Some(pair), image.operands.clone()),
        Instruction::new(
            Op::CompositeExtract,
            Some(texel_ty),
            image.result_id,
            vec![Operand::IdRef(pair), Operand::LiteralBit32(1)],
        ),
        Instruction::new(
            Op::CompositeExtract,
            Some(int_ty),
            Some(code),
            vec![Operand::IdRef(pair), Operand::LiteralBit32(0)],
        ),
        Instruction::new(
            Op::ImageSparseTexelsResident,
            Some(bool_ty),
            Some(resident),
            vec![Operand::IdRef(code)],
        ),
        Instruction::new(
            Op::Select,
            Some(i8u),
            Some(nack),
            vec![Operand::IdRef(resident), Operand::IdRef(zero), Operand::IdRef(one)],
        ),
    ];
    out.splice(image_at..=image_at, replacement);
    let at = out
        .iter()
        .rposition(|inst| inst.result_id == Some(res))
        .expect("the pair is still there");
    out[at].operands[1] = Operand::IdRef(nack);
    // the OpUndef the lowering made for the flag is now unused
    out.retain(|inst| !(inst.result_id == Some(flag) && inst.class.opcode == Op::Undef));
    ctx.add_capability(spirv::Capability::SparseResidency);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_texture_read_that_returns_a_flag_carries_residency() {
        for name in [
            "air.sample_texture_2d.v4f32",
            "air.sample_texture_cube_array.v4f16",
            "air.sample_depth_2d.f32",
            "air.sample_compare_depth_2d.f32",
            "air.gather_texture_2d.v4f32",
            "air.gather_compare_depth_2d.f32",
            "air.read_texture_3d.u.v4i32",
            "air.read_depth_2d.f32",
        ] {
            assert!(carries_residency(name), "{name}");
        }
        for name in ["air.write_texture_2d.v4f32", "air.get_width_texture_2d", "air.calculate_clamped_lod_texture_2d"] {
            assert!(!carries_residency(name), "{name}");
        }
    }

    #[test]
    fn each_image_read_has_its_sparse_twin_and_nothing_else_does() {
        for (plain, sparse) in [
            (Op::ImageSampleImplicitLod, Op::ImageSparseSampleImplicitLod),
            (Op::ImageSampleExplicitLod, Op::ImageSparseSampleExplicitLod),
            (Op::ImageSampleDrefImplicitLod, Op::ImageSparseSampleDrefImplicitLod),
            (Op::ImageSampleDrefExplicitLod, Op::ImageSparseSampleDrefExplicitLod),
            (Op::ImageFetch, Op::ImageSparseFetch),
            (Op::ImageGather, Op::ImageSparseGather),
            (Op::ImageDrefGather, Op::ImageSparseDrefGather),
            (Op::ImageRead, Op::ImageSparseRead),
        ] {
            assert_eq!(sparse_twin(plain), Some(sparse));
        }
        // projective samples are never emitted for Metal, and a query has no residency
        assert_eq!(sparse_twin(Op::ImageSampleProjImplicitLod), None);
        assert_eq!(sparse_twin(Op::ImageQueryLod), None);
    }
}
