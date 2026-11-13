/* SPDX-License-Identifier: MIT */
/* nvmtl_spv_literal.h: which operand words of a SPIR-V instruction are literals, for the plugin's module passes that look
   for every use of an id (cs-table-pc in nvmtl_table_pc.h, bufoff in nvmtl_bufoff_pc.h). Pure C, no Vulkan or ObjC.

   Those passes refuse a module when any word of an instruction equals the id they want to move or rewrite, because a
   word they cannot classify might be a use. A literal that merely equals the id then refuses the module for nothing,
   and small ids are common: the translator declares the compute dispatch block early (ids 3 to 30), so
   `OpVariable %p StorageBuffer` (storage class 12), `OpVariable %p Function` (7), `OpExtInst %t %r %glsl Sqrt` (31) or
   `OpTypeVector %v %f 3` refused it. On the Geekbench 7 kernels 14 of 115 kernels lost cs-table-pc that way (B580
   night of 13 Nov, DESCRIPTOR-MODEL.md section 16).

   nvmtl_spv_word_is_literal(i, k) answers 1 only for operand positions the SPIR-V grammar fixes as literals (or as
   label ids, which can never be another kind of id) in the instructions where they occur; every other word is still
   treated as a possible id, so an instruction this table does not know keeps the old, conservative answer. */
#ifndef NVMTL_SPV_LITERAL_H
#define NVMTL_SPV_LITERAL_H
#include <stdint.h>

/* i: the instruction's first word; k: the word index (1 = the first operand). wc must be the instruction's word count. */
static int nvmtl_spv_word_is_literal(const uint32_t *i, uint32_t k)
{
    const uint32_t op = i[0] & 0xffffu, wc = i[0] >> 16;
    if (!k || k >= wc) return 0;
    switch (op) {
    case 12:  return k == 4;                       /* OpExtInst: type, result, set, the instruction number, operands */
    case 23: case 24: return k == 3;               /* OpTypeVector / OpTypeMatrix: the component or column count */
    case 25:  return k >= 3;                       /* OpTypeImage: dim, depth, arrayed, MS, sampled, format, access */
    case 32: case 39: return k == 2;               /* OpTypePointer / OpTypeForwardPointer: the storage class */
    case 43: case 50: return k >= 3;               /* OpConstant / OpSpecConstant: the value */
    case 52:  return k == 3;                       /* OpSpecConstantOp: the opcode (its own literals stay counted) */
    case 54:  return k == 3;                       /* OpFunction: the function control mask */
    case 59:  return k == 3;                       /* OpVariable: the storage class */
    case 61:  return k >= 4;                       /* OpLoad: memory operands */
    case 62:  return k >= 3;                       /* OpStore: memory operands */
    case 63:  return k >= 3;                       /* OpCopyMemory: memory operands */
    case 64:  return k >= 4;                       /* OpCopyMemorySized: memory operands */
    case 79:  return k >= 5;                       /* OpVectorShuffle: the components */
    case 81:  return k >= 4;                       /* OpCompositeExtract: the indices */
    case 82:  return k >= 5;                       /* OpCompositeInsert: the indices */
    case 246: return k >= 3;                       /* OpLoopMerge: loop control and its literals (k 1, 2: labels) */
    case 247: return k >= 2;                       /* OpSelectionMerge: selection control */
    case 250: return k >= 4;                       /* OpBranchConditional: branch weights */
    case 251: return k >= 3;                       /* OpSwitch: case literals and their labels (labels are never other ids) */
    case 342: case 349: case 350: case 351: case 352: case 353: case 354: case 355: case 356: case 357: case 358:
    case 359: case 360: case 361: case 362:
              return k == 4;                       /* OpGroupNonUniformBallotBitCount, IAdd .. LogicalXor: the group operation */
    case 264: case 265: case 266: case 267: case 268: case 269: case 270:
              return k == 4;                       /* OpGroupIAdd .. OpGroupSMax: the group operation */
    case 87: case 88: case 91: case 92: case 95: case 98:
              return k == 5;                       /* image sample / fetch / read: the image operands mask */
    case 89: case 90: case 93: case 94: case 96: case 97:
              return k == 6;                       /* the Dref and gather forms: the image operands mask */
    case 99:  return k == 4;                       /* OpImageWrite: the image operands mask */
    default:  return 0;
    }
}
#endif
