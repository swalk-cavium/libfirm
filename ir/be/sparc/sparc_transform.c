/*
 * Copyright (C) 1995-2010 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   code selection (transform FIRM into SPARC FIRM)
 * @version $Id$
 */

#include "config.h"

#include "irnode_t.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irgmod.h"
#include "iredges.h"
#include "irvrfy.h"
#include "ircons.h"
#include "irprintf.h"
#include "dbginfo.h"
#include "iropt_t.h"
#include "debug.h"
#include "error.h"

#include "../benode.h"
#include "../beirg.h"
#include "../beutil.h"
#include "../betranshlp.h"
#include "bearch_sparc_t.h"

#include "sparc_nodes_attr.h"
#include "sparc_transform.h"
#include "sparc_new_nodes.h"
#include "gen_sparc_new_nodes.h"

#include "gen_sparc_regalloc_if.h"

#include <limits.h>

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static sparc_code_gen_t *env_cg;

static ir_node *gen_SymConst(ir_node *node);


static inline int mode_needs_gp_reg(ir_mode *mode)
{
	return mode_is_int(mode) || mode_is_reference(mode);
}

/**
 * Create an And that will zero out upper bits.
 *
 * @param dbgi     debug info
 * @param block    the basic block
 * @param op       the original node
 * @param src_bits  number of lower bits that will remain
 */
static ir_node *gen_zero_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   int src_bits)
{
	if (src_bits == 8) {
		return new_bd_sparc_And_imm(dbgi, block, op, 0xFF);
	} else if (src_bits == 16) {
		ir_node *lshift = new_bd_sparc_Sll_imm(dbgi, block, op, 16);
		ir_node *rshift = new_bd_sparc_Slr_imm(dbgi, block, lshift, 16);
		return rshift;
	} else {
		panic("zero extension only supported for 8 and 16 bits");
	}
}

/**
 * Generate code for a sign extension.
 */
static ir_node *gen_sign_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                                   int src_bits)
{
	int shift_width = 32 - src_bits;
	ir_node *lshift_node = new_bd_sparc_Sll_imm(dbgi, block, op, shift_width);
	ir_node *rshift_node = new_bd_sparc_Sra_imm(dbgi, block, lshift_node, shift_width);
	return rshift_node;
}

/**
 * returns true if it is assured, that the upper bits of a node are "clean"
 * which means for a 16 or 8 bit value, that the upper bits in the register
 * are 0 for unsigned and a copy of the last significant bit for signed
 * numbers.
 */
static bool upper_bits_clean(ir_node *transformed_node, ir_mode *mode)
{
	(void) transformed_node;
	(void) mode;
	/* TODO */
	return false;
}

static ir_node *gen_extension(dbg_info *dbgi, ir_node *block, ir_node *op,
                              ir_mode *orig_mode)
{
	int bits = get_mode_size_bits(orig_mode);
	if (bits == 32)
		return op;

	if (mode_is_signed(orig_mode)) {
		return gen_sign_extension(dbgi, block, op, bits);
	} else {
		return gen_zero_extension(dbgi, block, op, bits);
	}
}


/**
 * Creates a possible DAG for a constant.
 */
static ir_node *create_const_graph_value(dbg_info *dbgi, ir_node *block,
				long value)
{
	ir_node *result;

	// we need to load hi & lo separately
	if (value < -4096 || value > 4095) {
		ir_node *hi = new_bd_sparc_HiImm(dbgi, block, (int) value);
		result = new_bd_sparc_LoImm(dbgi, block, hi, value);
		be_dep_on_frame(hi);
	} else {
		result = new_bd_sparc_Mov_imm(dbgi, block, (int) value);
		be_dep_on_frame(result);
	}

	return result;
}


/**
 * Create a DAG constructing a given Const.
 *
 * @param irn  a Firm const
 */
static ir_node *create_const_graph(ir_node *irn, ir_node *block)
{
	tarval  *tv = get_Const_tarval(irn);
	ir_mode *mode = get_tarval_mode(tv);
	dbg_info *dbgi = get_irn_dbg_info(irn);
	long value;


	if (mode_is_reference(mode)) {
		/* SPARC V8 is 32bit, so we can safely convert a reference tarval into Iu */
		assert(get_mode_size_bits(mode) == get_mode_size_bits(mode_Iu));
		tv = tarval_convert_to(tv, mode_Iu);
	}

	value = get_tarval_long(tv);
	return create_const_graph_value(dbgi, block, value);
}

