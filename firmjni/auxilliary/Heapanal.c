/* -*- c -*- */

# include "Heapanal.h"

# include "firm.h"
# include "irsimpletype.h"
# include "heapanal/heapanal.h"

/*  boilerplate stuff: */
#include "irvrfy.h"
#include "trvrfy.h"
#include "irdump.h"


/*  I/O: */
# include <stdio.h>

/*
 * Class:     firmjni_Heapanal
 * Method:    initAnal
 * Signature: ()V
 */
void Java_firmjni_Heapanal_initAnal (JNIEnv *env, jclass clazz)
{
  /*  from interprete.c:  */
  init_interprete ();
}

/*
 * Class:     firmjni_Heapanal
 * Method:    deInitAnal
 * Signature: ()V
 */
void Java_firmjni_Heapanal_deInitAnal (JNIEnv *env, jclass clazz)
{
  free_interprete ();
}

/*
 * Class:     firmjni_Heapanal
 * Method:    analHeap
 * Signature: (I)V
 */
void Java_firmjni_Heapanal_analHeap__I (JNIEnv *env, jclass clazz, jint fMethod)
{
  ha_analyse_heap((entity *) fMethod, 0);
}

/*
 * Class:     firmjni_Heapanal
 * Method:    analHeap
 * Signature: ()V
 */
void Java_firmjni_Heapanal_analHeap__ (JNIEnv *env, jclass clazz)
{
  int n_graphs;
  int i;

  fprintf (stdout, "Hello, Heap!\n");

  fprintf (stdout, "Ajacs Boilerplate:\n");
  {
    entity **free_methods = 0;
    int arr_len = 0;

    /* replace static constant fields by constants
       @@@ This corrects some code that is invalid Firm!!!
       Execute before irg_vrfy(). */
    /* ??? */
    /*  opt_load_const_static(); */

    /* dump graphs as they come out of the front end */
    char *suffix = "-fe";
    dump_all_types (suffix);
    dump_class_hierarchy (true, suffix);
    dump_all_ir_graphs(dump_ir_block_graph, suffix);
    dump_all_ir_graphs(dump_ir_block_graph_w_types, suffix);
    dump_all_ir_graphs(dump_cfg, suffix);

    /* verify constructed graphs */
    for (i = 0; i < get_irp_n_irgs(); i++)
      irg_vrfy(get_irp_irg(i));

    /* verify something */
    tr_vrfy();


    /*
     * test loop construction intraprocedural
     */
    for (i = 0; i < get_irp_n_irgs(); i++) {
      construct_backedges(get_irp_irg(i));

      if (1) {
        dump_loop_information(1);
        dump_ir_block_graph(get_irp_irg(i), "-1.2-intra-loop");
        dump_loop_information(0);
        dump_loop_tree(get_irp_irg(i), "-1.2-intra");
      }
    }

    DDMG (get_irp_main_irg ());
    assert(get_irp_main_irg());
    assert(get_irg_entity(get_irp_main_irg()));


    /** Do interprocedural optimizations **/
    /* Analysis that builds the call graph and finds the free methods,
       i.e. methods that are dereferenced.
       Optimizes polymorphic calls.*/
    cgana(&arr_len, &free_methods);
    /* Remove methods that are never called. */
    /*  gc_irgs(arr_len, free_methods); */
    /* Build the interprocedural dataflow representation */
    cg_construct(arr_len, free_methods);

    /* Test construction of interprocedural loop information */
    /*  construct_ip_backedges(); */

    dump_loop_information(1);
    dump_all_cg_block_graph("-1.2-inter-loop");
    dump_loop_information(0);
    dump_loop_tree(get_irp_main_irg(), "-1.2-inter");


    fprintf (stdout, "HA:\n");
    DDMG (get_irp_main_irg ());

    set_max_chi_height(8);  /* change ad lib */
    set_initial_context_depth(4);   /* change as needed */
    ha_analyse_heap(get_irg_entity(get_irp_main_irg()), 1);

    /* Remove the interprocedural dataflow representation */
    free(free_methods);
    cg_destruct();

    /* verify optimized graphs */
    for (i = 0; i < get_irp_n_irgs(); i++) {
      irg_vrfy(get_irp_irg(i));
    }

    tr_vrfy();

  }

  set_opt_dump_abstvals (1);

  /* ToDo:  Dump ??? */

  fprintf (stdout, "Bye,   Heap!\n");
}



/*
 * $Log$
 * Revision 1.6  2005/03/01 16:22:39  goetz
 * changed irdump flag
 *
 * Revision 1.5  2004/11/15 12:38:16  goetz
 * we need more and more libs at linking ...
 * changed node numbers (unknown_entity)
 *
 * Revision 1.4  2004/08/14 10:09:52  goetz
 * adapted to new semantics
 *
 * Revision 1.3  2004/04/30 09:00:01  goetz
 * added configure support for heap analyses
 *
 * Revision 1.2  2004/04/29 13:59:11  liekweg
 * Removed C++-Style comments --flo
 *
 * Revision 1.1  2004/04/29 12:11:36  liekweg
 * Moved ... to aux --flo
 *
 * Revision 1.1  2004/04/27 12:41:31  liekweg
 * Fixed ir/ana/irouts.c ir/ir/ircons.h ir/ir/irdump.c --flo
 * Added aux/Heapanal.c aux/Heapanal.java --flo
 *
 */
