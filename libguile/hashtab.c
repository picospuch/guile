/*	Copyright (C) 1995,1996 Free Software Foundation, Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA
 *
 * As a special exception, the Free Software Foundation gives permission
 * for additional uses of the text contained in its release of GUILE.
 *
 * The exception is that, if you link the GUILE library with other files
 * to produce an executable, this does not by itself cause the
 * resulting executable to be covered by the GNU General Public License.
 * Your use of that executable is in no way restricted on account of
 * linking the GUILE library code into it.
 *
 * This exception does not however invalidate any other reasons why
 * the executable file might be covered by the GNU General Public License.
 *
 * This exception applies only to the code released by the
 * Free Software Foundation under the name GUILE.  If you copy
 * code from other Free Software Foundation releases into a copy of
 * GUILE, as the General Public License permits, the exception does
 * not apply to the code that you add in this way.  To avoid misleading
 * anyone as to the status of such modified files, you must delete
 * this exception notice from them.
 *
 * If you write modifications of your own for GUILE, it is your choice
 * whether to permit this exception to apply to your modifications.
 * If you do not wish that, delete this exception notice.  */


#include <stdio.h>
#include "_scm.h"
#include "alist.h"
#include "hash.h"
#include "eval.h"

#include "hashtab.h"



SCM
scm_hash_fn_get_handle (table, obj, hash_fn, assoc_fn, closure)
     SCM table;
     SCM obj;
     unsigned int (*hash_fn)();
     SCM (*assoc_fn)();
     void * closure;
{
  unsigned int k;
  SCM h;

  SCM_ASSERT (SCM_NIMP (table) && SCM_VECTORP (table), table, SCM_ARG1, "hash_fn_get_handle");
  if (SCM_LENGTH (table) == 0)
    return SCM_EOL;
  k = hash_fn (obj, SCM_LENGTH (table), closure);
  SCM_ASSERT ((0 <= k) && (k < SCM_LENGTH (table)),
	      scm_ulong2num (k),
	      SCM_OUTOFRANGE,
	      "hash_fn_get_handle");
  h = assoc_fn (obj, SCM_VELTS (table)[k], closure);
  return h;
}



SCM
scm_hash_fn_create_handle_x (table, obj, init, hash_fn, assoc_fn, closure)
     SCM table;
     SCM obj;
     SCM init;
     unsigned int (*hash_fn)();
     SCM (*assoc_fn)();
     void * closure;
{
  unsigned int k;
  SCM it;

  SCM_ASSERT (SCM_NIMP (table) && SCM_VECTORP (table), table, SCM_ARG1, "hash_fn_create_handle_x");
  if (SCM_LENGTH (table) == 0)
    return SCM_EOL;
  k = hash_fn (obj, SCM_LENGTH (table), closure);
  SCM_ASSERT ((0 <= k) && (k < SCM_LENGTH (table)),
	      scm_ulong2num (k),
	      SCM_OUTOFRANGE,
	      "hash_fn_create_handle_x");
  SCM_REDEFER_INTS;
  it = assoc_fn (obj, SCM_VELTS (table)[k], closure);
  if (SCM_NIMP (it))
    {
      return it;
    }
  {
    SCM new_bucket;
    SCM old_bucket;
    old_bucket = SCM_VELTS (table)[k];
    new_bucket = scm_acons (obj, init, old_bucket);
    SCM_VELTS(table)[k] = new_bucket;
    SCM_REALLOW_INTS;
    return SCM_CAR (new_bucket);
  }
}




SCM 
scm_hash_fn_ref (table, obj, dflt, hash_fn, assoc_fn, closure)
     SCM table;
     SCM obj;
     SCM dflt;
     unsigned int (*hash_fn)();
     SCM (*assoc_fn)();
     void * closure;
{
  SCM it;

  it = scm_hash_fn_get_handle (table, obj, hash_fn, assoc_fn, closure);
  if (SCM_IMP (it))
    return dflt;
  else
    return SCM_CDR (it);
}




SCM 
scm_hash_fn_set_x (table, obj, val, hash_fn, assoc_fn, closure)
     SCM table;
     SCM obj;
     SCM val;
     unsigned int (*hash_fn)();
     SCM (*assoc_fn)();
     void * closure;
{
  SCM it;

  it = scm_hash_fn_create_handle_x (table, obj, SCM_BOOL_F, hash_fn, assoc_fn, closure);
  SCM_SETCDR (it, val);
  return val;
}