/**
 * create a DAG to load fp constant. sparc only supports loading from global memory
 */
static ir_node *create_fp_const_graph(ir_node *irn, ir_node *block)
{
	(void) block;
	(void) irn;
	panic("FP constants not implemented");
}


typedef enum {
	MATCH_NONE         = 0,
	MATCH_COMMUTATIVE  = 1 << 0,
	MATCH_SIZE_NEUTRAL = 1 << 1,
} match_flags_t;

typedef ir_node* (*new_binop_reg_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2);
typedef ir_node* (*new_binop_fp_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2, ir_mode *mode);
typedef ir_node* (*new_binop_imm_func) (dbg_info *dbgi, ir_node *block, ir_node *op1, int simm13);

/**
 * checks if a node's value can be encoded as a immediate
 *
 */
static bool is_imm_encodeable(const ir_node *node)
{
	long val;

	//assert(mode_is_float_vector(get_irn_mode(node)));

	if (!is_Const(node))
		return false;

	val = get_tarval_long(get_Const_tarval(node));

	return !(val < -4096 || val > 4095);
}

/**
 * helper function for binop operations
 *
 * @param new_binop_reg_func register generation function ptr
 * @param new_binop_imm_func immediate generation function ptr
 */
static ir_node *gen_helper_binop(ir_node *node, match_flags_t flags,
				new_binop_reg_func new_reg, new_binop_imm_func new_imm)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_binop_left(node);
	ir_node  *new_op1;
	ir_node  *op2     = get_binop_right(node);
	ir_node  *new_op2;
	dbg_info *dbgi    = get_irn_dbg_info(node);

	if (is_imm_encodeable(op2)) {
		ir_node *new_op1 = be_transform_node(op1);
		return new_imm(dbgi, block, new_op1, get_tarval_long(get_Const_tarval(op2)));
	}

	new_op2 = be_transform_node(op2);

	if ((flags & MATCH_COMMUTATIVE) && is_imm_encodeable(op1)) {
		return new_imm(dbgi, block, new_op2, get_tarval_long(get_Const_tarval(op1)) );
	}

	new_op1 = be_transform_node(op1);

	return new_reg(dbgi, block, new_op1, new_op2);
}

/**
 * helper function for FP binop operations
 */
static ir_node *gen_helper_binfpop(ir_node *node, new_binop_fp_func new_reg)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op1     = get_binop_left(node);
	ir_node  *new_op1;
	ir_node  *op2     = get_binop_right(node);
	ir_node  *new_op2;
	dbg_info *dbgi    = get_irn_dbg_info(node);

	new_op2 = be_transform_node(op2);
	new_op1 = be_transform_node(op1);
	return new_reg(dbgi, block, new_op1, new_op2, get_irn_mode(node));
}

/**
 * Creates an sparc Add.
 *
 * @param node   FIRM node
 * @return the created sparc Add node
 */
static ir_node *gen_Add(ir_node *node)
{
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi    = get_irn_dbg_info(node);

	(void) block;
	(void) dbgi;

	if (mode_is_float(mode))
		panic("FP not implemented yet");

	return gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL, new_bd_sparc_Add_reg, new_bd_sparc_Add_imm);
}


/**
 * Creates an sparc Sub.
 *
 * @param node       FIRM node
 * @return the created sparc Sub node
 */
static ir_node *gen_Sub(ir_node *node)
{
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi    = get_irn_dbg_info(node);

	(void) block;
	(void) dbgi;

	if (mode_is_float(mode))
		panic("FP not implemented yet");

	return gen_helper_binop(node, MATCH_SIZE_NEUTRAL, new_bd_sparc_Sub_reg, new_bd_sparc_Sub_imm);
}


/**
 * Transforms a Load.
 *
 * @param node    the ir Load node
 * @return the created sparc Load node
 */
static ir_node *gen_Load(ir_node *node)
{
	ir_mode  *mode     = get_Load_mode(node);
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Load_ptr(node);
	ir_node  *new_ptr  = be_transform_node(ptr);
	ir_node  *mem      = get_Load_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	dbg_info *dbgi      = get_irn_dbg_info(node);
	ir_node  *new_load = NULL;

	if (mode_is_float(mode))
		panic("SPARC: no fp implementation yet");

	new_load = new_bd_sparc_Ld(dbgi, block, new_ptr, new_mem, mode, NULL, 0, 0, false);
	set_irn_pinned(new_load, get_irn_pinned(node));

	return new_load;
}



