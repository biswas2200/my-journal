/* Fuzz target: everything that reads a secret or a key: PINs and
 * passphrases as typed, recovery keys as typed, the saved data key, the
 * saved wrapped keys and sealed entries, all from a possibly damaged
 * file. Accepted input always comes out in its one proper form. */
#include <string.h>
#include "fuzz.h"
#include "fuzz_store.h"

static void
check_lock_secret (const char *text)
{
  char *pin = jr_lock_secret_normalize (JR_LOCK_PIN, text);
  if (pin != NULL)
    {
      FUZZ_CHECK (strlen (pin) == 6 && strspn (pin, "0123456789") == 6);
      FUZZ_CHECK (jr_pin_valid (pin));
    }
  jr_secret_free (pin);

  char *phrase = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, text);
  if (phrase != NULL)
    {
      FUZZ_CHECK (g_utf8_validate (phrase, -1, NULL) && g_utf8_strlen (phrase, -1) >= 12);
      char *again = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, phrase);
      FUZZ_CHECK (again != NULL && strcmp (again, phrase) == 0);
      jr_secret_free (again);
    }
  jr_secret_free (phrase);
}

static void
check_recovery_key (const char *text)
{
  char out[JR_RECOVERY_NORM_LEN];
  if (!jr_recovery_key_normalize (text, out))
    {
      for (gsize i = 0; i < sizeof out; i++) /* refused: nothing left behind */
        FUZZ_CHECK (out[i] == 0);
      return;
    }
  FUZZ_CHECK (strlen (out) == 24 && strspn (out, "0123456789ABCDEFGHJKMNPQRSTVWXYZ") == 24);
  char again[JR_RECOVERY_NORM_LEN];
  FUZZ_CHECK (jr_recovery_key_normalize (out, again) && strcmp (again, out) == 0);
}

static void
check_saved_key (const char *text)
{
  JrKey *key = jr_key_import (text);
  if (key == NULL)
    return;
  char *exported = jr_key_export (key);
  JrKey *back = jr_key_import (exported);
  FUZZ_CHECK (back != NULL && jr_key_equal (key, back));
  jr_key_free (back);
  jr_secret_free (exported);
  jr_key_free (key);
}

static void
check_wrapped_key (const char *text)
{
  JrKdfCost cost;
  if (jr_key_wrap_cost (text, &cost))
    {
      FUZZ_CHECK (cost.ops >= JR_KDF_OPS_MIN && cost.ops <= 32);
      FUZZ_CHECK (cost.mem >= JR_KDF_MEM_MIN && cost.mem <= 1024u * 1024u * 1024u);
      FUZZ_CHECK (g_str_has_prefix (text, "v1$") || g_str_has_prefix (text, "d1$"));
    }
  if (jr_key_wrap_needs_device (text))
    FUZZ_CHECK (g_str_has_prefix (text, "d1$") && jr_key_wrap_cost (text, &cost));
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  check_lock_secret (text);
  check_recovery_key (text);
  check_saved_key (text);
  check_wrapped_key (text);

  /* Raw bytes as a sealed entry: forged ones never open. */
  JrKey *key = jr_key_import (FUZZ_FIXED_KEY);
  FUZZ_CHECK (key != NULL);
  char *opened = jr_unseal (key, data, size);
  if (opened != NULL)
    {
      /* Only a real seal opens; seal it again and it must open the same. */
      gsize len = strlen (opened), sealed_len = 0;
      guint8 *sealed = jr_seal (key, opened, len, &sealed_len);
      char *again = jr_unseal (key, sealed, sealed_len);
      FUZZ_CHECK (again != NULL && strcmp (again, opened) == 0);
      jr_secret_free (again);
      g_free (sealed);
    }
  jr_secret_free (opened);
  jr_key_free (key);
  return 0;
}
