/* This file is part of GNU epsilon.

   Copyright (C) 2012 Université Paris 13
   Copyright (C) 2015 Luca Saiu
   Written by Luca Saiu

   GNU epsilon is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU epsilon is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU epsilon.  If not, see <http://www.gnu.org/licenses/>. */


#ifndef MOVINGGC_H_
#define MOVINGGC_H_

/* It's important to include features.h because of global register
   variables, which have to be visible in all compilation units. */
#include "features.h"
#include "tags.h"

/* Initialize and finalize: */
void movinggc_initialize (void);

/* Register an array of permanent roots: */
void register_roots (void **pointer_to_roots, size_t size_in_words);

/* Handle temporary roots.  It is not allowed to register permanent
   roots when any temporary root is active.  Attempting to do so will
   likely have disastrous effects, and is not checked for. */
void movinggc_push_dynamic_root (void **pointer_to_root);
void movinggc_pop_dynamic_root (void);
void movinggc_pop_dynamic_roots (size_t how_many);

/* Pushing and popping temporary roots all the time is very expensive.
   When possible it is much better to scan for roots asynchronously,
   right before a collection, for example by examining an execution
   stack with a known structure.  This is the intended use case for
   the following hooks, to be run before and after collection: the
   pre-GC hook should push temporary roots, and the post-GC hook
   should pop them.  The number of active temporary roots must be the
   same at pre-GC entry and at post-GC exit time.  Hooks may not
   allocate from the GC'd heap. */
typedef void (*movinggc_hook_t) (void *argument);
void movinggc_set_pre_hook (movinggc_hook_t hook);
void movinggc_set_post_hook (movinggc_hook_t hook);
void movinggc_set_hook_argument (void *argument);

/* Allocate a new heap object and return an UNtagged pointer to it.

   Before allocation a collection may be triggered.  In that case
   the allocation function executes, in this order:
   1) the pre-GC hook, if any, passing the GC hook argument (whose content
      may be modified by either hook);
   2) a garbage collection;
   3) the post-GC hook, if any, passing the same GC hook argument;
   4) a heap resize, if needed.

   The char version is faster, but it still assumes objects to have a
   size which is a multiple of the word size. */
void *movinggc_allocate_chars (const size_t size_in_chars)
  __attribute__ ((hot, malloc));
void *movinggc_allocate_words (const size_t size_in_words)
  __attribute__ ((hot, malloc, flatten));

/* Explicit GC.  Also executes the pre- and post-GC hooks, if any. */
void movinggc_gc (void) __attribute__ ((noinline, cold));

/* Statistics and debugging: */
float movinggc_fill_ratio (void);
const char *movinggc_semispace_name_of (const void *untagged_pointer);
long movinggc_gc_no (void); // how many times did we GC?
double movinggc_allocated_bytes (void); // how many times bytes did we allocate since the beginning?
#endif // #ifndef MOVINGGC_H_