/**
 * Transforms a Store.
 *
 * @param node    the ir Store node
 * @return the created sparc Store node
 */
static ir_node *gen_Store(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *ptr      = get_Store_ptr(node);
	ir_node  *new_ptr  = be_transform_node(ptr);
	ir_node  *mem      = get_Store_mem(node);
	ir_node  *new_mem  = be_transform_node(mem);
	ir_node  *val      = get_Store_value(node);
	ir_node  *new_val  = be_transform_node(val);
	ir_mode  *mode     = get_irn_mode(val);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node *new_store = NULL;

	if (mode_is_float(mode))
		panic("SPARC: no fp implementation yet");

	new_store = new_bd_sparc_St(dbgi, block, new_ptr, new_val, new_mem, mode, NULL, 0, 0, false);

	return new_store;
}

/**
 * Creates an sparc Mul.
 * returns the lower 32bits of the 64bit multiply result
 *
 * @return the created sparc Mul node
 */
static ir_node *gen_Mul(ir_node *node) {
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);

	ir_node *mul;
	ir_node *proj_res_low;

	if (mode_is_float(mode)) {
		mul = gen_helper_binfpop(node, new_bd_sparc_fMul);
		return mul;
	}

	assert(mode_is_data(mode));
	mul = gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL, new_bd_sparc_Mul_reg, new_bd_sparc_Mul_imm);
	arch_irn_add_flags(mul, arch_irn_flags_modify_flags);

	proj_res_low = new_rd_Proj(dbgi, mul, mode_Iu, pn_sparc_Mul_low);
	return proj_res_low;
}

/**
 * Creates an sparc Mulh.
 * Mulh returns the upper 32bits of a mul instruction
 *
 * @return the created sparc Mulh node
 */
static ir_node *gen_Mulh(ir_node *node) {
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);

	ir_node *mul;
	ir_node *proj_res_hi;

	if (mode_is_float(mode))
		panic("FP not supported yet");


	assert(mode_is_data(mode));
	mul = gen_helper_binop(node, MATCH_COMMUTATIVE | MATCH_SIZE_NEUTRAL, new_bd_sparc_Mulh_reg, new_bd_sparc_Mulh_imm);
	//arch_irn_add_flags(mul, arch_irn_flags_modify_flags);
	proj_res_hi = new_rd_Proj(dbgi, mul, mode_Iu, pn_sparc_Mulh_low);
	return proj_res_hi;
}

/**
 * Creates an sparc Div.
 *
 * @return the created sparc Div node
 */
static ir_node *gen_Div(ir_node *node) {

	ir_mode  *mode    = get_irn_mode(node);

	ir_node *div;

	if (mode_is_float(mode))
		panic("FP not supported yet");

	//assert(mode_is_data(mode));
	div = gen_helper_binop(node, MATCH_SIZE_NEUTRAL, new_bd_sparc_Div_reg, new_bd_sparc_Div_imm);
	return div;
}


/**
 * transform abs node:
 * mov a, b
 * sra b, 31, b
 * xor a, b
 * sub a, b
 *
 * @return
 */
static ir_node *gen_Abs(ir_node *node) {
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_mode  *mode    = get_irn_mode(node);
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_node   *op     = get_Abs_op(node);

	ir_node *mov, *sra, *xor, *sub, *new_op;

	if (mode_is_float(mode))
		panic("FP not supported yet");

	new_op = be_transform_node(op);

	mov = new_bd_sparc_Mov_reg(dbgi, block, new_op);
	sra = new_bd_sparc_Sra_imm(dbgi, block, mov, 31);
	xor = new_bd_sparc_Xor_reg(dbgi, block, new_op, sra);
	sub = new_bd_sparc_Sub_reg(dbgi, block, sra, xor);

	return sub;
}

/**
 * Transforms a Not node.
 *
 * @return the created ARM Not node
 */
static ir_node *gen_Not(ir_node *node)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op      = get_Not_op(node);
	ir_node  *new_op  = be_transform_node(op);
	dbg_info *dbgi    = get_irn_dbg_info(node);

	return new_bd_sparc_Not(dbgi, block, new_op);
}

