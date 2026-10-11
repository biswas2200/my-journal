/* Fuzz target: the saved wrong-try state, as read from a possibly
 * damaged file. Accepted state is in range, saves and reads back the
 * same, and never asks for more than one 30 s wait. */
#include "fuzz.h"
#include "lockout.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  JrLockout l;
  if (!jr_lockout_from_string (text, &l))
    return 0;
  FUZZ_CHECK (jr_lockout_tries_left (&l) >= 1 && jr_lockout_tries_left (&l) <= JR_LOCKOUT_MAX_TRIES);
  FUZZ_CHECK (jr_lockout_pin_tries_left (&l) >= 0 && jr_lockout_pin_tries_left (&l) <= JR_PIN_MAX_TRIES);
  static const gint64 nows[] = { 1, G_GINT64_CONSTANT (1791316800), G_MAXINT64 };
  for (gsize i = 0; i < G_N_ELEMENTS (nows); i++)
    {
      gint64 left = jr_lockout_remaining (&l, nows[i]);
      FUZZ_CHECK (left >= 0 && left <= JR_LOCKOUT_SECS);
    }
  char buf[JR_LOCKOUT_STR_LEN];
  jr_lockout_to_string (&l, buf, sizeof buf);
  JrLockout back;
  FUZZ_CHECK (jr_lockout_from_string (buf, &back) && jr_lockout_equal (&l, &back));
  /* Every event keeps the state readable. */
  jr_lockout_fail (&l, nows[1]);
  jr_lockout_pin_fail (&l);
  jr_lockout_to_string (&l, buf, sizeof buf);
  FUZZ_CHECK (jr_lockout_from_string (buf, &back) && jr_lockout_equal (&l, &back));
  return 0;
}
