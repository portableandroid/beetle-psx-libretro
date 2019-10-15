/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "disassembler.h"
#include "lightrec.h"
#include "memmanager.h"
#include "optimizer.h"
#include "regcache.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

struct optimizer_list {
	void (**optimizers)(struct opcode *);
	unsigned int nb_optimizers;
};

bool opcode_reads_register(union code op, u8 reg)
{
	switch (op.i.op) {
	case OP_SPECIAL:
		switch (op.r.op) {
		case OP_SPECIAL_SYSCALL:
		case OP_SPECIAL_BREAK:
			return false;
		case OP_SPECIAL_JR:
		case OP_SPECIAL_JALR:
		case OP_SPECIAL_MTHI:
		case OP_SPECIAL_MTLO:
			return op.r.rs == reg;
		case OP_SPECIAL_MFHI:
			return reg == REG_HI;
		case OP_SPECIAL_MFLO:
			return reg == REG_LO;
		case OP_SPECIAL_SLL:
		case OP_SPECIAL_SRL:
		case OP_SPECIAL_SRA:
			return op.r.rt == reg;
		default:
			return op.r.rs == reg || op.r.rt == reg;
		}
	case OP_CP0:
		switch (op.r.rs) {
		case OP_CP0_MTC0:
		case OP_CP0_CTC0:
			return op.r.rt == reg;
		default:
			return false;
		}
	case OP_CP2:
		if (op.r.op == OP_CP2_BASIC) {
			switch (op.r.rs) {
			case OP_CP2_BASIC_MTC2:
			case OP_CP2_BASIC_CTC2:
				return op.r.rt == reg;
			default:
				return false;
			}
		} else {
			return false;
		}
	case OP_J:
	case OP_JAL:
	case OP_LUI:
		return false;
	case OP_BEQ:
	case OP_BNE:
	case OP_LWL:
	case OP_LWR:
	case OP_SB:
	case OP_SH:
	case OP_SWL:
	case OP_SW:
	case OP_SWR:
		return op.i.rs == reg || op.i.rt == reg;
	default:
		return op.i.rs == reg;
	}
}

bool opcode_writes_register(union code op, u8 reg)
{
	switch (op.i.op) {
	case OP_SPECIAL:
		switch (op.r.op) {
		case OP_SPECIAL_JR:
		case OP_SPECIAL_JALR:
		case OP_SPECIAL_SYSCALL:
		case OP_SPECIAL_BREAK:
			return false;
		case OP_SPECIAL_MULT:
		case OP_SPECIAL_MULTU:
		case OP_SPECIAL_DIV:
		case OP_SPECIAL_DIVU:
			return reg == REG_LO || reg == REG_HI;
		case OP_SPECIAL_MTHI:
			return reg == REG_HI;
		case OP_SPECIAL_MTLO:
			return reg == REG_LO;
		default:
			return op.r.rd == reg;
		}
	case OP_ADDI:
	case OP_ADDIU:
	case OP_SLTI:
	case OP_SLTIU:
	case OP_ANDI:
	case OP_ORI:
	case OP_XORI:
	case OP_LUI:
	case OP_LB:
	case OP_LH:
	case OP_LWL:
	case OP_LW:
	case OP_LBU:
	case OP_LHU:
	case OP_LWR:
		return op.i.rt == reg;
	case OP_CP0:
		switch (op.r.rs) {
		case OP_CP0_MFC0:
		case OP_CP0_CFC0:
			return op.i.rt == reg;
		default:
			return false;
		}
	case OP_META_MOV:
		return op.r.rd == reg;
	default:
		return false;
	}
}

/* TODO: Complete */
static bool is_nop(union code op)
{
	if (opcode_writes_register(op, 0)) {
		switch (op.i.op) {
		case OP_CP0:
			return op.r.rs != OP_CP0_MFC0;
		case OP_LB:
		case OP_LH:
		case OP_LWL:
		case OP_LW:
		case OP_LBU:
		case OP_LHU:
		case OP_LWR:
			return false;
		default:
			return true;
		}
	}

	switch (op.i.op) {
	case OP_SPECIAL:
		switch (op.r.op) {
		case OP_SPECIAL_AND:
			return op.r.rd == op.r.rt && op.r.rd == op.r.rs;
		case OP_SPECIAL_ADD:
		case OP_SPECIAL_ADDU:
			return (op.r.rd == op.r.rt && op.r.rs == 0) ||
				(op.r.rd == op.r.rs && op.r.rt == 0);
		case OP_SPECIAL_SUB:
		case OP_SPECIAL_SUBU:
			return op.r.rd == op.r.rs && op.r.rt == 0;
		case OP_SPECIAL_OR:
			if (op.r.rd == op.r.rt)
				return op.r.rd == op.r.rs || op.r.rs == 0;
			else
				return (op.r.rd == op.r.rs) && op.r.rt == 0;
		case OP_SPECIAL_SLL:
		case OP_SPECIAL_SRA:
		case OP_SPECIAL_SRL:
			return op.r.imm == 0;
		default:
			return false;
		}
	case OP_ORI:
	case OP_ADDI:
	case OP_ADDIU:
		return op.i.rt == op.i.rs && op.i.imm == 0;
	case OP_BGTZ:
		return (op.i.rs == 0 || op.i.imm == 1);
	case OP_REGIMM:
		return (op.i.op == OP_REGIMM_BLTZ ||
				op.i.op == OP_REGIMM_BLTZAL) &&
			(op.i.rs == 0 || op.i.imm == 1);
	case OP_BNE:
		return (op.i.rs == op.i.rt || op.i.imm == 1);
	default:
		return false;
	}
}