static ir_node *gen_And(ir_node *node)
{
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi    = get_irn_dbg_info(node);

	(void) block;
	(void) dbgi;

	if (mode_is_float(mode))
		panic("FP not implemented yet");

	return gen_helper_binop(node, MATCH_COMMUTATIVE, new_bd_sparc_And_reg, new_bd_sparc_And_imm);
}

static ir_node *gen_Or(ir_node *node)
{
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi    = get_irn_dbg_info(node);

	(void) block;
	(void) dbgi;

	if (mode_is_float(mode))
		panic("FP not implemented yet");

	return gen_helper_binop(node, MATCH_COMMUTATIVE, new_bd_sparc_Or_reg, new_bd_sparc_Or_imm);
}

static ir_node *gen_Xor(ir_node *node)
{
	ir_mode  *mode    = get_irn_mode(node);
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	dbg_info *dbgi    = get_irn_dbg_info(node);

	(void) block;
	(void) dbgi;

	if (mode_is_float(mode))
		panic("FP not implemented yet");

	return gen_helper_binop(node, MATCH_COMMUTATIVE, new_bd_sparc_Xor_reg, new_bd_sparc_Xor_imm);
}

static ir_node *gen_Shl(ir_node *node)
{
	return gen_helper_binop(node, MATCH_SIZE_NEUTRAL, new_bd_sparc_Sll_reg, new_bd_sparc_Sll_imm);
}

static ir_node *gen_Shr(ir_node *node)
{
	return gen_helper_binop(node, MATCH_SIZE_NEUTRAL, new_bd_sparc_Slr_reg, new_bd_sparc_Slr_imm);
}

static ir_node *gen_Shra(ir_node *node)
{
	return gen_helper_binop(node, MATCH_SIZE_NEUTRAL, new_bd_sparc_Sra_reg, new_bd_sparc_Sra_imm);
}

/****** TRANSFORM GENERAL BACKEND NODES ********/

/**
 * Transforms a Minus node.
 *
 */
static ir_node *gen_Minus(ir_node *node)
{
	ir_node  *block   = be_transform_node(get_nodes_block(node));
	ir_node  *op      = get_Minus_op(node);
	ir_node  *new_op  = be_transform_node(op);
	dbg_info *dbgi    = get_irn_dbg_info(node);
	ir_mode  *mode    = get_irn_mode(node);

	if (mode_is_float(mode)) {
		panic("FP not implemented yet");
	}

	assert(mode_is_data(mode));
	return new_bd_sparc_Minus(dbgi, block, new_op);
}

/**
 * Transforms a Const node.
 *
 * @param node    the ir Const node
 * @return The transformed sparc node.
 */
static ir_node *gen_Const(ir_node *node)
{
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_mode *mode = get_irn_mode(node);
	dbg_info *dbg = get_irn_dbg_info(node);

	(void) dbg;

	if (mode_is_float(mode)) {
		return create_fp_const_graph(node, block);
	}

	return create_const_graph(node, block);
}

/**
 * AddSP
 * @param node the ir AddSP node
 * @return transformed sparc SAVE node
 */
static ir_node *gen_be_AddSP(ir_node *node)
{
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_AddSP_size);
	ir_node  *new_sz = be_transform_node(sz);
	ir_node  *sp     = get_irn_n(node, be_pos_AddSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	/* SPARC stack grows in reverse direction */
	new_op = new_bd_sparc_SubSP(dbgi, block, new_sp, new_sz, nomem);

	return new_op;
}


/**
 * SubSP
 * @param node the ir SubSP node
 * @return transformed sparc SAVE node
 */
static ir_node *gen_be_SubSP(ir_node *node)
{
	ir_node  *block  = be_transform_node(get_nodes_block(node));
	ir_node  *sz     = get_irn_n(node, be_pos_SubSP_size);
	ir_node  *new_sz = be_transform_node(sz);
	ir_node  *sp     = get_irn_n(node, be_pos_SubSP_old_sp);
	ir_node  *new_sp = be_transform_node(sp);
	dbg_info *dbgi   = get_irn_dbg_info(node);
	ir_node  *nomem  = new_NoMem();
	ir_node  *new_op;

	/* SPARC stack grows in reverse direction */
	new_op = new_bd_sparc_AddSP(dbgi, block, new_sp, new_sz, nomem);
	return new_op;
}