SCM 
scm_hash_fn_remove_x (table, obj, hash_fn, assoc_fn, delete_fn, closure)
     SCM table;
     SCM obj;
     unsigned int (*hash_fn)();
     SCM (*assoc_fn)();
     SCM (*delete_fn)();
     void * closure;
{
  unsigned int k;
  SCM h;

  SCM_ASSERT (SCM_NIMP (table) && SCM_VECTORP (table), table, SCM_ARG1, "hash_fn_remove_x");
  if (SCM_LENGTH (table) == 0)
    return SCM_EOL;
  k = hash_fn (obj, SCM_LENGTH (table), closure);
  SCM_ASSERT ((0 <= k) && (k < SCM_LENGTH (table)),
	      scm_ulong2num (k),
	      SCM_OUTOFRANGE,
	      "hash_fn_remove_x");
  h = assoc_fn (obj, SCM_VELTS (table)[k], closure);
  SCM_VELTS(table)[k] = delete_fn (h, SCM_VELTS(table)[k]);
  return h;
}




SCM_PROC (s_hashq_get_handle, "hashq-get-handle", 2, 0, 0, scm_hashq_get_handle);

SCM
scm_hashq_get_handle (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_get_handle (table, obj, scm_ihashq, scm_sloppy_assq, 0);
}


SCM_PROC (s_hashq_create_handle_x, "hashq-create-handle!", 3, 0, 0, scm_hashq_create_handle_x);

SCM
scm_hashq_create_handle_x (table, obj, init)
     SCM table;
     SCM obj;
     SCM init;
{
  return scm_hash_fn_create_handle_x (table, obj, init, scm_ihashq, scm_sloppy_assq, 0);
}


SCM_PROC (s_hashq_ref, "hashq-ref", 2, 1, 0, scm_hashq_ref);

SCM 
scm_hashq_ref (table, obj, dflt)
     SCM table;
     SCM obj;
     SCM dflt;
{
  if (dflt == SCM_UNDEFINED)
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, obj, dflt, scm_ihashq, scm_sloppy_assq, 0);
}



SCM_PROC (s_hashq_set_x, "hashq-set!", 3, 0, 0, scm_hashq_set_x);

SCM 
scm_hashq_set_x (table, obj, val)
     SCM table;
     SCM obj;
     SCM val;
{
  return scm_hash_fn_set_x (table, obj, val, scm_ihashq, scm_sloppy_assq, 0);
}



SCM_PROC (s_hashq_remove_x, "hashq-remove!", 2, 0, 0, scm_hashq_remove_x);

SCM
scm_hashq_remove_x (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_remove_x (table, obj, scm_ihashq, scm_sloppy_assq, scm_delq_x, 0);
}




SCM_PROC (s_hashv_get_handle, "hashv-get-handle", 2, 0, 0, scm_hashv_get_handle);

SCM
scm_hashv_get_handle (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_get_handle (table, obj, scm_ihashv, scm_sloppy_assv, 0);
}


SCM_PROC (s_hashv_create_handle_x, "hashv-create-handle!", 3, 0, 0, scm_hashv_create_handle_x);

SCM
scm_hashv_create_handle_x (table, obj, init)
     SCM table;
     SCM obj;
     SCM init;
{
  return scm_hash_fn_create_handle_x (table, obj, init, scm_ihashv, scm_sloppy_assv, 0);
}


SCM_PROC (s_hashv_ref, "hashv-ref", 2, 1, 0, scm_hashv_ref);

SCM 
scm_hashv_ref (table, obj, dflt)
     SCM table;
     SCM obj;
     SCM dflt;
{
  if (dflt == SCM_UNDEFINED)
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, obj, dflt, scm_ihashv, scm_sloppy_assv, 0);
}



SCM_PROC (s_hashv_set_x, "hashv-set!", 3, 0, 0, scm_hashv_set_x);

SCM 
scm_hashv_set_x (table, obj, val)
     SCM table;
     SCM obj;
     SCM val;
{
  return scm_hash_fn_set_x (table, obj, val, scm_ihashv, scm_sloppy_assv, 0);
}


SCM_PROC (s_hashv_remove_x, "hashv-remove!", 2, 0, 0, scm_hashv_remove_x);

SCM
scm_hashv_remove_x (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_remove_x (table, obj, scm_ihashv, scm_sloppy_assv, scm_delv_x, 0);
}



SCM_PROC (s_hash_get_handle, "hash-get-handle", 2, 0, 0, scm_hash_get_handle);

SCM
scm_hash_get_handle (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_get_handle (table, obj, scm_ihash, scm_sloppy_assoc, 0);
}


SCM_PROC (s_hash_create_handle_x, "hash-create-handle!", 3, 0, 0, scm_hash_create_handle_x);

SCM
scm_hash_create_handle_x (table, obj, init)
     SCM table;
     SCM obj;
     SCM init;
{
  return scm_hash_fn_create_handle_x (table, obj, init, scm_ihash, scm_sloppy_assoc, 0);
}


SCM_PROC (s_hash_ref, "hash-ref", 2, 1, 0, scm_hash_ref);

SCM 
scm_hash_ref (table, obj, dflt)
     SCM table;
     SCM obj;
     SCM dflt;
{
  if (dflt == SCM_UNDEFINED)
    dflt = SCM_BOOL_F;
  return scm_hash_fn_ref (table, obj, dflt, scm_ihash, scm_sloppy_assoc, 0);
}



