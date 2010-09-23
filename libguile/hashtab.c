/* Copyright (C) 1995,1996,1998,1999,2000,2001, 2003, 2004, 2006, 2008, 2009, 2010 Free Software Foundation, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */



#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <assert.h>

#include "libguile/_scm.h"
#include "libguile/alist.h"
#include "libguile/hash.h"
#include "libguile/eval.h"
#include "libguile/root.h"
#include "libguile/vectors.h"
#include "libguile/ports.h"

#include "libguile/validate.h"
#include "libguile/hashtab.h"




/* NOTES
 *
 * 1. The current hash table implementation uses weak alist vectors
 *    (implementation in weaks.c) internally, but we do the scanning
 *    ourselves (in scan_weak_hashtables) because we need to update the
 *    hash table structure when items are dropped during GC.
 *
 * 2. All hash table operations still work on alist vectors.
 *
 */

/* A hash table is a cell containing a vector of association lists.
 *
 * Growing or shrinking, with following rehashing, is triggered when
 * the load factor
 *
 *   L = N / S    (N: number of items in table, S: bucket vector length)
 *
 * passes an upper limit of 0.9 or a lower limit of 0.25.
 *
 * The implementation stores the upper and lower number of items which
 * trigger a resize in the hashtable object.
 *
 * Possible hash table sizes (primes) are stored in the array
 * hashtable_size.
 */

static unsigned long hashtable_size[] = {
  31, 61, 113, 223, 443, 883, 1759, 3517, 7027, 14051, 28099, 56197, 112363,
  224717, 449419, 898823, 1797641, 3595271, 7190537, 14381041
#if 0
  /* vectors are currently restricted to 2^24-1 = 16777215 elements. */
  28762081, 57524111, 115048217, 230096423, 460192829
  /* larger values can't be represented as INUMs */
#endif
};

#define HASHTABLE_SIZE_N (sizeof(hashtable_size)/sizeof(unsigned long))

static char *s_hashtable = "hashtable";



/* Helper functions and macros to deal with weak pairs.

   Weak pairs need to be accessed very carefully since their components can
   be nullified by the GC when the object they refer to becomes unreachable.
   Hence the macros and functions below that detect such weak pairs within
   buckets and remove them.  */


/* Remove nullified weak pairs from ALIST such that the result contains only
   valid pairs.  Set REMOVED_ITEMS to the number of pairs that have been
   deleted.  */
static SCM
scm_fixup_weak_alist (SCM alist, size_t *removed_items)
{
  SCM result;
  SCM prev = SCM_EOL;

  *removed_items = 0;
  for (result = alist;
       scm_is_pair (alist);
       prev = alist, alist = SCM_CDR (alist))
    {
      SCM pair = SCM_CAR (alist);

      if (scm_is_pair (pair))
	{
	  if (SCM_WEAK_PAIR_DELETED_P (pair))
	    {
	      /* Remove from ALIST weak pair PAIR whose car/cdr has been
		 nullified by the GC.  */
	      if (prev == SCM_EOL)
		result = SCM_CDR (alist);
	      else
		SCM_SETCDR (prev, SCM_CDR (alist));

	      (*removed_items)++;
	      continue;
	    }
	}
    }

  return result;
}


/* Return true if OBJ is either a weak hash table or a weak alist vector (as
   defined in `weaks.[ch]').
   FIXME: We should eventually keep only weah hash tables.  Actually, the
   procs in `weaks.c' already no longer return vectors.  */
/* XXX: We assume that if OBJ is a vector, then it's a _weak_ alist vector.  */
#define IS_WEAK_THING(_obj)					\
  ((SCM_HASHTABLE_P (table) && (SCM_HASHTABLE_WEAK_P (table)))	\
   || (SCM_I_IS_VECTOR (table)))


/* Packed arguments for `do_weak_bucket_assoc ()'.  */
struct t_assoc_args
{
  /* Input arguments.  */
  SCM object;
  SCM buckets;
  size_t bucket_index;
  scm_t_assoc_fn assoc_fn;
  void *closure;

  /* Output arguments.  */
  SCM result;
  size_t removed_items;
};

static void *
do_weak_bucket_assoc (void *data)
{
  struct t_assoc_args *args;
  size_t removed;
  SCM bucket, result;

  args = (struct t_assoc_args *) data;

  bucket = SCM_SIMPLE_VECTOR_REF (args->buckets, args->bucket_index);
  bucket = scm_fixup_weak_alist (bucket, &removed);

  SCM_SIMPLE_VECTOR_SET (args->buckets, args->bucket_index, bucket);

  /* Run ASSOC_FN on the now clean BUCKET.  */
  result = args->assoc_fn (args->object, bucket, args->closure);

  args->result = result;
  args->removed_items = removed;

  return args;
}

/* Lookup OBJECT in weak hash table TABLE using ASSOC.  OBJECT is searched
   for in the alist that is the BUCKET_INDEXth element of BUCKETS.
   Optionally update TABLE and rehash it.  */
static SCM
weak_bucket_assoc (SCM table, SCM buckets, size_t bucket_index,
		   scm_t_hash_fn hash_fn,
		   scm_t_assoc_fn assoc, SCM object, void *closure)
{
  SCM result;
  struct t_assoc_args args;

  args.object = object;
  args.buckets = buckets;
  args.bucket_index = bucket_index;
  args.assoc_fn = assoc;
  args.closure = closure;

  /* Fixup the bucket and pass the clean bucket to ASSOC.  Do that with the
     allocation lock held to avoid seeing disappearing links pointing to
     objects that have already been reclaimed (this happens when the
     disappearing links that point to it haven't yet been cleared.)
     Thus, ASSOC must not take long, and it must not make any non-local
     exit.  */
  GC_call_with_alloc_lock (do_weak_bucket_assoc, &args);

  result = args.result;
  assert (!scm_is_pair (result) ||
	  !SCM_WEAK_PAIR_DELETED_P (GC_is_visible (result)));

  if (args.removed_items > 0 && SCM_HASHTABLE_P (table))
    {
      /* Update TABLE's item count and optionally trigger a rehash.  */
      size_t remaining;

      assert (SCM_HASHTABLE_N_ITEMS (table) >= args.removed_items);

      remaining = SCM_HASHTABLE_N_ITEMS (table) - args.removed_items;
      SCM_SET_HASHTABLE_N_ITEMS (table, remaining);

      scm_i_rehash (table, hash_fn, closure, "weak_bucket_assoc");
    }

  return result;
}