/**
 * transform FrameAddr
 */
static ir_node *gen_be_FrameAddr(ir_node *node)
{
	ir_node   *block  = be_transform_node(get_nodes_block(node));
	ir_entity *ent    = be_get_frame_entity(node);
	ir_node   *fp     = be_get_FrameAddr_frame(node);
	ir_node   *new_fp = be_transform_node(fp);
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	ir_node   *new_node;
	new_node = new_bd_sparc_FrameAddr(dbgi, block, new_fp, ent);
	return new_node;
}

/**
 * Transform a be_Copy.
 */
static ir_node *gen_be_Copy(ir_node *node)
{
	ir_node *result = be_duplicate_node(node);
	ir_mode *mode   = get_irn_mode(result);

	if (mode_needs_gp_reg(mode)) {
		set_irn_mode(node, mode_Iu);
	}

	return result;
}

/**
 * Transform a Call
 */
static ir_node *gen_be_Call(ir_node *node)
{
	ir_node *res = be_duplicate_node(node);
	arch_irn_add_flags(res, arch_irn_flags_modify_flags);
	return res;
}

/**
 * Transforms a Switch.
 *
 */
static ir_node *gen_SwitchJmp(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *selector = get_Cond_selector(node);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node *new_op = be_transform_node(selector);
	ir_node *const_graph;
	ir_node *sub;

	ir_node *proj;
	const ir_edge_t *edge;
	int min = INT_MAX;
	int max = INT_MIN;
	int translation;
	int pn;
	int n_projs;

	foreach_out_edge(node, edge) {
		proj = get_edge_src_irn(edge);
		assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

		pn = get_Proj_proj(proj);

		min = pn<min ? pn : min;
		max = pn>max ? pn : max;
	}

	translation = min;
	n_projs = max - translation + 1;

	foreach_out_edge(node, edge) {
		proj = get_edge_src_irn(edge);
		assert(is_Proj(proj) && "Only proj allowed at SwitchJmp");

		pn = get_Proj_proj(proj) - translation;
		set_Proj_proj(proj, pn);
	}

	const_graph = create_const_graph_value(dbgi, block, translation);
	sub = new_bd_sparc_Sub_reg(dbgi, block, new_op, const_graph);
	return new_bd_sparc_SwitchJmp(dbgi, block, sub, n_projs, get_Cond_default_proj(node) - translation);
}

/**
 * Transform Cond nodes
 */
static ir_node *gen_Cond(ir_node *node)
{
	ir_node  *selector = get_Cond_selector(node);
	ir_mode  *mode     = get_irn_mode(selector);
	ir_node  *block;
	ir_node  *flag_node;
	dbg_info *dbgi;

	// switch/case jumps
	if (mode != mode_b) {
		return gen_SwitchJmp(node);
	}

	// regular if/else jumps
	assert(is_Proj(selector));

	block     = be_transform_node(get_nodes_block(node));
	dbgi      = get_irn_dbg_info(node);
	flag_node = be_transform_node(get_Proj_pred(selector));
	return new_bd_sparc_BXX(dbgi, block, flag_node, get_Proj_proj(selector));
}

/**
 * transform Cmp
 */
static ir_node *gen_Cmp(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *op1      = get_Cmp_left(node);
	ir_node  *op2      = get_Cmp_right(node);
	ir_mode  *cmp_mode = get_irn_mode(op1);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *new_op1;
	ir_node  *new_op2;
	bool      is_unsigned;

	if (mode_is_float(cmp_mode)) {
		panic("FloatCmp not implemented");
	}

	/*
	if (get_mode_size_bits(cmp_mode) != 32) {
		panic("CmpMode != 32bit not supported yet");
	}
	*/

	assert(get_irn_mode(op2) == cmp_mode);
	is_unsigned = !mode_is_signed(cmp_mode);

	/* compare with 0 can be done with Tst */
	/*
	if (is_Const(op2) && tarval_is_null(get_Const_tarval(op2))) {
		new_op1 = be_transform_node(op1);
		return new_bd_sparc_Tst(dbgi, block, new_op1, false,
		                          is_unsigned);
	}

	if (is_Const(op1) && tarval_is_null(get_Const_tarval(op1))) {
		new_op2 = be_transform_node(op2);
		return new_bd_sparc_Tst(dbgi, block, new_op2, true,
		                          is_unsigned);
	}
	*/

	/* integer compare */
	new_op1 = be_transform_node(op1);
	new_op1 = gen_extension(dbgi, block, new_op1, cmp_mode);
	new_op2 = be_transform_node(op2);
	new_op2 = gen_extension(dbgi, block, new_op2, cmp_mode);
	return new_bd_sparc_Cmp_reg(dbgi, block, new_op1, new_op2, false, is_unsigned);
}