SCM_PROC (s_hash_set_x, "hash-set!", 3, 0, 0, scm_hash_set_x);

SCM 
scm_hash_set_x (table, obj, val)
     SCM table;
     SCM obj;
     SCM val;
{
  return scm_hash_fn_set_x (table, obj, val, scm_ihash, scm_sloppy_assoc, 0);
}



SCM_PROC (s_hash_remove_x, "hash-remove!", 2, 0, 0, scm_hash_remove_x);

SCM
scm_hash_remove_x (table, obj)
     SCM table;
     SCM obj;
{
  return scm_hash_fn_remove_x (table, obj, scm_ihash, scm_sloppy_assoc, scm_delete_x, 0);
}




struct scm_ihashx_closure
{
  SCM hash;
  SCM assoc;
  SCM delete;
};



static unsigned int scm_ihashx SCM_P ((SCM obj, unsigned int n, struct scm_ihashx_closure * closure));

static unsigned int
scm_ihashx (obj, n, closure)
     SCM obj;
     unsigned int n;
     struct scm_ihashx_closure * closure;
{
  SCM answer;
  SCM_ALLOW_INTS;
  answer = scm_apply (closure->hash,
		      scm_listify (obj, scm_ulong2num ((unsigned long)n), SCM_UNDEFINED),
		      SCM_EOL);
  SCM_DEFER_INTS;
  return SCM_INUM (answer);
}



static SCM scm_sloppy_assx SCM_P ((SCM obj, SCM alist, struct scm_ihashx_closure * closure));

static SCM
scm_sloppy_assx (obj, alist, closure)
     SCM obj;
     SCM alist;
     struct scm_ihashx_closure * closure;
{
  SCM answer;
  SCM_ALLOW_INTS;
  answer = scm_apply (closure->assoc,
		      scm_listify (obj, alist, SCM_UNDEFINED),
		      SCM_EOL);
  SCM_DEFER_INTS;
  return answer;
}




static SCM scm_delx_x SCM_P ((SCM obj, SCM alist, struct scm_ihashx_closure * closure));

static SCM
scm_delx_x (obj, alist, closure)
     SCM obj;
     SCM alist;
     struct scm_ihashx_closure * closure;
{
  SCM answer;
  SCM_ALLOW_INTS;
  answer = scm_apply (closure->delete,
		      scm_listify (obj, alist, SCM_UNDEFINED),
		      SCM_EOL);
  SCM_DEFER_INTS;
  return answer;
}



SCM_PROC (s_hashx_get_handle, "hashx-get-handle", 4, 0, 0, scm_hashx_get_handle);

SCM
scm_hashx_get_handle (hash, assoc, table, obj)
     SCM hash;
     SCM assoc;
     SCM table;
     SCM obj;
{
  struct scm_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_get_handle (table, obj, scm_ihashx, scm_sloppy_assx, (void *)&closure);
}


SCM_PROC (s_hashx_create_handle_x, "hashx-create-handle!", 5, 0, 0, scm_hashx_create_handle_x);

SCM
scm_hashx_create_handle_x (hash, assoc, table, obj, init)
     SCM hash;
     SCM assoc;
     SCM table;
     SCM obj;
     SCM init;
{
  struct scm_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_create_handle_x (table, obj, init, scm_ihashx, scm_sloppy_assx, (void *)&closure);
}



SCM_PROC (s_hashx_ref, "hashx-ref", 4, 1, 0, scm_hashx_ref);

SCM 
scm_hashx_ref (hash, assoc, table, obj, dflt)
     SCM hash;
     SCM assoc;
     SCM table;
     SCM obj;
     SCM dflt;
{
  struct scm_ihashx_closure closure;
  if (dflt == SCM_UNDEFINED)
    dflt = SCM_BOOL_F;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_ref (table, obj, dflt, scm_ihashx, scm_sloppy_assx, (void *)&closure);
}




SCM_PROC (s_hashx_set_x, "hashx-set!", 5, 0, 0, scm_hashx_set_x);

SCM 
scm_hashx_set_x (hash, assoc, table, obj, val)
     SCM hash;
     SCM assoc;
     SCM table;
     SCM obj;
     SCM val;
{
  struct scm_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  return scm_hash_fn_set_x (table, obj, val, scm_ihashx, scm_sloppy_assx, (void *)&closure);
}



SCM
scm_hashx_remove_x (hash, assoc, delete, table, obj)
     SCM hash;
     SCM assoc;
     SCM delete;
     SCM table;
     SCM obj;
{
  struct scm_ihashx_closure closure;
  closure.hash = hash;
  closure.assoc = assoc;
  closure.delete = delete;
  return scm_hash_fn_remove_x (table, obj, scm_ihashx, scm_sloppy_assx, scm_delx_x, 0);
}




void
scm_init_hashtab ()
{
#include "hashtab.x"
}