static SCM
make_hash_table (int flags, unsigned long k, const char *func_name) 
{
  SCM vector;
  scm_t_hashtable *t;
  int i = 0, n = k ? k : 31;
  while (i < HASHTABLE_SIZE_N && n > hashtable_size[i])
    ++i;
  n = hashtable_size[i];

  /* In both cases, i.e., regardless of whether we are creating a weak hash
     table, we return a non-weak vector.  This is because the vector itself
     is not weak in the case of a weak hash table: the alist pairs are.  */
  vector = scm_c_make_vector (n, SCM_EOL);

  t = scm_gc_malloc_pointerless (sizeof (*t), s_hashtable);
  t->min_size_index = t->size_index = i;
  t->n_items = 0;
  t->lower = 0;
  t->upper = 9 * n / 10;
  t->flags = flags;
  t->hash_fn = NULL;

  /* FIXME: we just need two words of storage, not three */
  return scm_double_cell (scm_tc7_hashtable, SCM_UNPACK (vector),
                          (scm_t_bits)t, 0);
}

void
scm_i_rehash (SCM table,
	      scm_t_hash_fn hash_fn,
	      void *closure,
	      const char* func_name)
{
  SCM buckets, new_buckets;
  int i;
  unsigned long old_size;
  unsigned long new_size;

  if (SCM_HASHTABLE_N_ITEMS (table) < SCM_HASHTABLE_LOWER (table))
    {
      /* rehashing is not triggered when i <= min_size */
      i = SCM_HASHTABLE (table)->size_index;
      do
	--i;
      while (i > SCM_HASHTABLE (table)->min_size_index
	     && SCM_HASHTABLE_N_ITEMS (table) < hashtable_size[i] / 4);
    }
  else
    {
      i = SCM_HASHTABLE (table)->size_index + 1;
      if (i >= HASHTABLE_SIZE_N)
	/* don't rehash */
	return;

      /* Remember HASH_FN for rehash_after_gc, but only when CLOSURE
	 is not needed since CLOSURE can not be guaranteed to be valid
	 after this function returns.
      */
      if (closure == NULL)
	SCM_HASHTABLE (table)->hash_fn = hash_fn;
    }
  SCM_HASHTABLE (table)->size_index = i;
  
  new_size = hashtable_size[i];
  if (i <= SCM_HASHTABLE (table)->min_size_index)
    SCM_HASHTABLE (table)->lower = 0;
  else
    SCM_HASHTABLE (table)->lower = new_size / 4;
  SCM_HASHTABLE (table)->upper = 9 * new_size / 10;
  buckets = SCM_HASHTABLE_VECTOR (table);

  new_buckets = scm_c_make_vector (new_size, SCM_EOL);

  /* When this is a weak hashtable, running the GC might change it.
     We need to cope with this while rehashing its elements.  We do
     this by first installing the new, empty bucket vector.  Then we
     remove the elements from the old bucket vector and insert them
     into the new one.
  */

  SCM_SET_HASHTABLE_VECTOR (table, new_buckets);
  SCM_SET_HASHTABLE_N_ITEMS (table, 0);

  old_size = SCM_SIMPLE_VECTOR_LENGTH (buckets);
  for (i = 0; i < old_size; ++i)
    {
      SCM ls, cell, handle;

      ls = SCM_SIMPLE_VECTOR_REF (buckets, i);
      SCM_SIMPLE_VECTOR_SET (buckets, i, SCM_EOL);

      while (scm_is_pair (ls))
	{
	  unsigned long h;

	  cell = ls;
	  handle = SCM_CAR (cell);
	  ls = SCM_CDR (ls);

	  if (SCM_WEAK_PAIR_DELETED_P (handle))
	    /* HANDLE is a nullified weak pair: skip it.  */
	    continue;

	  h = hash_fn (SCM_CAR (handle), new_size, closure);
	  if (h >= new_size)
	    scm_out_of_range (func_name, scm_from_ulong (h));
	  SCM_SETCDR (cell, SCM_SIMPLE_VECTOR_REF (new_buckets, h));
	  SCM_SIMPLE_VECTOR_SET (new_buckets, h, cell);
	  SCM_HASHTABLE_INCREMENT (table);
	}
    }
}


void
scm_i_hashtable_print (SCM exp, SCM port, scm_print_state *pstate)
{
  scm_puts ("#<", port);
  if (SCM_HASHTABLE_WEAK_KEY_P (exp))
    scm_puts ("weak-key-", port);
  else if (SCM_HASHTABLE_WEAK_VALUE_P (exp))
    scm_puts ("weak-value-", port);
  else if (SCM_HASHTABLE_DOUBLY_WEAK_P (exp))
    scm_puts ("doubly-weak-", port);
  scm_puts ("hash-table ", port);
  scm_uintprint (SCM_HASHTABLE_N_ITEMS (exp), 10, port);
  scm_putc ('/', port);
  scm_uintprint (SCM_SIMPLE_VECTOR_LENGTH (SCM_HASHTABLE_VECTOR (exp)),
		 10, port);
  scm_puts (">", port);
}