bool load_in_delay_slot(union code op)
{
	switch (op.i.op) {
	case OP_CP0:
		switch (op.r.rs) {
		case OP_CP0_MFC0:
		case OP_CP0_CFC0:
			return true;
		default:
			break;
		}

		break;
	case OP_CP2:
		if (op.r.op == OP_CP2_BASIC) {
			switch (op.r.rs) {
			case OP_CP2_BASIC_MFC2:
			case OP_CP2_BASIC_CFC2:
				return true;
			default:
				break;
			}
		}

		break;
	case OP_LWC2:
	case OP_LB:
	case OP_LH:
	case OP_LW:
	case OP_LWL:
	case OP_LWR:
	case OP_LBU:
	case OP_LHU:
		return true;
	default:
		break;
	}

	return false;
}

static u32 lightrec_propagate_consts(union code c, u32 known, u32 *v)
{
	switch (c.i.op) {
	case OP_SPECIAL:
		switch (c.r.op) {
		case OP_SPECIAL_SLL:
			if (known & BIT(c.r.rt)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] << c.r.imm;
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SRL:
			if (known & BIT(c.r.rt)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] >> c.r.imm;
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SRA:
			if (known & BIT(c.r.rt)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = (s32)v[c.r.rt] >> c.r.imm;
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SLLV:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] << (v[c.r.rs] & 0x1f);
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SRLV:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] >> (v[c.r.rs] & 0x1f);
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SRAV:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = (s32)v[c.r.rt]
					  >> (v[c.r.rs] & 0x1f);
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_ADD:
		case OP_SPECIAL_ADDU:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = (s32)v[c.r.rt] + (s32)v[c.r.rs];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SUB:
		case OP_SPECIAL_SUBU:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] - v[c.r.rs];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_AND:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] & v[c.r.rs];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_OR:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] | v[c.r.rs];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_XOR:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rt] ^ v[c.r.rs];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_NOR:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = ~(v[c.r.rt] | v[c.r.rs]);
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SLT:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = (s32)v[c.r.rs] < (s32)v[c.r.rt];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		case OP_SPECIAL_SLTU:
			if (known & BIT(c.r.rt) && known & BIT(c.r.rs)) {
				known |= BIT(c.r.rd);
				v[c.r.rd] = v[c.r.rs] < v[c.r.rt];
			} else {
				known &= ~BIT(c.r.rd);
			}
			break;
		default:
			break;
		}
		break;
	case OP_REGIMM:
		break;
	case OP_ADDI:
	case OP_ADDIU:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = v[c.i.rs] + (s32)(s16)c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_SLTI:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = (s32)v[c.i.rs] < (s32)(s16)c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_SLTIU:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = v[c.i.rs] < (u32)(s32)(s16)c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_ANDI:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = v[c.i.rs] & c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_ORI:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = v[c.i.rs] | c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_XORI:
		if (known & BIT(c.i.rs)) {
			known |= BIT(c.i.rt);
			v[c.i.rt] = v[c.i.rs] ^ c.i.imm;
		} else {
			known &= ~BIT(c.i.rt);
		}
		break;
	case OP_LUI:
		known |= BIT(c.i.rt);
		v[c.i.rt] = c.i.imm << 16;
		break;
	case OP_CP0:
		switch (c.r.rs) {
		case OP_CP0_MFC0:
		case OP_CP0_CFC0:
			known &= ~BIT(c.r.rt);
			break;
		}
		break;
	case OP_CP2:
		if (c.r.op == OP_CP2_BASIC) {
			switch (c.r.rs) {
			case OP_CP2_BASIC_MFC2:
			case OP_CP2_BASIC_CFC2:
				known &= ~BIT(c.r.rt);
				break;
			}
		}
		break;
	case OP_LB:
	case OP_LH:
	case OP_LWL:
	case OP_LW:
	case OP_LBU:
	case OP_LHU:
	case OP_LWR:
	case OP_LWC2:
		known &= ~BIT(c.i.rt);
		break;
	case OP_META_MOV:
		if (known & BIT(c.r.rs)) {
			known |= BIT(c.r.rd);
			v[c.r.rd] = v[c.r.rs];
		} else {
			known &= ~BIT(c.r.rd);
		}
		break;
	default:
		break;
	}

	return known;
}

