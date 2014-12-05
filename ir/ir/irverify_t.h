/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    New checker of irnodes for correctness.
 * @author   Michael Beck
 */
#ifndef FIRM_IR_IRVERIFY_T_H
#define FIRM_IR_IRVERIFY_T_H

#include "irverify.h"

/**
 * Set the default verify_node and verify_proj_node operations.
 */
void ir_register_verify_node_ops(void);

/**
 * Print a verification warning about a specific node. The format string
 * is in ir_printf format.
 */
void verify_warn(const ir_node *n, const char *format, ...);

#endif