SCM
scm_c_make_hash_table (unsigned long k)
{
  return make_hash_table (0, k, "scm_c_make_hash_table");
}

SCM_DEFINE (scm_make_hash_table, "make-hash-table", 0, 1, 0,
	    (SCM n),
	    "Make a new abstract hash table object with minimum number of buckets @var{n}\n")
#define FUNC_NAME s_scm_make_hash_table
{
  if (SCM_UNBNDP (n))
    return make_hash_table (0, 0, FUNC_NAME);
  else
    return make_hash_table (0, scm_to_ulong (n), FUNC_NAME);
}
#undef FUNC_NAME

SCM_DEFINE (scm_make_weak_key_hash_table, "make-weak-key-hash-table", 0, 1, 0, 
	    (SCM n),
	    "@deffnx {Scheme Procedure} make-weak-value-hash-table size\n"
	    "@deffnx {Scheme Procedure} make-doubly-weak-hash-table size\n"
	    "Return a weak hash table with @var{size} buckets.\n"
	    "\n"
	    "You can modify weak hash tables in exactly the same way you\n"
	    "would modify regular hash tables. (@pxref{Hash Tables})")
#define FUNC_NAME s_scm_make_weak_key_hash_table
{
  if (SCM_UNBNDP (n))
    return make_hash_table (SCM_HASHTABLEF_WEAK_CAR, 0, FUNC_NAME);
  else
    return make_hash_table (SCM_HASHTABLEF_WEAK_CAR,
			    scm_to_ulong (n), FUNC_NAME);
}
#undef FUNC_NAME


SCM_DEFINE (scm_make_weak_value_hash_table, "make-weak-value-hash-table", 0, 1, 0, 
            (SCM n),
	    "Return a hash table with weak values with @var{size} buckets.\n"
	    "(@pxref{Hash Tables})")
#define FUNC_NAME s_scm_make_weak_value_hash_table
{
  if (SCM_UNBNDP (n))
    return make_hash_table (SCM_HASHTABLEF_WEAK_CDR, 0, FUNC_NAME);
  else
    {
      return make_hash_table (SCM_HASHTABLEF_WEAK_CDR,
			      scm_to_ulong (n), FUNC_NAME);
    }
}
#undef FUNC_NAME


SCM_DEFINE (scm_make_doubly_weak_hash_table, "make-doubly-weak-hash-table", 1, 0, 0, 
            (SCM n),
	    "Return a hash table with weak keys and values with @var{size}\n"
	    "buckets.  (@pxref{Hash Tables})")
#define FUNC_NAME s_scm_make_doubly_weak_hash_table
{
  if (SCM_UNBNDP (n))
    return make_hash_table (SCM_HASHTABLEF_WEAK_CAR | SCM_HASHTABLEF_WEAK_CDR,
			    0,
			    FUNC_NAME);
  else
    {
      return make_hash_table (SCM_HASHTABLEF_WEAK_CAR | SCM_HASHTABLEF_WEAK_CDR,
			      scm_to_ulong (n),
			      FUNC_NAME);
    }
}
#undef FUNC_NAME


SCM_DEFINE (scm_hash_table_p, "hash-table?", 1, 0, 0, 
            (SCM obj),
	    "Return @code{#t} if @var{obj} is an abstract hash table object.")
#define FUNC_NAME s_scm_hash_table_p
{
  return scm_from_bool (SCM_HASHTABLE_P (obj));
}
#undef FUNC_NAME


SCM_DEFINE (scm_weak_key_hash_table_p, "weak-key-hash-table?", 1, 0, 0, 
           (SCM obj),
	    "@deffnx {Scheme Procedure} weak-value-hash-table? obj\n"
	    "@deffnx {Scheme Procedure} doubly-weak-hash-table? obj\n"
	    "Return @code{#t} if @var{obj} is the specified weak hash\n"
	    "table. Note that a doubly weak hash table is neither a weak key\n"
	    "nor a weak value hash table.")
#define FUNC_NAME s_scm_weak_key_hash_table_p
{
  return scm_from_bool (SCM_HASHTABLE_P (obj) && SCM_HASHTABLE_WEAK_KEY_P (obj));
}
#undef FUNC_NAME


SCM_DEFINE (scm_weak_value_hash_table_p, "weak-value-hash-table?", 1, 0, 0, 
            (SCM obj),
	    "Return @code{#t} if @var{obj} is a weak value hash table.")
#define FUNC_NAME s_scm_weak_value_hash_table_p
{
  return scm_from_bool (SCM_HASHTABLE_P (obj) && SCM_HASHTABLE_WEAK_VALUE_P (obj));
}
#undef FUNC_NAME


SCM_DEFINE (scm_doubly_weak_hash_table_p, "doubly-weak-hash-table?", 1, 0, 0, 
            (SCM obj),
	    "Return @code{#t} if @var{obj} is a doubly weak hash table.")
#define FUNC_NAME s_scm_doubly_weak_hash_table_p
{
  return scm_from_bool (SCM_HASHTABLE_P (obj) && SCM_HASHTABLE_DOUBLY_WEAK_P (obj));
}
#undef FUNC_NAME


/* Accessing hash table entries.  */

SCM
scm_hash_fn_get_handle (SCM table, SCM obj,
			scm_t_hash_fn hash_fn, scm_t_assoc_fn assoc_fn,
			void * closure)