/**
 * Transforms a SymConst node.
 */
static ir_node *gen_SymConst(ir_node *node)
{
	ir_node   *block  = be_transform_node(get_nodes_block(node));
	ir_entity *entity = get_SymConst_entity(node);
	dbg_info  *dbgi   = get_irn_dbg_info(node);
	ir_node   *new_node;

	new_node = new_bd_sparc_SymConst(dbgi, block, entity);
	be_dep_on_frame(new_node);
	return new_node;
}

/**
 * Transforms a Conv node.
 *
 */
static ir_node *gen_Conv(ir_node *node)
{
	ir_node  *block    = be_transform_node(get_nodes_block(node));
	ir_node  *op       = get_Conv_op(node);
	ir_node  *new_op   = be_transform_node(op);
	ir_mode  *src_mode = get_irn_mode(op);
	ir_mode  *dst_mode = get_irn_mode(node);
	dbg_info *dbg      = get_irn_dbg_info(node);

	int src_bits = get_mode_size_bits(src_mode);
	int dst_bits = get_mode_size_bits(dst_mode);

	if (src_mode == dst_mode)
		return new_op;

	if (mode_is_float(src_mode) || mode_is_float(dst_mode)) {
		assert((src_bits <= 64 && dst_bits <= 64) && "quad FP not implemented");

		if (mode_is_float(src_mode)) {
			if (mode_is_float(dst_mode)) {
				// float -> float conv
				if (src_bits > dst_bits) {
					return new_bd_sparc_FsTOd(dbg, block, new_op, dst_mode);
				} else {
					return new_bd_sparc_FdTOs(dbg, block, new_op, dst_mode);
				}
			} else {
				// float -> int conv
				switch (dst_bits) {
					case 32:
						return new_bd_sparc_FsTOi(dbg, block, new_op, dst_mode);
					case 64:
						return new_bd_sparc_FdTOi(dbg, block, new_op, dst_mode);
					default:
						panic("quad FP not implemented");
				}
			}
		} else {
			// int -> float conv
			switch (dst_bits) {
				case 32:
					return new_bd_sparc_FiTOs(dbg, block, new_op, src_mode);
				case 64:
					return new_bd_sparc_FiTOd(dbg, block, new_op, src_mode);
				default:
					panic("quad FP not implemented");
			}
		}
	} else { /* complete in gp registers */
		int min_bits;
		ir_mode *min_mode;

		if (src_bits == dst_bits) {
			/* kill unneccessary conv */
			return new_op;
		}

		if (src_bits < dst_bits) {
			min_bits = src_bits;
			min_mode = src_mode;
		} else {
			min_bits = dst_bits;
			min_mode = dst_mode;
		}

		if (upper_bits_clean(new_op, min_mode)) {
			return new_op;
		}

		if (mode_is_signed(min_mode)) {
			return gen_sign_extension(dbg, block, new_op, min_bits);
		} else {
			return gen_zero_extension(dbg, block, new_op, min_bits);
		}
	}
}

static ir_node *gen_Unknown(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	/* just produce a 0 */
	ir_mode *mode = get_irn_mode(node);
	if (mode_is_float(mode)) {
		panic("FP not implemented");
		be_dep_on_frame(node);
		return node;
	} else if (mode_needs_gp_reg(mode)) {
		return create_const_graph_value(dbgi, new_block, 0);
	}

	panic("Unexpected Unknown mode");
}

/**
 * Transform some Phi nodes
 */
