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


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "features.h"
#include "movinggc.h"
#include "tags.h"

#ifdef MOVINGGC_USE_GLOBAL_POINTERS
#ifdef MOVINGGC_USE_REGISTER_POINTERS
// esi edi ebx
//register void **movinggc_fromspace_next_unallocated_word asm ("%esi");
//register void **movinggc_fromspace_after_payload_end asm ("%ebx");
#else // in the #else branch we have !defined(MOVINGGC_USE_REGISTER_POINTERS)
static void **movinggc_fromspace_next_unallocated_word = NULL;;
static void **movinggc_fromspace_after_payload_end = NULL;;
#endif // #ifdef MOVINGGC_USE_REGISTER_POINTERS
#endif // #ifdef MOVINGGC_USE_GLOBAL_POINTERS

#ifdef MOVINGGC_VERBOSE
#define movinggc_log(format, ...) \
  do { fprintf (stderr, format, ## __VA_ARGS__); fflush(stderr); } while (0)
#else
#define movinggc_log(...)       /* do nothing */
#endif // #ifdef MOVINGGC_VERBOSE

#ifdef MOVINGGC_VERY_VERBOSE
#define movinggc_verbose_log(format, ...) \
  do { fprintf (stderr, format, ## __VA_ARGS__); fflush(stderr); } while (0)
#else
#define movinggc_verbose_log(...)       /* do nothing */
#endif // #ifdef MOVINGGC_VERBOSE

#define MOVINGGC_SEMISPACE_WORD_NO \
  ((sizeof(void*) == 8) ? \
   ((1 << 16) / sizeof(void*)) /* 64Kib */ \
   : \
   ((1 << 15) / sizeof(void*)) /* 32Kib */)
#define MOVINGGC_INITIAL_ROOTS_ALLOCATED_SIZE 64

#define MOVINGGC_SEMISPACE_WORD_NO 1024 //32000 //128   // (1 * 1024 * 1024) // 128 //(2L * 1024)

/* Grow semispaces if fromspace is fuller than this ratio after a
   collection: */
#define MOVINGGC_GROW_THRESHOLD 0.33//0.01 //0.05

#define MOVINGGC_SWAP(A, B) \
  { const __typeof(A) t_e_m_porary__ = A; \
    A = B; \
    B = t_e_m_porary__; }

#define if_likely(CONDITION) \
  if(__builtin_expect(CONDITION, true))
#define if_unlikely(CONDITION) \
  if(__builtin_expect(CONDITION, false))
#define while_likely(CONDITION) \
  while(__builtin_expect(CONDITION, true))
#define while_unlikely(CONDITION) \
  while(__builtin_expect(CONDITION, false))

#define movinggc_fatal(message, ...)                                        \
  do {                                                                      \
    fprintf(stderr, "Movinggc fatal error: " message "\n", ## __VA_ARGS__); \
    exit(EXIT_FAILURE);                                                     \
  } while (0)

static movinggc_hook_t movinggc_pre_hook;
static movinggc_hook_t movinggc_post_hook;
static void *movinggc_hook_argument;
static long movinggc_gc_index;
static double movinggc_allocated_byte_no;

const char *a_name = "A";
const char *b_name = "B";
const char *nonheap_name = "out-of-heap";

struct movinggc_semispace_header
{
  void **next_unallocated_word;
  void **after_payload_end;
  void **payload_beginning;
  size_t payload_size_in_words;
  const char *name;
}; // struct
typedef struct movinggc_semispace_header *movinggc_semispace_header_t;

static struct movinggc_semispace_header movinggc_a_semispace;
static struct movinggc_semispace_header movinggc_b_semispace;
static movinggc_semispace_header_t movinggc_fromspace;
static movinggc_semispace_header_t movinggc_tospace;

static void
movinggc_dump_semispace (movinggc_semispace_header_t semispace)
{
  long payload_words =
    semispace->after_payload_end - semispace->payload_beginning;
  long words_free =
    semispace->after_payload_end - semispace->next_unallocated_word;
  float words_free_percentage __attribute__ ((unused)) =
    ((float) words_free) / payload_words * 100.0;
  long chars_free __attribute__ ((unused)) = words_free * sizeof (void*);
  fprintf (stderr, "%s %.01fkiB [%p, %p) %.01fkiB(%.01f%%) free\n",
           semispace->name, words_free * sizeof (void*) / 1024.0,
           semispace->payload_beginning, semispace->after_payload_end,
           chars_free / 1024.0,words_free_percentage);
}

static void
movinggc_dump_semispace_content (movinggc_semispace_header_t semispace)
  __attribute__ ((unused));
static void
movinggc_dump_semispace_contents (void)
  __attribute__ ((unused));

static void
movinggc_dump_semispace_contents (void)
{
  movinggc_dump_semispace_content (movinggc_fromspace);
  movinggc_dump_semispace_content (movinggc_tospace);
}

static void
movinggc_dump_semispace_content (movinggc_semispace_header_t semispace)
{
  movinggc_dump_semispace(semispace);
  void **p;
  for (p = semispace->payload_beginning ; p < semispace->after_payload_end; p ++)
    fprintf (stderr, "%p: %p or %li, tag %li\n", p, *p, (long)*p, (long)*p & 1);
  fprintf (stderr, "\n");
}

static void
movinggc_dump_semispaces (void)
{
  movinggc_dump_semispace (movinggc_fromspace);
  movinggc_dump_semispace (movinggc_tospace);
}

static movinggc_semispace_header_t
movinggc_semispace_of (const void *untagged_pointer_as_void_star)
{
  void **untagged_pointer = (void **) untagged_pointer_as_void_star;
  if (untagged_pointer >= movinggc_a_semispace.payload_beginning &&
      untagged_pointer < movinggc_a_semispace.after_payload_end)
    return &movinggc_a_semispace;
  else if (untagged_pointer >= movinggc_b_semispace.payload_beginning
           && untagged_pointer < movinggc_b_semispace.after_payload_end)
    return &movinggc_b_semispace;
  else
    return NULL;
}

const char *
movinggc_semispace_name_of (const void *untagged_pointer_as_void_star)
{
  movinggc_semispace_header_t semispace =
    movinggc_semispace_of (untagged_pointer_as_void_star);
  if (semispace != NULL)
    return semispace->name;
  else
    return nonheap_name;
}

void
movinggc_finalize_semispace (movinggc_semispace_header_t semispace)
{
  free (semispace->payload_beginning);
}

void
movinggc_initialize_semispace (movinggc_semispace_header_t semispace,
                               const char *name,
                               const size_t payload_size_in_words)
{
  /* Allocate the payload: */
  void **semispace_payload;
  semispace_payload = (void **) malloc (payload_size_in_words * sizeof(void*));
  if_unlikely (semispace_payload == NULL)
    movinggc_fatal ("movinggc_initialize_semispace(): couldn't allocate");

  /* Set fields: */
  semispace->name = name;
  semispace->payload_beginning = semispace_payload;
  semispace->after_payload_end = semispace_payload + payload_size_in_words;
  semispace->payload_size_in_words = payload_size_in_words;
  semispace->next_unallocated_word = semispace_payload;
}

void
movinggc_destructively_grow_semispace (movinggc_semispace_header_t
                                       semispace,
                                       size_t new_payload_size_in_words)
{
  const char *name = semispace->name;
  movinggc_finalize_semispace (semispace);
  movinggc_initialize_semispace (semispace, name, new_payload_size_in_words);
}

static inline void *movinggc_allocate_from (movinggc_semispace_header_t
                                            semispace, size_t size_in_chars)
  __attribute__ ((always_inline, flatten));
static inline void *
movinggc_allocate_from (movinggc_semispace_header_t semispace,
                        size_t size_in_chars)
{
#ifdef MOVINGGC_DEBUG
  if_unlikely (size_in_chars <= 0)
    movinggc_fatal ("movinggc_allocate_from(): object size not positive");
  if_unlikely (size_in_chars % sizeof (void *) != 0)
    movinggc_fatal
    ("movinggc_allocate_from(): object size not a wordsize multiple");
#endif // #ifdef MOVINGGC_DEBUG
  void **const next_unallocated_word = semispace->next_unallocated_word;
  void **const next_unallocated_word_after_the_new_objext = (void **)
    (((char *) next_unallocated_word) + size_in_chars + sizeof (void *)); // count the header word
#ifdef MOVINGGC_DEBUG
  if_unlikely (next_unallocated_word_after_the_new_objext >
               semispace->after_payload_end)
    {
      movinggc_verbose_log
        ("movinggc_allocate_from(): we were trying to allocate from %s\n",
         semispace->name);
      movinggc_fatal ("movinggc_allocate_from(): not enough space allocating %li chars from %s",
                      (long)size_in_chars, semispace->name);
    }                           // if_unlikely
#endif // #ifdef MOVINGGC_DEBUG

  /* Ok, there is space available; fill the header word, bump the pointer
     and return the next unallocated object: */
  (*next_unallocated_word) = MOVINGGC_NONFORWARDING_HEADER (size_in_chars);
  semispace->next_unallocated_word =
    next_unallocated_word_after_the_new_objext;
  return ((char *) next_unallocated_word) + sizeof (void *);
}

void movinggc_grow_semispaces (void) __attribute__ ((noinline));

float
movinggc_fill_ratio_of (movinggc_semispace_header_t semispace,
                        size_t char_no_to_be_allocated)
{
  const size_t free_word_no =
    semispace->after_payload_end -
    semispace->next_unallocated_word - char_no_to_be_allocated;
  const size_t semispace_size_in_words = semispace->payload_size_in_words;
  return 1.0 - (float) free_word_no / (float) semispace_size_in_words;
}

float
movinggc_fill_ratio (void)
{
  return movinggc_fill_ratio_of (movinggc_fromspace, 0);
}

bool movinggc_should_we_grow_semispaces (size_t chars_no_to_be_allocated)
  __attribute__ ((noinline));
bool
movinggc_should_we_grow_semispaces (size_t chars_no_to_be_allocated)
{
  const float current_fill_ratio =
    movinggc_fill_ratio_of (movinggc_fromspace, chars_no_to_be_allocated);
  return current_fill_ratio > MOVINGGC_GROW_THRESHOLD;
}

static void
movinggc_gc_internal (bool include_in_stati)
  __attribute__ ((cold, noinline));

static void
movinggc_resize_semispaces (const size_t new_semispace_size_in_words)
{
  const size_t old_semispace_size_in_words __attribute__ ((unused)) =
    movinggc_fromspace->payload_size_in_words;
  /* Destructively grow tospace: of couse we can afford to lose its
     content, as all the useful data are in fromspace: */
  movinggc_destructively_grow_semispace (movinggc_tospace,
                                         new_semispace_size_in_words);
  /* Collect, so that the growd tospace becomes fromspace and vice-versa;
     then we can destructively grow also the new tospace: */
  movinggc_gc_internal (0);
  movinggc_destructively_grow_semispace (movinggc_tospace,
                                         new_semispace_size_in_words);
#ifdef MOVINGGC_VERBOSE
  movinggc_log ("Grow semispaces to %.01fkiB: ",
                new_semispace_size_in_words / 1024.0 * sizeof (void*));
  movinggc_dump_semispace (movinggc_fromspace);
#endif // #ifdef MOVINGGC_VERBOSE
}

void
movinggc_grow_semispaces (void)
{
  movinggc_resize_semispaces (movinggc_fromspace->payload_size_in_words * 2);
}

static void movinggc_gc_then_resize_semispaces_if_needed (size_t size_in_chars)
  __attribute__ ((noinline, cold));
static void
movinggc_gc_then_resize_semispaces_if_needed (size_t size_in_chars)
{
  /* No, we need to GC before allocating... */
  movinggc_gc_internal (1);

  bool resized = false;

  /* And maybe we should also grow semispaces.  If we're really
     unlucky even more than once: */
  while_unlikely (movinggc_should_we_grow_semispaces (size_in_chars))
    {
      movinggc_grow_semispaces ();
      resized = true;
    }

  size_t free_words = movinggc_fromspace->after_payload_end
    - movinggc_fromspace->next_unallocated_word;
  if_unlikely (free_words * sizeof (void*)
               < size_in_chars + sizeof (void*))
    {
      fprintf (stderr, "WARNING: bad resizing strategy: not enough space\n");
      movinggc_resize_semispaces (movinggc_fromspace->payload_size_in_words
                                  + 2 * (size_in_chars
                                         / sizeof (void*) + sizeof(void*)));
      resized = true;
    }
  if (resized)
    movinggc_dump_semispace (movinggc_fromspace);
}

/* Using char* instead of void** saves a few instructions.  Here it's
   important. */
void *
movinggc_allocate_chars (const size_t size_in_chars)
{
  //const size_t size_in_chars = 8;
#ifdef MOVINGGC_DEBUG
  if_unlikely (size_in_chars <= 0)
    movinggc_fatal ("movinggc_allocate_chars(): object size not positive");
  if_unlikely (size_in_chars % sizeof (void *) != 0)
    movinggc_fatal
    ("movinggc_allocate_chars(): object size not a wordsize multiple");
#endif // #ifdef MOVINGGC_DEBUG

  movinggc_verbose_log ("Attempting an allocation from %s...\n",
                        movinggc_fromspace->name);

  /* Do we have enough space available in fromspace? */
#ifdef MOVINGGC_USE_GLOBAL_POINTERS
  void **res = movinggc_fromspace_next_unallocated_word + 1;
  movinggc_fromspace_next_unallocated_word =
    (void **) (((char *) res) + size_in_chars);
  if_unlikely (movinggc_fromspace_next_unallocated_word >
               movinggc_fromspace_after_payload_end)
  {
    /* This is horrible, but I have to keep the allocation fast path
       fast.  Passing a negative size to the semispace resizing
       function is a bad idea, particularly with the current clunky
       resizing strategy. */
    movinggc_fromspace_next_unallocated_word -=
      (1 + size_in_chars / sizeof (void*));
    movinggc_gc_then_resize_semispaces_if_needed (size_in_chars);
    return movinggc_allocate_chars (size_in_chars);
  }
  res[-1] = MOVINGGC_NONFORWARDING_HEADER (size_in_chars);
#else
  if_unlikely (((char *) movinggc_fromspace->next_unallocated_word)
               + size_in_chars + sizeof (void *)
               > (char *) movinggc_fromspace->after_payload_end)
    movinggc_gc_then_resize_semispaces_if_needed (size_in_chars);
  /* Ok, now we can allocate. */
  void *res = movinggc_allocate_from (movinggc_fromspace, size_in_chars);
#endif // #ifdef MOVINGGC_USE_GLOBAL_POINTERS

  movinggc_verbose_log ("...Allocated %p(%li) (%liB, %s)\n",
                        res, (long) res, size_in_chars,
                        movinggc_semispace_name_of (res));
#ifdef MOVINGGC_DEBUG
  if_unlikely (movinggc_semispace_of (res) != movinggc_fromspace)
    movinggc_fatal ("%p allocated from %s instead of fromspace (%s)", res,
                    movinggc_semispace_name_of (res), movinggc_fromspace->name);
#endif // #ifdef MOVINGGC_DEBUG
  return res;
}

void
movinggc_initialize (void)
{
  printf ("Each semispace is %li words long (%.1fKiB)\n",
          (long) MOVINGGC_SEMISPACE_WORD_NO,
          ((double) MOVINGGC_SEMISPACE_WORD_NO) * sizeof (void *) / 1024.);
  movinggc_initialize_semispace (&movinggc_a_semispace, a_name,
                                 MOVINGGC_SEMISPACE_WORD_NO);
  movinggc_initialize_semispace (&movinggc_b_semispace, b_name,
                                 MOVINGGC_SEMISPACE_WORD_NO);
  movinggc_fromspace = &movinggc_a_semispace;
  movinggc_tospace = &movinggc_b_semispace;
#ifdef MOVINGGC_USE_GLOBAL_POINTERS
  movinggc_fromspace_next_unallocated_word =
    movinggc_fromspace->next_unallocated_word;
  movinggc_fromspace_after_payload_end = movinggc_fromspace->after_payload_end;
#endif // #ifdef MOVINGGC_USE_GLOBAL_POINTERS

  movinggc_gc_index = 0;
  movinggc_allocated_byte_no = 0.0;
  movinggc_pre_hook = NULL;
  movinggc_post_hook = NULL;
  movinggc_hook_argument = NULL;

  movinggc_dump_semispaces ();
}

struct movinggc_root
{
  /* The address of the candidate pointer *must* be indirect, as we're
     gonna move it at collection time. */
  void **pointer_to_roots;
  size_t size_in_words;
};                              // struct

struct movinggc_root *movinggc_roots = NULL;
size_t movinggc_roots_allocated_size = 0;
size_t movinggc_roots_no = 0;
void
register_roots (void **pointer_to_roots, size_t size_in_words)
{
  /* Grow the array of roots, if needed: */
  if_unlikely (movinggc_roots_no == movinggc_roots_allocated_size)
    {
      movinggc_verbose_log ("Enlarging the root array from %i ",
                            (int) movinggc_roots_allocated_size);
      if (movinggc_roots_allocated_size == 0)
        movinggc_roots_allocated_size = MOVINGGC_INITIAL_ROOTS_ALLOCATED_SIZE;
      else
        movinggc_roots_allocated_size *= 2;
      movinggc_verbose_log ("to %i\n", (int) movinggc_roots_allocated_size);
      movinggc_roots = (struct movinggc_root *)
        realloc (movinggc_roots,
                 sizeof (struct movinggc_root) * movinggc_roots_allocated_size);
      if_unlikely (movinggc_roots ==
                   NULL)
        movinggc_fatal ("register_roots(): couldn't enlerge the array");
    } // if

  /* Add the new root: */
  movinggc_roots[movinggc_roots_no].pointer_to_roots = pointer_to_roots;
  movinggc_roots[movinggc_roots_no].size_in_words = size_in_words;
  movinggc_roots_no++;
  /* movinggc_verbose_log("Registered the root %p, whose first word contains %p\n", */
  /*        pointer_to_roots, *pointer_to_roots); */
  /* movinggc_verbose_log("Roots are now %i\n", (int)movinggc_roots_no); */
}

void
movinggc_push_dynamic_root (void **root_pointer)
{
  register_roots (root_pointer, 1);
}

void
movinggc_pop_dynamic_root (void)
{
  movinggc_roots_no--;
}

void
movinggc_pop_dynamic_roots (size_t how_many)
{
  movinggc_roots_no -= how_many;
}

void
movinggc_set_pre_hook (movinggc_hook_t hook)
{
  movinggc_pre_hook = hook;
}

void
movinggc_set_post_hook (movinggc_hook_t hook)
{
  movinggc_post_hook = hook;
}

void
movinggc_set_hook_argument (void *argument)
{
  movinggc_hook_argument = argument;
}

static void
movinggc_swap_spaces (void)
{
  /* Swap the space headers; header pointers are const, and they should stay
     like that because of GCC optimization, so we don't touch them. Of course
     the payload is not affected by this: */
  MOVINGGC_SWAP (movinggc_fromspace, movinggc_tospace);

  /* Reset the next_unallocated_word of what is now tospace, so that the
     next collection will start to fill it from the beginning: */
  movinggc_tospace->next_unallocated_word
    = movinggc_tospace->payload_beginning;

#ifdef MOVINGGC_USE_GLOBAL_POINTERS
  /* Reset global pointers: */
  movinggc_fromspace_next_unallocated_word
    = movinggc_fromspace->next_unallocated_word;
  movinggc_fromspace_after_payload_end
    = movinggc_fromspace->after_payload_end;
#endif // #ifdef MOVINGGC_USE_GLOBAL_POINTERS

#ifdef MOVINGGC_DEBUG
  void **p;
  for (p = movinggc_tospace->payload_beginning;
       p < movinggc_tospace->after_payload_end; p++)
    *p = (void *) 0xdead20;
  for (p = movinggc_fromspace->next_unallocated_word;
       p < movinggc_fromspace->after_payload_end; p++)
    *p = (void *) 0xdead30;
#endif // #ifdef MOVINGGC_DEBUG
  movinggc_verbose_log ("Swap semispaces: the new fromspace is %s\n", movinggc_fromspace->name);
#ifdef MOVINGGC_VERY_VERBOSE
  movinggc_dump_semispaces ();
#endif // #ifdef MOVINGGC_VERBOSE
}

/* Return the untagged version if the parameter is a valid tagged pointer,
   otherwise return NULL: */
static const void *
movinggc_untag_candidate_pointer (const void *tagged_candidate_pointer)
{
  if (MOVINGGC_IS_NONPOINTER (tagged_candidate_pointer))
    return NULL;
  /* ... otherwise we can assume that the object is a tagged pointer. */
  const void *untagged_candidate_pointer =
    MOVINGGC_UNTAG_POINTER (tagged_candidate_pointer);

#ifdef MOVINGGC_DEBUG
  /* Is there a pointer tag? */
  if_unlikely (!MOVINGGC_IS_POINTER (tagged_candidate_pointer))
    {
      movinggc_verbose_log ("tagged_candidate_pointer is %p\n",
                            tagged_candidate_pointer);
      movinggc_verbose_log ("tagged_candidate_pointer has tag %lx\n",
                            MOVINGGC_WORD_TO_TAG (tagged_candidate_pointer));
      movinggc_fatal
        ("tagged_candidate_pointer is neither a pointer nor a non-pointer");
    }

  /* Does the parameter refer an already moved pointer? */
  movinggc_semispace_header_t semispace =
    movinggc_semispace_of (untagged_candidate_pointer);
  if_unlikely (semispace == NULL)
    {
      movinggc_fatal ("pointer %p (tagged %p) points out of the heap",
                      untagged_candidate_pointer, tagged_candidate_pointer);
    }
  if_unlikely (semispace == movinggc_tospace)
    {
      movinggc_fatal ("pointer %p is already in tospace (%s)",
                      untagged_candidate_pointer,
                      movinggc_semispace_name_of (untagged_candidate_pointer));
    }
#endif // #ifdef MOVINGGC_DEBUG

  /* Ok, if we arrived here then the candidate pointer is definitely a pointer: */
  return untagged_candidate_pointer;
}

static void movinggc_scavenge_pointer_to_candidate_pointer (const void
                                                            **pointer_to_candidate_pointer);

inline static void *
movinggc_scavenge_pointer (const void *untagged_pointer)
  __attribute__ ((always_inline));

/* Move the given fromspace object and return a tagged pointer to the new tospace
   copy, unless it the parameter points to a forwarding pointer; in that case just
   return a tagged pointer to the tospace copy: */
inline static void *
movinggc_scavenge_pointer (const void *untagged_pointer)
{
  /* If we arrived here then the parameter refers a valid tagged pointer pointing
     within fromspace. */
#ifdef MOVINGGC_DEBUG
  if_unlikely (movinggc_semispace_of (untagged_pointer) != movinggc_fromspace)
    movinggc_fatal ("%p (%s) is not in fromspace", untagged_pointer,
                    movinggc_semispace_name_of (untagged_pointer));;
#endif // #ifdef MOVINGGC_DEBUG

  /* Check whether the parameter refers a forwarding pointer: */
  const void *tagged_header = ((const void **) untagged_pointer)[-1];
  if_unlikely (MOVINGGC_IS_FORWARDING (tagged_header))
    {
      void **untagged_forwarding_pointer =
        MOVINGGC_FORWARDING_HEADER_TO_DESTINATION (tagged_header);
      movinggc_verbose_log ("%p (%s) forwards to %p (%s)\n",
                            untagged_pointer,
                            movinggc_semispace_name_of (untagged_pointer),
                            untagged_forwarding_pointer,
                            movinggc_semispace_name_of
                            (untagged_forwarding_pointer));
      return MOVINGGC_TAG_POINTER (untagged_forwarding_pointer);
    } // if

#ifdef MOVINGGC_DEBUG
  /* Check that the header has a valid tag: */
  if_unlikely (!MOVINGGC_IS_NONFORWARDING (tagged_header))
    {
      movinggc_verbose_log ("tagged_header is %p\n", tagged_header);
      movinggc_fatal ("tagged_header is both forwarding and non-forwarding");
    }
#endif // #ifdef MOVINGGC_DEBUG

  /* Ok, the parameter refers a fromspace object which is not a forwarding pointer;
     we have to copy it and install a forwarding pointer in the original pointer
     object: */
  const size_t size_in_chars =
    (movinggc_bitmask_t)
    MOVINGGC_NONFORWARDING_HEADER_TO_SIZE (tagged_header);
#ifdef MOVINGGC_DEBUG
  if_unlikely (size_in_chars <= 0)
    movinggc_fatal ("corrupted header: object size not positive");
  if_unlikely (size_in_chars % sizeof (void *) != 0)
    movinggc_fatal ("corrupted header: object size not a wordsize multiple");
#endif // #ifdef MOVINGGC_DEBUG

  const void **object_in_tospace =
    movinggc_allocate_from (movinggc_tospace, size_in_chars);
  ((const void **) untagged_pointer)[-1] =
    MOVINGGC_FORWARDING_HEADER (object_in_tospace);
  movinggc_verbose_log ("* scavenging %p (%iB, %s) to %p (%s)\n",
                        untagged_pointer,
                        (int) size_in_chars,
                        movinggc_semispace_name_of (untagged_pointer),
                        object_in_tospace,
                        movinggc_semispace_name_of (object_in_tospace));

  /* Now we have to copy object fields into the new copy, and scavenge
     the new copy (or just push the pointers to the words to be changed
     onto the stack, to be scavenged later): */
#ifdef MOVINGGC_USE_MEMCPY
  memcpy (object_in_tospace, untagged_pointer, size_in_chars);
#else
  int i;
  const size_t size_in_words = size_in_chars / sizeof (void *);
  for (i = 0; i < size_in_words; i++)
    object_in_tospace[i] = ((const void **) untagged_pointer)[i];
#endif // #ifdef MOVINGGC_USE_MEMCPY

#ifdef MOVINGGC_DEBUG
  /* Clear the original object, so that we can't use it by mistake: */
  memset ((void *) untagged_pointer, 0, size_in_chars);
#endif // #ifdef MOVINGGC_DEBUG

  /* Return a tagged pointer to the new copy: */
  return MOVINGGC_TAG_POINTER (object_in_tospace);
}

static void
movinggc_scavenge_pointer_to_candidate_pointer (const void
                                                **pointer_to_candidate_pointer)
{
  /* Dereference the pointer to the candidate pointer; this is always safe if
     the parameter is, in fact, a pointer to something: */
  const void *tagged_candidate_pointer = *pointer_to_candidate_pointer;

  /* Is the candidate pointer really a pointer? Scavenge it if it is, and update the
     pointer-to-pointer; otherwise we have nothing to do: */
  const void *untagged_pointer =
    movinggc_untag_candidate_pointer (tagged_candidate_pointer);
  if (untagged_pointer != NULL)
    *pointer_to_candidate_pointer =
      movinggc_scavenge_pointer (untagged_pointer);
  else
    movinggc_verbose_log
      ("* not scavenging non-pointer %li or %p (tagged %p)\n",
       (long int) (MOVINGGC_UNTAG_NONPOINTER (tagged_candidate_pointer)),
       MOVINGGC_UNTAG_NONPOINTER (tagged_candidate_pointer),
       tagged_candidate_pointer);
}

inline static void movinggc_scavenge_pointer_to_pointer (const void
                                                  **pointer_to_tagged_pointer)
  __attribute__ ((always_inline));
inline static void
movinggc_scavenge_pointer_to_pointer (const void **pointer_to_tagged_pointer)
{
  /* Dereference the pointer-pointer; this is always safe if
     the parameter is, in fact, a pointer to something: */
  const void *tagged_pointer = *pointer_to_tagged_pointer;

  /* Scavenge and update: if the parameter is in fact a pointer to a tagged pointer,
     as it is assumed to be, we don't have to check anything: */
  const void *untagged_pointer = MOVINGGC_UNTAG_POINTER (tagged_pointer);
#ifdef MOVINGGC_DEBUG
  if (!MOVINGGC_IS_POINTER (tagged_pointer))
    movinggc_fatal
      ("movinggc_scavenge_pointer_to_pointer(): %p isn't a tagged pointer",
       untagged_pointer);
#endif // #ifdef MOVINGGC_DEBUG
  *pointer_to_tagged_pointer = movinggc_scavenge_pointer (untagged_pointer);
}

long
movinggc_gc_no (void)
{
  return movinggc_gc_index;
}

double
movinggc_allocated_bytes (void)
{
  return movinggc_allocated_byte_no;
}

/* Cheney's two-finger algorithm.  In my version the left finger
   moves from the beginning to the end of tospace, until it meets
   its allocation pointer, which is actually the right finger.
   The left finger always points to tospace object headers. */
static void movinggc_two_fingers ()
{
  const void **left_finger = (const void**)movinggc_tospace->payload_beginning;
  while (left_finger < (const void**)movinggc_tospace->next_unallocated_word)
    {
      size_t object_size_in_bytes =
        MOVINGGC_NONFORWARDING_HEADER_TO_SIZE(*left_finger);
#ifdef MOVINGGC_DEBUG
      if_unlikely (! MOVINGGC_IS_NONFORWARDING(*left_finger)
                   || object_size_in_bytes <= 0
                   || object_size_in_bytes % sizeof (void*) != 0)
        movinggc_fatal ("corrupted tospace scavenged header %p", *left_finger);
#endif // #ifdef MOVINGGC_DEBUG
      size_t object_size_in_words = object_size_in_bytes / sizeof (void*);
      int i;
      for (i = 0; i < object_size_in_words; i ++)
        movinggc_scavenge_pointer_to_candidate_pointer (left_finger + 1 + i);
      left_finger += 1 + object_size_in_words;
#ifdef MOVINGGC_DEBUG
      if_unlikely (left_finger
                   > (const void**)movinggc_tospace->next_unallocated_word)
        movinggc_fatal ("left finger crossed an object boundary");
#endif // #ifdef MOVINGGC_DEBUG
    } // while
}


static void
movinggc_gc_internal (bool include_in_stats)
{
  if (include_in_stats)
    movinggc_log ("GC#%li %s->%s... ", movinggc_gc_index,
                  movinggc_fromspace->name, movinggc_tospace->name);
  else
    movinggc_verbose_log ("Internal GC %s->%s... ",
                          movinggc_fromspace->name, movinggc_tospace->name);

  if (movinggc_pre_hook)
    {
      if (include_in_stats)
        movinggc_verbose_log ("Entering pre-GC hook...\n");
      movinggc_pre_hook (movinggc_hook_argument);
      if (include_in_stats)
        movinggc_verbose_log ("...exited pre-GC hook.\n");
    }

#ifdef MOVINGGC_VERY_VERBOSE
  movinggc_dump_semispaces ();
#endif // #ifdef MOVINGGC_VERY_VERBOSE
#ifdef MOVINGGC_DEBUG
  assert (movinggc_fill_ratio_of(movinggc_tospace, 0) == 0.0);
#endif // #ifdef MOVINGGC_DEBUG

  /* Scavenge roots. */
  int root_index;
  for (root_index = 0; root_index < movinggc_roots_no; root_index++)
    {
      const void **candidate_pointers = (const void **)
        movinggc_roots[root_index].pointer_to_roots;
      const int word_no = movinggc_roots[root_index].size_in_words;
      int word_index;
      for (word_index = 0; word_index < word_no; word_index++)
        movinggc_scavenge_pointer_to_candidate_pointer (candidate_pointers +
                                                        word_index);
    } // for

  /* Scavenge reachable objects, starting from the roots already in tospace. */
  movinggc_two_fingers ();

  if (include_in_stats)
    movinggc_allocated_byte_no
      += movinggc_tospace->payload_size_in_words * sizeof(void);

  if (movinggc_post_hook)
    {
      if (include_in_stats)
        movinggc_verbose_log ("Entering post-GC hook...\n");
      movinggc_post_hook (movinggc_hook_argument);
      if (include_in_stats)
        movinggc_verbose_log ("...exited post-GC hook.\n");
    }

  size_t scavenged_word_no __attribute__ ((unused)) =
    movinggc_tospace->next_unallocated_word
    - movinggc_tospace->payload_beginning;

  movinggc_swap_spaces ();
  if (include_in_stats)
    movinggc_log ("GC done: scavenged %.02fkiB\n",
                  (float) scavenged_word_no / sizeof(void*) / 1024.0);
#ifdef MOVINGGC_VERY_VERBOSE
  movinggc_dump_semispace (movinggc_fromspace);
#endif // #ifdef MOVINGGC_VERBOSE
  if (include_in_stats)
    movinggc_gc_index ++;
}

void
movinggc_gc (void)
{
  movinggc_gc_internal (1);
}

void *
movinggc_allocate_words (const size_t size_in_words)
{
  return movinggc_allocate_chars (size_in_words * sizeof (void *));
}
