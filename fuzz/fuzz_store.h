/* The keyring stand-in for journal files in fuzzing. make_seeds saves
 * each seed's device secret in the file itself (setting "fuzz_device",
 * which the app ignores), so a seed locked with a PIN opens again in
 * another process, and a mutated one exercises a wrong or missing
 * secret. */
#pragma once

#include "journal.h"

#define FUZZ_PIN "111111"
#define FUZZ_NEW_PIN "222222"
#define FUZZ_PASSPHRASE "correct horse battery"
#define FUZZ_MASTER "river lamp quiet oak"
#define FUZZ_DEVICE_SETTING "fuzz_device"

#define FUZZ_FAST ((JrKdfCost){ JR_KDF_OPS_MIN, JR_KDF_MEM_MIN })
/* Opens sealed bytes in the secrets target: bytes 0..31, in base64. */
#define FUZZ_FIXED_KEY "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="

typedef struct {
  JrKey *secret; /* NULL: the keyring has none */
} FuzzStore;

static inline JrDeviceStatus
fuzz_store_load (const char *id, JrKey **out, gpointer user)
{
  (void) id;
  FuzzStore *s = user;
  if (s->secret == NULL)
    return JR_DEVICE_MISSING;
  *out = jr_key_dup (s->secret);
  return JR_DEVICE_FOUND;
}

static inline gboolean
fuzz_store_save (const char *id, const JrKey *secret, gpointer user)
{
  (void) id;
  FuzzStore *s = user;
  jr_key_free (s->secret);
  s->secret = jr_key_dup (secret);
  return TRUE;
}

static inline void
fuzz_store_clear (const char *id, gpointer user)
{
  (void) id; (void) user;
}