#define FUNC_NAME "scm_hash_fn_get_handle"
{
  unsigned long k;
  SCM buckets, h;

  if (SCM_HASHTABLE_P (table))
    buckets = SCM_HASHTABLE_VECTOR (table);
  else
    {
      SCM_VALIDATE_VECTOR (1, table);
      buckets = table;
    }

  if (SCM_SIMPLE_VECTOR_LENGTH (buckets) == 0)
    return SCM_BOOL_F;
  k = hash_fn (obj, SCM_SIMPLE_VECTOR_LENGTH (buckets), closure);
  if (k >= SCM_SIMPLE_VECTOR_LENGTH (buckets))
    scm_out_of_range ("hash_fn_get_handle", scm_from_ulong (k));

  if (IS_WEAK_THING (table))
    h = weak_bucket_assoc (table, buckets, k, hash_fn,
			   assoc_fn, obj, closure);
  else
    h = assoc_fn (obj, SCM_SIMPLE_VECTOR_REF (buckets, k), closure);

  return h;
}
#undef FUNC_NAME


SCM
scm_hash_fn_create_handle_x (SCM table, SCM obj, SCM init,
			     scm_t_hash_fn hash_fn, scm_t_assoc_fn assoc_fn,
                             void * closure)
#define FUNC_NAME "scm_hash_fn_create_handle_x"
{
  unsigned long k;
  SCM buckets, it;

  if (SCM_HASHTABLE_P (table))
    buckets = SCM_HASHTABLE_VECTOR (table);
  else
    {
      SCM_ASSERT (scm_is_simple_vector (table),
		  table, SCM_ARG1, "hash_fn_create_handle_x");
      buckets = table;
    }
  if (SCM_SIMPLE_VECTOR_LENGTH (buckets) == 0)
    SCM_MISC_ERROR ("void hashtable", SCM_EOL);

  k = hash_fn (obj, SCM_SIMPLE_VECTOR_LENGTH (buckets), closure);
  if (k >= SCM_SIMPLE_VECTOR_LENGTH (buckets))
    scm_out_of_range ("hash_fn_create_handle_x", scm_from_ulong (k));

  if (IS_WEAK_THING (table))
    it = weak_bucket_assoc (table, buckets, k, hash_fn,
			    assoc_fn, obj, closure);
  else
    it = assoc_fn (obj, SCM_SIMPLE_VECTOR_REF (buckets, k), closure);

  if (scm_is_pair (it))
    return it;
  else if (scm_is_true (it))
    scm_wrong_type_arg_msg (NULL, 0, it, "a pair");
  else
    {
      /* When this is a weak hashtable, running the GC can change it.
	 Thus, we must allocate the new cells first and can only then
	 access BUCKETS.  Also, we need to fetch the bucket vector
	 again since the hashtable might have been rehashed.  This
	 necessitates a new hash value as well.
      */
      SCM handle, new_bucket;

      if ((SCM_HASHTABLE_P (table)) && (SCM_HASHTABLE_WEAK_P (table)))
	{
	  /* FIXME: We don't support weak alist vectors.  */
	  /* Use a weak cell.  */
	  if (SCM_HASHTABLE_DOUBLY_WEAK_P (table))
	    handle = scm_doubly_weak_pair (obj, init);
	  else if (SCM_HASHTABLE_WEAK_KEY_P (table))
	    handle = scm_weak_car_pair (obj, init);
	  else
	    handle = scm_weak_cdr_pair (obj, init);
	}
      else
	/* Use a regular, non-weak cell.  */
	handle = scm_cons (obj, init);

      new_bucket = scm_cons (handle, SCM_EOL);

      if (!scm_is_eq (table, buckets)
	  && !scm_is_eq (SCM_HASHTABLE_VECTOR (table), buckets))
	{
	  buckets = SCM_HASHTABLE_VECTOR (table);
	  k = hash_fn (obj, SCM_SIMPLE_VECTOR_LENGTH (buckets), closure);
	  if (k >= SCM_SIMPLE_VECTOR_LENGTH (buckets))
	    scm_out_of_range ("hash_fn_create_handle_x", scm_from_ulong (k));
	}
      SCM_SETCDR (new_bucket, SCM_SIMPLE_VECTOR_REF (buckets, k));
      SCM_SIMPLE_VECTOR_SET (buckets, k, new_bucket);
      if (!scm_is_eq (table, buckets))
	{
	  /* Update element count and maybe rehash the table.  The
	     table might have too few entries here since weak hash
	     tables used with the hashx_* functions can not be
	     rehashed after GC.
	  */
	  SCM_HASHTABLE_INCREMENT (table);
	  if (SCM_HASHTABLE_N_ITEMS (table) < SCM_HASHTABLE_LOWER (table)
	      || SCM_HASHTABLE_N_ITEMS (table) > SCM_HASHTABLE_UPPER (table))
	    scm_i_rehash (table, hash_fn, closure, FUNC_NAME);
	}
      return SCM_CAR (new_bucket);
    }
}
#undef FUNC_NAME


SCM
scm_hash_fn_ref (SCM table, SCM obj, SCM dflt,
		 scm_t_hash_fn hash_fn, scm_t_assoc_fn assoc_fn,
                 void *closure)
{
  SCM it = scm_hash_fn_get_handle (table, obj, hash_fn, assoc_fn, closure);
  if (scm_is_pair (it))
    return SCM_CDR (it);
  else
    return dflt;
}




SCM
scm_hash_fn_set_x (SCM table, SCM obj, SCM val,
		   scm_t_hash_fn hash_fn, scm_t_assoc_fn assoc_fn,
                   void *closure)
{
  SCM it;

  it = scm_hash_fn_create_handle_x (table, obj, SCM_BOOL_F, hash_fn, assoc_fn, closure);
  SCM_SETCDR (it, val);

  if (SCM_HASHTABLE_P (table) && SCM_HASHTABLE_WEAK_VALUE_P (table)
      && SCM_NIMP (val))
    /* IT is a weak-cdr pair.  Register a disappearing link from IT's
       cdr to VAL like `scm_weak_cdr_pair' does.  */
    SCM_I_REGISTER_DISAPPEARING_LINK ((void *) SCM_CDRLOC (it), SCM2PTR (val));

  return val;
}


