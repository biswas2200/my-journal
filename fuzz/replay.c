/* Runs a fuzz target without libFuzzer (GCC builds, see README.md).
 *
 *   replay_X DIR...                every file in DIR once: the seeds and
 *                                  every crash ever found, as a test
 *   replay_X -mutate N [-seed S] DIR...
 *                                  also N random mutations of them: a
 *                                  plain fuzzer (not guided by coverage)
 *                                  for machines without clang
 *
 * If an input crashes, it is written to crash-<pid>.bin in the current
 * folder first (on SIGABRT, SIGSEGV, SIGBUS, SIGFPE or SIGILL; run with
 * ASAN_OPTIONS=abort_on_error=1 so the sanitizers abort too). */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);

#define INPUT_MAX_LEN (64 * 1024)

static const uint8_t *current;
static size_t current_size;

static void
save_current (int sig)
{
  char name[64];
  /* Only async-signal-safe calls here. */
  int n = snprintf (name, sizeof name, "crash-%d.bin", (int) getpid ());
  int fd = n > 0 ? open (name, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
  if (fd >= 0)
    {
      if (write (fd, current, current_size) < 0)
        { /* nothing more can be done in a signal handler */ }
      close (fd);
      static const char msg[] = "replay: the crashing input is in crash-<pid>.bin\n";
      if (write (2, msg, sizeof msg - 1) < 0)
        { }
    }
  signal (sig, SIG_DFL);
  raise (sig);
}

static void
run (const uint8_t *data, size_t size)
{
  static const uint8_t empty[1];
  if (data == NULL) /* GLib gives NULL for empty data; libFuzzer never does */
    data = empty;
  current = data;
  current_size = size;
  LLVMFuzzerTestOneInput (data, size);
}

static const uint8_t SPECIAL[] = { 0, 0xff, 0x7f, 0x80, '-', ':', '$', ' ', '0', '9', '\n', 0xc3, 0xe2 };

/* One to four random changes, like libFuzzer's simpler mutations. */
static void
mutate (GRand *r, GArray *b, GPtrArray *seeds)
{
  int changes = g_rand_int_range (r, 1, 5);
  for (int c = 0; c < changes; c++)
    {
      guint len = b->len;
      switch (g_rand_int_range (r, 0, 7))
        {
        case 0: /* flip a bit */
          if (len > 0)
            g_array_index (b, guint8, g_rand_int_range (r, 0, len)) ^= (guint8) (1u << g_rand_int_range (r, 0, 8));
          break;
        case 1: /* a random byte */
          if (len > 0)
            g_array_index (b, guint8, g_rand_int_range (r, 0, len)) = (guint8) g_rand_int_range (r, 0, 256);
          break;
        case 2: /* a byte that often matters */
          if (len > 0)
            g_array_index (b, guint8, g_rand_int_range (r, 0, len)) = SPECIAL[g_rand_int_range (r, 0, sizeof SPECIAL)];
          break;
        case 3: /* insert a byte */
          if (len < INPUT_MAX_LEN)
            {
              guint8 v = (guint8) g_rand_int_range (r, 0, 256);
              g_array_insert_vals (b, g_rand_int_range (r, 0, len + 1), &v, 1);
            }
          break;
        case 4: /* delete a run */
          if (len > 0)
            {
              guint at = g_rand_int_range (r, 0, len), n = g_rand_int_range (r, 1, 9);
              g_array_remove_range (b, at, MIN (len - at, n));
            }
          break;
        case 5: /* repeat a run */
          if (len > 0 && len < INPUT_MAX_LEN / 2)
            {
              guint at = g_rand_int_range (r, 0, len), n = g_rand_int_range (r, 1, 33);
              n = MIN (len - at, n); /* MIN evaluates twice: no calls inside */
              g_autofree guint8 *copy = g_memdup2 (b->data + at, n);
              g_array_insert_vals (b, g_rand_int_range (r, 0, len + 1), copy, n);
            }
          break;
        default: /* splice in part of another seed */
          {
            GBytes *other = g_ptr_array_index (seeds, g_rand_int_range (r, 0, seeds->len));
            gsize olen;
            const guint8 *odata = g_bytes_get_data (other, &olen);
            if (olen > 0 && len > 0)
              {
                guint from = g_rand_int_range (r, 0, olen), n = g_rand_int_range (r, 1, 65);
                n = MIN (olen - from, n);
                guint at = g_rand_int_range (r, 0, len);
                g_array_set_size (b, MAX (len, at + n));
                memcpy (b->data + at, odata + from, n);
              }
          }
          break;
        }
    }
}

int
main (int argc, char **argv)
{
  gint64 mutations = 0;
  guint32 seed = (guint32) g_get_real_time ();
  GPtrArray *seeds = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "-mutate") == 0 && i + 1 < argc)
        mutations = g_ascii_strtoll (argv[++i], NULL, 10);
      else if (strcmp (argv[i], "-seed") == 0 && i + 1 < argc)
        seed = (guint32) g_ascii_strtoull (argv[++i], NULL, 10);
      else
        {
          GDir *dir = g_dir_open (argv[i], 0, NULL);
          if (dir == NULL)
            {
              g_printerr ("replay: cannot read %s\n", argv[i]);
              return 2;
            }
          GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
          const char *name;
          while ((name = g_dir_read_name (dir)) != NULL)
            g_ptr_array_add (names, g_build_filename (argv[i], name, NULL));
          g_dir_close (dir);
          g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
          for (guint k = 0; k < names->len; k++)
            {
              char *data;
              gsize len;
              if (g_file_get_contents (g_ptr_array_index (names, k), &data, &len, NULL))
                g_ptr_array_add (seeds, g_bytes_new_take (data, len));
            }
          g_ptr_array_unref (names);
        }
    }
  if (seeds->len == 0)
    {
      g_printerr ("usage: %s [-mutate N] [-seed S] DIR...\n", argv[0]);
      return 2;
    }
  static const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL };
  for (gsize i = 0; i < G_N_ELEMENTS (sigs); i++)
    signal (sigs[i], save_current);

  for (guint i = 0; i < seeds->len; i++)
    {
      gsize len;
      const guint8 *data = g_bytes_get_data (g_ptr_array_index (seeds, i), &len);
      run (data, len);
    }
  printf ("replayed %u saved inputs\n", seeds->len);

  if (mutations > 0)
    {
      GRand *r = g_rand_new_with_seed (seed);
      GArray *b = g_array_new (FALSE, FALSE, 1);
      for (gint64 n = 0; n < mutations; n++)
        {
          gsize len;
          const guint8 *data = g_bytes_get_data (g_ptr_array_index (seeds, g_rand_int_range (r, 0, seeds->len)),
                                                 &len);
          g_array_set_size (b, 0);
          g_array_append_vals (b, data, len);
          mutate (r, b, seeds);
          run ((const guint8 *) b->data, b->len);
        }
      printf ("ran %" G_GINT64_FORMAT " mutations (-seed %u)\n", mutations, seed);
      g_array_unref (b);
      g_rand_free (r);
    }
  g_ptr_array_unref (seeds);
  return 0;
}