static ir_node *gen_Phi(ir_node *node)
{
	const arch_register_req_t *req;
	ir_node  *block = be_transform_node(get_nodes_block(node));
	ir_graph *irg   = current_ir_graph;
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_mode  *mode  = get_irn_mode(node);
	ir_node  *phi;

	if (mode_needs_gp_reg(mode)) {
		/* we shouldn't have any 64bit stuff around anymore */
		assert(get_mode_size_bits(mode) <= 32);
		/* all integer operations are on 32bit registers now */
		mode = mode_Iu;
		req  = sparc_reg_classes[CLASS_sparc_gp].class_req;
	} else {
		req = arch_no_register_req;
	}

	/* phi nodes allow loops, so we use the old arguments for now
	 * and fix this later */
	phi = new_ir_node(dbgi, irg, block, op_Phi, mode, get_irn_arity(node), get_irn_in(node) + 1);
	copy_node_attr(irg, node, phi);
	be_duplicate_deps(node, phi);
	arch_set_out_register_req(phi, 0, req);
	be_enqueue_preds(node);
	return phi;
}


/**
 * Transform a Proj from a Load.
 */
static ir_node *gen_Proj_Load(ir_node *node)
{
	ir_node  *load     = get_Proj_pred(node);
	ir_node  *new_load = be_transform_node(load);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	/* renumber the proj */
	switch (get_sparc_irn_opcode(new_load)) {
		case iro_sparc_Ld:
			/* handle all gp loads equal: they have the same proj numbers. */
			if (proj == pn_Load_res) {
				return new_rd_Proj(dbgi, new_load, mode_Iu, pn_sparc_Ld_res);
			} else if (proj == pn_Load_M) {
				return new_rd_Proj(dbgi, new_load, mode_M, pn_sparc_Ld_M);
			}
			break;
		default:
			panic("Unsupported Proj from Load");
	}

	return be_duplicate_node(node);
}

/**
 * Transform the Projs of a be_AddSP.
 */
static ir_node *gen_Proj_be_AddSP(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_AddSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, new_pred, mode_Iu,
		                           pn_sparc_SubSP_stack);
		arch_set_irn_register(res, &sparc_gp_regs[REG_SP]);
		return res;
	} else if (proj == pn_be_AddSP_res) {
		return new_rd_Proj(dbgi, new_pred, mode_Iu, pn_sparc_SubSP_stack);
	} else if (proj == pn_be_AddSP_M) {
		return new_rd_Proj(dbgi, new_pred, mode_M, pn_sparc_SubSP_M);
	}

	panic("Unsupported Proj from AddSP");
}

/**
 * Transform the Projs of a be_SubSP.
 */
static ir_node *gen_Proj_be_SubSP(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	long     proj      = get_Proj_proj(node);

	if (proj == pn_be_SubSP_sp) {
		ir_node *res = new_rd_Proj(dbgi, new_pred, mode_Iu,
		                           pn_sparc_AddSP_stack);
		arch_set_irn_register(res, &sparc_gp_regs[REG_SP]);
		return res;
	} else if (proj == pn_be_SubSP_M) {
		return new_rd_Proj(dbgi,  new_pred, mode_M, pn_sparc_AddSP_M);
	}

	panic("Unsupported Proj from SubSP");
}

/**
 * Transform the Projs from a Cmp.
 */
static ir_node *gen_Proj_Cmp(ir_node *node)
{
	(void) node;
	panic("not implemented");
}

/**
 * transform Projs from a Div
 */
static ir_node *gen_Proj_Div(ir_node *node)
{
	ir_node  *pred     = get_Proj_pred(node);
	ir_node  *new_pred = be_transform_node(pred);
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_mode  *mode     = get_irn_mode(node);
	long     proj      = get_Proj_proj(node);

	switch (proj) {
		case pn_Div_res:
			if (is_sparc_Div(new_pred)) {
				return new_rd_Proj(dbgi, new_pred, mode, pn_sparc_Div_res);
			}
		break;
	default:
		break;
	}
	panic("Unsupported Proj from Div");
}


/**
 * Transform a Proj node.
 */