SCM
scm_hash_fn_remove_x (SCM table, SCM obj,
		      scm_t_hash_fn hash_fn,
		      scm_t_assoc_fn assoc_fn,
                      void *closure)
{
  unsigned long k;
  SCM buckets, h;

  if (SCM_HASHTABLE_P (table))
    buckets = SCM_HASHTABLE_VECTOR (table);
  else
    {
      SCM_ASSERT (scm_is_simple_vector (table), table,
		  SCM_ARG1, "hash_fn_remove_x");
      buckets = table;
    }
  if (SCM_SIMPLE_VECTOR_LENGTH (buckets) == 0)
    return SCM_EOL;

  k = hash_fn (obj, SCM_SIMPLE_VECTOR_LENGTH (buckets), closure);
  if (k >= SCM_SIMPLE_VECTOR_LENGTH (buckets))
    scm_out_of_range ("hash_fn_remove_x", scm_from_ulong (k));

  if (IS_WEAK_THING (table))
    h = weak_bucket_assoc (table, buckets, k, hash_fn,
			   assoc_fn, obj, closure);
  else
    h = assoc_fn (obj, SCM_SIMPLE_VECTOR_REF (buckets, k), closure);

  if (scm_is_true (h))
    {
      SCM_SIMPLE_VECTOR_SET 
	(buckets, k, scm_delq_x (h, SCM_SIMPLE_VECTOR_REF (buckets, k)));
      if (!scm_is_eq (table, buckets))
	{
	  SCM_HASHTABLE_DECREMENT (table);
	  if (SCM_HASHTABLE_N_ITEMS (table) < SCM_HASHTABLE_LOWER (table))
	    scm_i_rehash (table, hash_fn, closure, "scm_hash_fn_remove_x");
	}
    }
  return h;
}

SCM_DEFINE (scm_hash_clear_x, "hash-clear!", 1, 0, 0,
	    (SCM table),
	    "Remove all items from @var{table} (without triggering a resize).")
