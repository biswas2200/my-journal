/* Shared by every fuzz target (see fuzz/README.md).
 *
 * A target is one function, LLVMFuzzerTestOneInput, that takes any bytes
 * and must never crash, leak, hang or break one of its FUZZ_CHECKs. With
 * clang it is linked to libFuzzer; with GCC to replay.c, which runs the
 * saved inputs as a normal test. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "vault.h"

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);

/* A rule the code promises; breaking it is a finding like a crash. */
#define FUZZ_CHECK(cond)                                                       \
  do                                                                           \
    {                                                                          \
      if (!(cond))                                                             \
        {                                                                      \
          fprintf (stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
          abort ();                                                            \
        }                                                                      \
    }                                                                          \
  while (0)

static void
fuzz_quiet (const char *domain, GLogLevelFlags level, const char *message, gpointer user)
{
  (void) domain; (void) level; (void) message; (void) user;
}

/* Once per process. Warnings about damaged input are expected and
 * silenced; criticals (our code breaking its own rules) stay fatal. */
static inline void
fuzz_setup (void)
{
  static gboolean done;
  if (done)
    return;
  done = TRUE;
  if (!jr_crypto_init ())
    abort ();
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE, fuzz_quiet, NULL);
}

/* The input as a C string: it ends at the first zero byte, as text from
 * GTK or SQLite would. */
static inline char *
fuzz_string (const uint8_t *data, size_t size)
{
  return g_strndup ((const char *) data, size);
}
