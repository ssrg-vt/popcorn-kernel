/* ANSI-C code produced by gperf version 3.0.4 */
/* Command-line: /usr/bin/gperf --output-file kobject_prebuilt_hash_preprocessed.c --multiple-iterations 10 kobject_prebuilt_hash.gperf  */
/* Computed positions: -k'1-2' */

#line 6 "kobject_prebuilt_hash.gperf"

#include <kernel.h>
#include <toolchain.h>
#include <syscall_handler.h>
#include <string.h>
#line 12 "kobject_prebuilt_hash.gperf"
struct z_object;
static struct k_mutex kernel_mutexes[1] = {
Z_MUTEX_INITIALIZER(kernel_mutexes[0])};

#define TOTAL_KEYWORDS 6
#define MIN_WORD_LENGTH 8
#define MAX_WORD_LENGTH 8
#define MIN_HASH_VALUE 0
#define MAX_HASH_VALUE 5
/* maximum key range = 6, duplicates = 0 */

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
/*ARGSUSED*/
static unsigned int
hash ( const char *str,  unsigned int len)
{
  static unsigned char asso_values[] =
    {
      2, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 3, 6, 6, 6, 6, 6,
      6, 6, 2, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 1, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 0, 6,
      6, 6, 6, 6, 6, 6, 2, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 2, 0,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 0, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6
    };
  return asso_values[(unsigned char)str[1]] + asso_values[(unsigned char)str[0]];
}

static unsigned char __unused lengthtable[] =
  {
     8,  8,  8,  8,  8,  8
  };

static struct z_object wordlist[] =
  {
#line 19 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0048158, {}, K_OBJ_THREAD, 0, { .thread_id = 1 }},
#line 16 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0023090, {}, K_OBJ_DRIVER_UART, 0 | K_OBJ_FLAG_DRIVER, { .unused = 0 }},
#line 21 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0099000, {}, K_OBJ_THREAD_STACK_ELEMENT, 0, { .stack_size = 8192 }},
#line 20 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0093000, {}, K_OBJ_THREAD_STACK_ELEMENT, 0, { .stack_size = 24576 }},
#line 18 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0048020, {}, K_OBJ_THREAD, 0, { .thread_id = 0 }},
#line 17 "kobject_prebuilt_hash.gperf"
    {(char *)0x00000000a0026018, {}, K_OBJ_SYS_MUTEX, 0, { .mutex = &kernel_mutexes[0] }}
  };

#ifdef __GNUC__
__inline
#if defined __GNUC_STDC_INLINE__ || defined __GNUC_GNU_INLINE__
__attribute__ ((__gnu_inline__))
#endif
#endif
static inline struct z_object *
z_object_lookup ( const char *str,  unsigned int len)
{
  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
       int key = hash((const char *)&str, len);

      if (key <= MAX_HASH_VALUE && key >= 0)
        if (len == sizeof(void *))
          {
             const char *s = wordlist[key].name;

            if (str == s)
              return &wordlist[key];
          }
    }
  return 0;
}
#line 22 "kobject_prebuilt_hash.gperf"

struct z_object *z_object_gperf_find(const void *obj)
{
    return z_object_lookup((const char *)obj, sizeof(void *));
}

void z_object_gperf_wordlist_foreach(_wordlist_cb_func_t func, void *context)
{
    int i;

    for (i = MIN_HASH_VALUE; i <= MAX_HASH_VALUE; i++) {
        if (wordlist[i].name != NULL) {
            func(&wordlist[i], context);
        }
    }
}

#ifndef CONFIG_DYNAMIC_OBJECTS
struct z_object *z_object_find(const void *obj)
	ALIAS_OF(z_object_gperf_find);

void z_object_wordlist_foreach(_wordlist_cb_func_t func, void *context)
	ALIAS_OF(z_object_gperf_wordlist_foreach);
#endif

uint8_t _thread_idx_map[2] = { 0xfc,  0xff, };