#define FUNC_NAME s_scm_hash_clear_x
{
  if (SCM_HASHTABLE_P (table))
    {
      scm_vector_fill_x (SCM_HASHTABLE_VECTOR (table), SCM_EOL);
      SCM_SET_HASHTABLE_N_ITEMS (table, 0);
    }
  else
    scm_vector_fill_x (table, SCM_EOL);
  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME



SCM_DEFINE (scm_hashq_get_handle, "hashq-get-handle", 2, 0, 0,
            (SCM table, SCM key),
	    "This procedure returns the @code{(key . value)} pair from the\n"
	    "hash table @var{table}.  If @var{table} does not hold an\n"
	    "associated value for @var{key}, @code{#f} is returned.\n"
	    "Uses @code{eq?} for equality testing.")
#define FUNC_NAME s_scm_hashq_get_handle
{
  return scm_hash_fn_get_handle (table, key,
				 (scm_t_hash_fn) scm_ihashq,
				 (scm_t_assoc_fn) scm_sloppy_assq,
				 0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashq_create_handle_x, "hashq-create-handle!", 3, 0, 0,
            (SCM table, SCM key, SCM init),
	    "This function looks up @var{key} in @var{table} and returns its handle.\n"
	    "If @var{key} is not already present, a new handle is created which\n"
	    "associates @var{key} with @var{init}.")
#define FUNC_NAME s_scm_hashq_create_handle_x
{
  return scm_hash_fn_create_handle_x (table, key, init,
				      (scm_t_hash_fn) scm_ihashq,
				      (scm_t_assoc_fn) scm_sloppy_assq,
				      0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashq_ref, "hashq-ref", 2, 1, 0,
            (SCM table, SCM key, SCM dflt),
	    "Look up @var{key} in the hash table @var{table}, and return the\n"
	    "value (if any) associated with it.  If @var{key} is not found,\n"
	    "return @var{default} (or @code{#f} if no @var{default} argument\n"
	    "is supplied).  Uses @code{eq?} for equality testing.")
#define FUNC_NAME s_scm_hashq_ref
{
  if (SCM_UNBNDP (dflt))
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, key, dflt,
			  (scm_t_hash_fn) scm_ihashq,
			  (scm_t_assoc_fn) scm_sloppy_assq,
			  0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hashq_set_x, "hashq-set!", 3, 0, 0,
            (SCM table, SCM key, SCM val),
	    "Find the entry in @var{table} associated with @var{key}, and\n"
	    "store @var{value} there. Uses @code{eq?} for equality testing.")
#define FUNC_NAME s_scm_hashq_set_x
{
  return scm_hash_fn_set_x (table, key, val,
			    (scm_t_hash_fn) scm_ihashq,
			    (scm_t_assoc_fn) scm_sloppy_assq,
			    0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hashq_remove_x, "hashq-remove!", 2, 0, 0,
            (SCM table, SCM key),
	    "Remove @var{key} (and any value associated with it) from\n"
	    "@var{table}.  Uses @code{eq?} for equality tests.")
#define FUNC_NAME s_scm_hashq_remove_x
{
  return scm_hash_fn_remove_x (table, key,
			       (scm_t_hash_fn) scm_ihashq,
			       (scm_t_assoc_fn) scm_sloppy_assq,
			       0);
}
#undef FUNC_NAME




SCM_DEFINE (scm_hashv_get_handle, "hashv-get-handle", 2, 0, 0,
            (SCM table, SCM key),
	    "This procedure returns the @code{(key . value)} pair from the\n"
	    "hash table @var{table}.  If @var{table} does not hold an\n"
	    "associated value for @var{key}, @code{#f} is returned.\n"
	    "Uses @code{eqv?} for equality testing.")
#define FUNC_NAME s_scm_hashv_get_handle
{
  return scm_hash_fn_get_handle (table, key,
				 (scm_t_hash_fn) scm_ihashv,
				 (scm_t_assoc_fn) scm_sloppy_assv,
				 0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashv_create_handle_x, "hashv-create-handle!", 3, 0, 0,
            (SCM table, SCM key, SCM init),
	    "This function looks up @var{key} in @var{table} and returns its handle.\n"
	    "If @var{key} is not already present, a new handle is created which\n"
	    "associates @var{key} with @var{init}.")
#define FUNC_NAME s_scm_hashv_create_handle_x
{
  return scm_hash_fn_create_handle_x (table, key, init,
				      (scm_t_hash_fn) scm_ihashv,
				      (scm_t_assoc_fn) scm_sloppy_assv,
				      0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashv_ref, "hashv-ref", 2, 1, 0,
            (SCM table, SCM key, SCM dflt),
	    "Look up @var{key} in the hash table @var{table}, and return the\n"
	    "value (if any) associated with it.  If @var{key} is not found,\n"
	    "return @var{default} (or @code{#f} if no @var{default} argument\n"
	    "is supplied).  Uses @code{eqv?} for equality testing.")
#define FUNC_NAME s_scm_hashv_ref
{
  if (SCM_UNBNDP (dflt))
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, key, dflt,
			  (scm_t_hash_fn) scm_ihashv,
			  (scm_t_assoc_fn) scm_sloppy_assv,
			  0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hashv_set_x, "hashv-set!", 3, 0, 0,
            (SCM table, SCM key, SCM val),
	    "Find the entry in @var{table} associated with @var{key}, and\n"
	    "store @var{value} there. Uses @code{eqv?} for equality testing.")
#define FUNC_NAME s_scm_hashv_set_x
{
  return scm_hash_fn_set_x (table, key, val,
			    (scm_t_hash_fn) scm_ihashv,
			    (scm_t_assoc_fn) scm_sloppy_assv,
			    0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashv_remove_x, "hashv-remove!", 2, 0, 0,
            (SCM table, SCM key),
	    "Remove @var{key} (and any value associated with it) from\n"
	    "@var{table}.  Uses @code{eqv?} for equality tests.")
#define FUNC_NAME s_scm_hashv_remove_x
{
  return scm_hash_fn_remove_x (table, key,
			       (scm_t_hash_fn) scm_ihashv,
			       (scm_t_assoc_fn) scm_sloppy_assv,
			       0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hash_get_handle, "hash-get-handle", 2, 0, 0,
            (SCM table, SCM key),
	    "This procedure returns the @code{(key . value)} pair from the\n"
	    "hash table @var{table}.  If @var{table} does not hold an\n"
	    "associated value for @var{key}, @code{#f} is returned.\n"
	    "Uses @code{equal?} for equality testing.")
#define FUNC_NAME s_scm_hash_get_handle
{
  return scm_hash_fn_get_handle (table, key,
				 (scm_t_hash_fn) scm_ihash,
				 (scm_t_assoc_fn) scm_sloppy_assoc,
				 0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hash_create_handle_x, "hash-create-handle!", 3, 0, 0,
            (SCM table, SCM key, SCM init),
	    "This function looks up @var{key} in @var{table} and returns its handle.\n"
	    "If @var{key} is not already present, a new handle is created which\n"
	    "associates @var{key} with @var{init}.")
#define FUNC_NAME s_scm_hash_create_handle_x
{
  return scm_hash_fn_create_handle_x (table, key, init,
				      (scm_t_hash_fn) scm_ihash,
				      (scm_t_assoc_fn) scm_sloppy_assoc,
				      0);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hash_ref, "hash-ref", 2, 1, 0,
            (SCM table, SCM key, SCM dflt),
	    "Look up @var{key} in the hash table @var{table}, and return the\n"
	    "value (if any) associated with it.  If @var{key} is not found,\n"
	    "return @var{default} (or @code{#f} if no @var{default} argument\n"
	    "is supplied).  Uses @code{equal?} for equality testing.")
#define FUNC_NAME s_scm_hash_ref
{
  if (SCM_UNBNDP (dflt))
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, key, dflt,
			  (scm_t_hash_fn) scm_ihash,
			  (scm_t_assoc_fn) scm_sloppy_assoc,
			  0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hash_set_x, "hash-set!", 3, 0, 0,
            (SCM table, SCM key, SCM val),
	    "Find the entry in @var{table} associated with @var{key}, and\n"
	    "store @var{value} there. Uses @code{equal?} for equality\n"
	    "testing.")
#define FUNC_NAME s_scm_hash_set_x
{
  return scm_hash_fn_set_x (table, key, val,
			    (scm_t_hash_fn) scm_ihash,
			    (scm_t_assoc_fn) scm_sloppy_assoc,
			    0);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hash_remove_x, "hash-remove!", 2, 0, 0,
            (SCM table, SCM key),
	    "Remove @var{key} (and any value associated with it) from\n"
	    "@var{table}.  Uses @code{equal?} for equality tests.")
#define FUNC_NAME s_scm_hash_remove_x
{
  return scm_hash_fn_remove_x (table, key,
			       (scm_t_hash_fn) scm_ihash,
			       (scm_t_assoc_fn) scm_sloppy_assoc,
			       0);
}
#undef FUNC_NAME




typedef struct scm_t_ihashx_closure
{
  SCM hash;
  SCM assoc;
} scm_t_ihashx_closure;



static unsigned long
scm_ihashx (SCM obj, unsigned long n, void *arg)
{
  SCM answer;
  scm_t_ihashx_closure *closure = (scm_t_ihashx_closure *) arg;
  answer = scm_call_2 (closure->hash, obj, scm_from_ulong (n));
  return scm_to_ulong (answer);
}



static SCM
scm_sloppy_assx (SCM obj, SCM alist, void *arg)
{
  scm_t_ihashx_closure *closure = (scm_t_ihashx_closure *) arg;
  return scm_call_2 (closure->assoc, obj, alist);
}


SCM_DEFINE (scm_hashx_get_handle, "hashx-get-handle", 4, 0, 0, 
            (SCM hash, SCM assoc, SCM table, SCM key),
	    "This behaves the same way as the corresponding\n"
	    "@code{-get-handle} function, but uses @var{hash} as a hash\n"
	    "function and @var{assoc} to compare keys.  @code{hash} must be\n"
	    "a function that takes two arguments, a key to be hashed and a\n"
	    "table size.  @code{assoc} must be an associator function, like\n"
	    "@code{assoc}, @code{assq} or @code{assv}.")
#define FUNC_NAME s_scm_hashx_get_handle
{
  scm_t_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_get_handle (table, key, scm_ihashx, scm_sloppy_assx,
				 (void *) &closure);
}
#undef FUNC_NAME


SCM_DEFINE (scm_hashx_create_handle_x, "hashx-create-handle!", 5, 0, 0, 
            (SCM hash, SCM assoc, SCM table, SCM key, SCM init),
	    "This behaves the same way as the corresponding\n"
	    "@code{-create-handle} function, but uses @var{hash} as a hash\n"
	    "function and @var{assoc} to compare keys.  @code{hash} must be\n"
	    "a function that takes two arguments, a key to be hashed and a\n"
	    "table size.  @code{assoc} must be an associator function, like\n"
	    "@code{assoc}, @code{assq} or @code{assv}.")
#define FUNC_NAME s_scm_hashx_create_handle_x
{
  scm_t_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_create_handle_x (table, key, init, scm_ihashx,
				      scm_sloppy_assx, (void *)&closure);
}
#undef FUNC_NAME



SCM_DEFINE (scm_hashx_ref, "hashx-ref", 4, 1, 0, 
            (SCM hash, SCM assoc, SCM table, SCM key, SCM dflt),
	    "This behaves the same way as the corresponding @code{ref}\n"
	    "function, but uses @var{hash} as a hash function and\n"
	    "@var{assoc} to compare keys.  @code{hash} must be a function\n"
	    "that takes two arguments, a key to be hashed and a table size.\n"
	    "@code{assoc} must be an associator function, like @code{assoc},\n"
	    "@code{assq} or @code{assv}.\n"
	    "\n"
	    "By way of illustration, @code{hashq-ref table key} is\n"
	    "equivalent to @code{hashx-ref hashq assq table key}.")
#define FUNC_NAME s_scm_hashx_ref
{
  scm_t_ihashx_closure closure;
  if (SCM_UNBNDP (dflt))
    dflt = SCM_BOOL_F;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_ref (table, key, dflt, scm_ihashx, scm_sloppy_assx,
			  (void *)&closure);
}
#undef FUNC_NAME




SCM_DEFINE (scm_hashx_set_x, "hashx-set!", 5, 0, 0,
            (SCM hash, SCM assoc, SCM table, SCM key, SCM val),
	    "This behaves the same way as the corresponding @code{set!}\n"
	    "function, but uses @var{hash} as a hash function and\n"
	    "@var{assoc} to compare keys.  @code{hash} must be a function\n"
	    "that takes two arguments, a key to be hashed and a table size.\n"
	    "@code{assoc} must be an associator function, like @code{assoc},\n"
	    "@code{assq} or @code{assv}.\n"
	    "\n"
	    " By way of illustration, @code{hashq-set! table key} is\n"
	    "equivalent to @code{hashx-set!  hashq assq table key}.")
#define FUNC_NAME s_scm_hashx_set_x
{
  scm_t_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_set_x (table, key, val, scm_ihashx, scm_sloppy_assx,
			    (void *)&closure);
}
#undef FUNC_NAME

SCM_DEFINE (scm_hashx_remove_x, "hashx-remove!", 4, 0, 0,
	    (SCM hash, SCM assoc, SCM table, SCM obj),
	    "This behaves the same way as the corresponding @code{remove!}\n"
	    "function, but uses @var{hash} as a hash function and\n"
	    "@var{assoc} to compare keys.  @code{hash} must be a function\n"
	    "that takes two arguments, a key to be hashed and a table size.\n"
	    "@code{assoc} must be an associator function, like @code{assoc},\n"
	    "@code{assq} or @code{assv}.\n"
	    "\n"
	    " By way of illustration, @code{hashq-remove! table key} is\n"
	    "equivalent to @code{hashx-remove!  hashq assq #f table key}.")
#define FUNC_NAME s_scm_hashx_remove_x
{
  scm_t_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_remove_x (table, obj, scm_ihashx, scm_sloppy_assx,
                               (void *) &closure);
}
#undef FUNC_NAME

/* Hash table iterators */

SCM_DEFINE (scm_hash_fold, "hash-fold", 3, 0, 0, 
            (SCM proc, SCM init, SCM table),
	    "An iterator over hash-table elements.\n"
            "Accumulates and returns a result by applying PROC successively.\n"
            "The arguments to PROC are \"(key value prior-result)\" where key\n"
            "and value are successive pairs from the hash table TABLE, and\n"
            "prior-result is either INIT (for the first application of PROC)\n"
            "or the return value of the previous application of PROC.\n"
            "For example, @code{(hash-fold acons '() tab)} will convert a hash\n"
            "table into an a-list of key-value pairs.")
#define FUNC_NAME s_scm_hash_fold
{
  SCM_VALIDATE_PROC (1, proc);
  if (!SCM_HASHTABLE_P (table))
    SCM_VALIDATE_VECTOR (3, table);
  return scm_internal_hash_fold ((scm_t_hash_fold_fn) scm_call_3,
				 (void *) SCM_UNPACK (proc), init, table);
}
#undef FUNC_NAME

static SCM
for_each_proc (void *proc, SCM handle)
{
  return scm_call_2 (SCM_PACK (proc), SCM_CAR (handle), SCM_CDR (handle));
}

SCM_DEFINE (scm_hash_for_each, "hash-for-each", 2, 0, 0, 
            (SCM proc, SCM table),
	    "An iterator over hash-table elements.\n"
            "Applies PROC successively on all hash table items.\n"
            "The arguments to PROC are \"(key value)\" where key\n"
            "and value are successive pairs from the hash table TABLE.")
#define FUNC_NAME s_scm_hash_for_each
{
  SCM_VALIDATE_PROC (1, proc);
  if (!SCM_HASHTABLE_P (table))
    SCM_VALIDATE_VECTOR (2, table);
  
  scm_internal_hash_for_each_handle (for_each_proc,
				     (void *) SCM_UNPACK (proc),
				     table);
  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME

SCM_DEFINE (scm_hash_for_each_handle, "hash-for-each-handle", 2, 0, 0, 
            (SCM proc, SCM table),
	    "An iterator over hash-table elements.\n"
            "Applies PROC successively on all hash table handles.")
#define FUNC_NAME s_scm_hash_for_each_handle
{
  SCM_ASSERT (scm_is_true (scm_procedure_p (proc)), proc, 1, FUNC_NAME);
  if (!SCM_HASHTABLE_P (table))
    SCM_VALIDATE_VECTOR (2, table);
  
  scm_internal_hash_for_each_handle ((scm_t_hash_handle_fn) scm_call_1,
				     (void *) SCM_UNPACK (proc),
				     table);
  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME

static SCM
map_proc (void *proc, SCM key, SCM data, SCM value)
{
  return scm_cons (scm_call_2 (SCM_PACK (proc), key, data), value);
}

SCM_DEFINE (scm_hash_map_to_list, "hash-map->list", 2, 0, 0, 
            (SCM proc, SCM table),
	    "An iterator over hash-table elements.\n"
            "Accumulates and returns as a list the results of applying PROC successively.\n"
            "The arguments to PROC are \"(key value)\" where key\n"
            "and value are successive pairs from the hash table TABLE.")
#define FUNC_NAME s_scm_hash_map_to_list
{
  SCM_VALIDATE_PROC (1, proc);
  if (!SCM_HASHTABLE_P (table))
    SCM_VALIDATE_VECTOR (2, table);
  return scm_internal_hash_fold (map_proc,
				 (void *) SCM_UNPACK (proc),
				 SCM_EOL,
				 table);
}
#undef FUNC_NAME



SCM
scm_internal_hash_fold (scm_t_hash_fold_fn fn, void *closure,
			SCM init, SCM table)
{
  long i, n;
  SCM buckets, result = init;
  
  if (SCM_HASHTABLE_P (table))
    buckets = SCM_HASHTABLE_VECTOR (table);
  else
    /* Weak alist vector.  */
    buckets = table;
  
  n = SCM_SIMPLE_VECTOR_LENGTH (buckets);
  for (i = 0; i < n; ++i)
    {
      SCM prev, ls;

      for (prev = SCM_BOOL_F, ls = SCM_SIMPLE_VECTOR_REF (buckets, i);
	   !scm_is_null (ls);
	   prev = ls, ls = SCM_CDR (ls))
	{
	  SCM handle;

	  if (!scm_is_pair (ls))
	    scm_wrong_type_arg (s_scm_hash_fold, SCM_ARG3, buckets);

	  handle = SCM_CAR (ls);
	  if (!scm_is_pair (handle))
	    scm_wrong_type_arg (s_scm_hash_fold, SCM_ARG3, buckets);

	  if (IS_WEAK_THING (table))
	    {
	      if (SCM_WEAK_PAIR_DELETED_P (handle))
		{
		  /* We hit a weak pair whose car/cdr has become
		     unreachable: unlink it from the bucket.  */
		  if (prev != SCM_BOOL_F)
		    SCM_SETCDR (prev, SCM_CDR (ls));
		  else
		    SCM_SIMPLE_VECTOR_SET (buckets, i, SCM_CDR (ls));

		  if (SCM_HASHTABLE_P (table))
		    /* Update the item count.  */
		    SCM_HASHTABLE_DECREMENT (table);

		  continue;
		}
	    }

	  result = fn (closure, SCM_CAR (handle), SCM_CDR (handle), result);
	}
    }

  return result;
}

/* The following redundant code is here in order to be able to support
   hash-for-each-handle.  An alternative would have been to replace
   this code and scm_internal_hash_fold above with a single
   scm_internal_hash_fold_handles, but we don't want to promote such
   an API. */

void
scm_internal_hash_for_each_handle (scm_t_hash_handle_fn fn, void *closure,
				   SCM table)
{
  long i, n;
  SCM buckets;
  
  if (SCM_HASHTABLE_P (table))
    buckets = SCM_HASHTABLE_VECTOR (table);
  else
    buckets = table;
  
  n = SCM_SIMPLE_VECTOR_LENGTH (buckets);
  for (i = 0; i < n; ++i)
    {
      SCM ls = SCM_SIMPLE_VECTOR_REF (buckets, i), handle;
      while (!scm_is_null (ls))
	{
	  if (!scm_is_pair (ls))
	    scm_wrong_type_arg (s_scm_hash_for_each, SCM_ARG3, buckets);
	  handle = SCM_CAR (ls);
	  if (!scm_is_pair (handle))
	    scm_wrong_type_arg (s_scm_hash_for_each, SCM_ARG3, buckets);
	  fn (closure, handle);
	  ls = SCM_CDR (ls);
	}
    }
}




void
scm_init_hashtab ()
{
#include "libguile/hashtab.x"
}

/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/