static ir_node *gen_Proj(ir_node *node)
{
	ir_graph *irg  = current_ir_graph;
	dbg_info *dbgi = get_irn_dbg_info(node);
	ir_node  *pred = get_Proj_pred(node);
	long     proj  = get_Proj_proj(node);

	(void) irg;
	(void) dbgi;

	if (is_Store(pred)) {
		if (proj == pn_Store_M) {
			return be_transform_node(pred);
		} else {
			panic("Unsupported Proj from Store");
		}
	} else if (is_Load(pred)) {
		return gen_Proj_Load(node);
	} else if (be_is_SubSP(pred)) {
		//panic("gen_Proj not implemented for SubSP");
		return gen_Proj_be_SubSP(node);
	} else if (be_is_AddSP(pred)) {
		//panic("gen_Proj not implemented for AddSP");
		return gen_Proj_be_AddSP(node);
	} else if (is_Cmp(pred)) {
		//panic("gen_Proj not implemented for Cmp");
		return gen_Proj_Cmp(node);
	} else if (is_Div(pred)) {
		return gen_Proj_Div(node);
	} else if (is_Start(pred)) {
	/*
		if (proj == pn_Start_X_initial_exec) {
			ir_node *block = get_nodes_block(pred);
			ir_node *jump;

			// we exchange the ProjX with a jump
			block = be_transform_node(block);
			jump  = new_rd_Jmp(dbgi, block);
			return jump;
		}

		if (node == get_irg_anchor(irg, anchor_tls)) {
			return gen_Proj_tls(node);
		}
	*/
	} else {
		ir_node *new_pred = be_transform_node(pred);
		ir_mode *mode     = get_irn_mode(node);
		if (mode_needs_gp_reg(mode)) {
			ir_node *new_proj = new_r_Proj(new_pred, mode_Iu, get_Proj_proj(node));
			new_proj->node_nr = node->node_nr;
			return new_proj;
		}
	}

	return be_duplicate_node(node);
}


/**
 * transform a Jmp
 */
static ir_node *gen_Jmp(ir_node *node)
{
	ir_node  *block     = get_nodes_block(node);
	ir_node  *new_block = be_transform_node(block);
	dbg_info *dbgi      = get_irn_dbg_info(node);

	return new_bd_sparc_Ba(dbgi, new_block);
}

/**
 * configure transformation callbacks
 */
void sparc_register_transformers(void)
{
	be_start_transform_setup();

	be_set_transform_function(op_Abs,          gen_Abs);
	be_set_transform_function(op_Add,          gen_Add);
	be_set_transform_function(op_And,          gen_And);
	be_set_transform_function(op_be_AddSP,     gen_be_AddSP);
	be_set_transform_function(op_be_Call,      gen_be_Call);
	be_set_transform_function(op_be_Copy,      gen_be_Copy);
	be_set_transform_function(op_be_FrameAddr, gen_be_FrameAddr);
	be_set_transform_function(op_be_SubSP,     gen_be_SubSP);
	be_set_transform_function(op_Cmp,          gen_Cmp);
	be_set_transform_function(op_Cond,         gen_Cond);
	be_set_transform_function(op_Const,        gen_Const);
	be_set_transform_function(op_Conv,         gen_Conv);
	be_set_transform_function(op_Div,          gen_Div);
	be_set_transform_function(op_Eor,          gen_Xor);
	be_set_transform_function(op_Jmp,          gen_Jmp);
	be_set_transform_function(op_Load,         gen_Load);
	be_set_transform_function(op_Minus,        gen_Minus);
	be_set_transform_function(op_Mul,          gen_Mul);
	be_set_transform_function(op_Mulh,         gen_Mulh);
	be_set_transform_function(op_Not,          gen_Not);
	be_set_transform_function(op_Or,           gen_Or);
	be_set_transform_function(op_Phi,          gen_Phi);
	be_set_transform_function(op_Proj,         gen_Proj);
	be_set_transform_function(op_Shl,          gen_Shl);
	be_set_transform_function(op_Shr,          gen_Shr);
	be_set_transform_function(op_Shrs,         gen_Shra);
	be_set_transform_function(op_Store,        gen_Store);
	be_set_transform_function(op_Sub,          gen_Sub);
	be_set_transform_function(op_SymConst,     gen_SymConst);
	be_set_transform_function(op_Unknown,      gen_Unknown);

	be_set_transform_function(op_sparc_Save,   be_duplicate_node);
}

/**
 * Transform a Firm graph into a SPARC graph.
 */
void sparc_transform_graph(sparc_code_gen_t *cg)
{
	sparc_register_transformers();
	env_cg = cg;
	be_transform_graph(cg->irg, NULL);
}

void sparc_init_transform(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.sparc.transform");
}
