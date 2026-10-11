/* keyring: see keyring.h.
 *
 * One item per journal file, found by the file's random device id. It
 * shows in Passwords and Keys (Seahorse) as "Journal PIN key". */
#include "keyring.h"

#include <libsecret/secret.h>

static const SecretSchema SCHEMA = {
  "org.biswaspub.Journal.PinKey",
  SECRET_SCHEMA_NONE,
  { { "journal", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
  0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

/* Worker thread: may wait on D-Bus, or on the keyring's unlock prompt. */
static JrDeviceStatus
keyring_load (const char *id, JrKey **out, gpointer user)
{
  (void) user;
  GError *error = NULL;
  char *b64 = secret_password_lookup_sync (&SCHEMA, NULL, &error, "journal", id, NULL);
  if (error != NULL)
    {
      g_debug ("Login keyring unavailable: %s", error->message);
      g_error_free (error);
      return JR_DEVICE_UNAVAILABLE;
    }
  if (b64 == NULL)
    return JR_DEVICE_MISSING;
  *out = jr_key_import (b64);
  secret_password_free (b64); /* wipes it */
  return *out != NULL ? JR_DEVICE_FOUND : JR_DEVICE_MISSING;
}

/* Worker thread. */
static gboolean
keyring_save (const char *id, const JrKey *secret, gpointer user)
{
  (void) user;
  GError *error = NULL;
  char *b64 = jr_key_export (secret);
  gboolean ok = secret_password_store_sync (&SCHEMA, SECRET_COLLECTION_DEFAULT, "Journal PIN key", b64,
                                            NULL, &error, "journal", id, NULL);
  jr_secret_free (b64);
  if (error != NULL)
    {
      g_debug ("Could not save to the login keyring: %s", error->message);
      g_error_free (error);
    }
  return ok;
}

/* UI thread: fire and forget, never blocks. */
static void
keyring_clear (const char *id, gpointer user)
{
  (void) user;
  secret_password_clear (&SCHEMA, NULL, NULL, NULL, "journal", id, NULL);
}

const JrDeviceStore *
jr_keyring_store (void)
{
  static const JrDeviceStore store = { keyring_load, keyring_save, keyring_clear, NULL };
  return &store;
}