static int lightrec_transform_ops(struct block *block)
{
	struct opcode *list = block->opcode_list;

	for (; list; list = list->next) {

		/* Transform all opcodes detected as useless to real NOPs
		 * (0x0: SLL r0, r0, #0) */
		if (list->opcode != 0 && is_nop(list->c)) {
			pr_debug("Converting useless opcode 0x%08x to NOP\n",
					list->opcode);
			list->opcode = 0x0;
			continue;
		}

		switch (list->i.op) {
		/* Transform BEQ / BNE to BEQZ / BNEZ meta-opcodes if one of the
		 * two registers is zero. */
		case OP_BEQ:
			if ((list->i.rs == 0) ^ (list->i.rt == 0)) {
				list->i.op = OP_META_BEQZ;
				if (list->i.rs == 0) {
					list->i.rs = list->i.rt;
					list->i.rt = 0;
				}
			}
			break;
		case OP_BNE:
			if (list->i.rs == 0) {
				list->i.op = OP_META_BNEZ;
				list->i.rs = list->i.rt;
				list->i.rt = 0;
			} else if (list->i.rt == 0) {
				list->i.op = OP_META_BNEZ;
			}
			break;

		/* Transform ORI/ADDI/ADDIU with imm #0 or ORR/ADD/ADDU/SUB/SUBU
		 * with register $zero to the MOV meta-opcode */
		case OP_ORI:
		case OP_ADDI:
		case OP_ADDIU:
			if (list->i.imm == 0) {
				pr_debug("Convert ORI/ADDI/ADDIU #0 to MOV\n");
				list->i.op = OP_META_MOV;
				list->r.rd = list->i.rt;
			}
			break;
		case OP_SPECIAL:
			switch (list->r.op) {
			case OP_SPECIAL_OR:
			case OP_SPECIAL_ADD:
			case OP_SPECIAL_ADDU:
				if (list->r.rs == 0) {
					pr_debug("Convert OR/ADD $zero to MOV\n");
					list->i.op = OP_META_MOV;
					list->r.rs = list->r.rt;
				}
			case OP_SPECIAL_SUB: /* fall-through */
			case OP_SPECIAL_SUBU:
				if (list->r.rt == 0) {
					pr_debug("Convert OR/ADD/SUB $zero to MOV\n");
					list->i.op = OP_META_MOV;
				}
			default: /* fall-through */
				break;
			}
		default: /* fall-through */
			break;
		}
	}

	return 0;
}

static int lightrec_switch_delay_slots(struct block *block)
{
	struct opcode *list;
	u8 flags;

	for (list = block->opcode_list; list->next; list = list->next) {
		union code op = list->c;
		union code next_op = list->next->c;

		if (!has_delay_slot(op) ||
		    (list->flags & LIGHTREC_NO_DS) ||
		    next_op.opcode == 0 ||
		    load_in_delay_slot(next_op) ||
		    has_delay_slot(next_op))
			continue;

		switch (list->i.op) {
		case OP_SPECIAL:
			switch (op.r.op) {
			case OP_SPECIAL_JALR:
				if (opcode_reads_register(next_op, op.r.rd) ||
				    opcode_writes_register(next_op, op.r.rd))
					continue;
			case OP_SPECIAL_JR: /* fall-through */
				if (opcode_writes_register(next_op, op.r.rs))
					continue;
			default: /* fall-through */
				break;
			}
		case OP_J: /* fall-through */
			break;
		case OP_JAL:
			if (opcode_reads_register(next_op, 31) ||
			    opcode_writes_register(next_op, 31))
				continue;
			else
				break;
		case OP_BEQ:
		case OP_BNE:
			if (opcode_writes_register(next_op, op.i.rt))
				continue;
		case OP_BLEZ: /* fall-through */
		case OP_BGTZ:
			if (opcode_writes_register(next_op, op.i.rs))
				continue;
			break;
		case OP_REGIMM:
			switch (op.r.rt) {
			case OP_REGIMM_BLTZAL:
			case OP_REGIMM_BGEZAL:
				if (opcode_reads_register(next_op, 31) ||
				    opcode_writes_register(next_op, 31))
					continue;
			case OP_REGIMM_BLTZ: /* fall-through */
			case OP_REGIMM_BGEZ:
				if (opcode_writes_register(next_op, op.i.rs))
					continue;
				break;
			}
			break;
		case OP_META_BEQZ:
		case OP_META_BNEZ:
			if (opcode_writes_register(next_op, op.i.rs))
				continue;
		default: /* fall-through */
			break;
		}

		pr_debug("Swap branch and delay slot opcodes "
			 "at offsets 0x%x / 0x%x\n", list->offset << 2,
			 list->next->offset << 2);

		flags = list->next->flags;
		list->c = next_op;
		list->next->c = op;
		list->next->flags = list->flags | LIGHTREC_NO_DS;
		list->flags = flags;
		list->offset++;
		list->next->offset--;
	}

	return 0;
}

