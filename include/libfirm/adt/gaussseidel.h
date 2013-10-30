#ifndef MATRIX_H_
#define MATRIX_H_

#include <stdio.h>

#include "../begin.h"

typedef struct gs_matrix_t gs_matrix_t;

/**
 * Allocate a new matrix and init internal data for a matrix of size
 * row_init X col_init. Matrix cannot grow beyond these init values.
 * All elements are initially (implicitly) set to 0.
 */
gs_matrix_t *gs_new_matrix(int n_init_rows, int n_init_cols);

/**
 * Free space used by matrix m
 */
void gs_delete_matrix(gs_matrix_t *m);

/**
 * Sets m[row, col] to val
 */
void gs_matrix_set(gs_matrix_t *m, unsigned row, unsigned col, double val);

/**
 * Returns the value stored in m[row, col].
 */
double gs_matrix_get(const gs_matrix_t *m, unsigned row, unsigned col);

/**
 * Performs one step of the Gauss-Seidel algorithm
 * @p m         The iteration matrix
 * @p x         The iteration vector
 */
double gs_matrix_gauss_seidel(const gs_matrix_t *m, double *x);

unsigned gs_matrix_get_n_entries(const gs_matrix_t *m);

/**
 * Dumps the matrix factor*m to the stream @p out.
 */
void gs_matrix_dump(const gs_matrix_t *m, FILE *out);

#include "../end.h"

#endif