bool has_delay_slot(union code op)
{
	switch (op.i.op) {
	case OP_SPECIAL:
		switch (op.r.op) {
		case OP_SPECIAL_JR:
		case OP_SPECIAL_JALR:
			return true;
		default:
			return false;
		}
	case OP_J:
	case OP_JAL:
	case OP_BEQ:
	case OP_BNE:
	case OP_BLEZ:
	case OP_BGTZ:
	case OP_REGIMM:
	case OP_META_BEQZ:
	case OP_META_BNEZ:
		return true;
	default:
		return false;
	}
}

static int lightrec_add_unload(struct opcode *op, u8 reg)
{
	struct opcode *meta = lightrec_malloc(MEM_FOR_IR, sizeof(*meta));

	if (!meta)
		return -ENOMEM;

	meta->i.op = OP_META_REG_UNLOAD;
	meta->i.rs = reg;
	meta->flags = 0;
	meta->offset = op->offset;
	meta->next = op->next;
	op->next = meta;

	return 0;
}

static int lightrec_early_unload(struct block *block)
{
	struct opcode *list = block->opcode_list;
	u8 i;

	for (i = 1; i < 34; i++) {
		struct opcode *op, *last_r = NULL, *last_w = NULL;
		unsigned int last_r_id = 0, last_w_id = 0, id = 0;
		int ret;

		for (op = list; op->next; op = op->next, id++) {
			if (opcode_reads_register(op->c, i)) {
				last_r = op;
				last_r_id = id;
			}

			if (opcode_writes_register(op->c, i)) {
				last_w = op;
				last_w_id = id;
			}
		}

		if (last_w_id > last_r_id) {
			if (has_delay_slot(last_w->c) &&
			    !(last_w->flags & LIGHTREC_NO_DS))
				last_w = last_w->next;

			if (last_w->next) {
				ret = lightrec_add_unload(last_w, i);
				if (ret)
					return ret;
			}
		} else if (last_r) {
			if (has_delay_slot(last_r->c) &&
			    !(last_r->flags & LIGHTREC_NO_DS))
				last_r = last_r->next;

			if (last_r->next) {
				ret = lightrec_add_unload(last_r, i);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int lightrec_constant_folding(struct block *block)
{
	struct opcode *list;
	u32 known = BIT(0);
	u32 values[32] = { 0 };

	for (list = block->opcode_list; list; list = list->next) {
		/* Register $zero is always, well, zero */
		known |= BIT(0);
		values[0] = 0;

		switch (list->i.op) {
		case OP_LUI:
			if ((known & BIT(list->i.rt)) &&
			    values[list->i.rt] == (list->i.imm << 16)) {
				pr_debug("Remove duplicated LUI opcode\n");
				list->opcode = 0; /* NOP */
			}
			break;
		case OP_SB:
		case OP_SH:
		case OP_SW:
			/* Mark all store operations that target $sp, $gp, $k0
			 * or $k1 as not requiring code invalidation. This is
			 * based on the heuristic that stores using one of these
			 * registers as address will never hit a code page. */
			if (list->i.rs >= 26 && list->i.rs <= 29) {
				pr_debug("Flaging opcode 0x%08x as not requiring invalidation\n",
					 list->opcode);
				list->flags |= LIGHTREC_NO_INVALIDATE;
			}

			/* Detect writes whose destination address is inside the
			 * current block, using constant propagation. When these
			 * occur, we mark the blocks as not compilable. */
			if ((known & BIT(list->i.rs)) &&
			    kunseg(values[list->i.rs]) >= block->kunseg_pc &&
			    kunseg(values[list->i.rs]) < (block->kunseg_pc +
							  block->nb_ops * 4)) {
				pr_debug("Self-modifying block detected\n");
				block->flags |= BLOCK_NEVER_COMPILE;
				list->flags |= LIGHTREC_SMC;
			}
		default: /* fall-through */
			break;
		}

		known = lightrec_propagate_consts(list->c, known, values);
	}

	return 0;
}

static int (*lightrec_optimizers[])(struct block *) = {
	&lightrec_transform_ops,
	&lightrec_switch_delay_slots,
	&lightrec_constant_folding,
	&lightrec_early_unload,
};

int lightrec_optimize(struct block *block)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lightrec_optimizers); i++) {
		int ret = lightrec_optimizers[i](block);

		if (ret)
			return ret;
	}

	return 0;
}
