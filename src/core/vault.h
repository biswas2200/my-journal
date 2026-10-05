/* vault: the encryption key, PIN/recovery wrapping and entry sealing.
 *
 * One random 32-byte data key encrypts every entry body with libsodium
 * secretbox (XSalsa20-Poly1305). The data key is never stored in the
 * clear while a PIN is set: it is "wrapped" (encrypted) twice, once with a
 * key derived from the PIN and once with a key derived from the recovery
 * key, both via Argon2id. A wrong PIN fails authentication, so the wrapped
 * key doubles as the salted Argon2id PIN check.
 *
 * Key bytes live in sodium_malloc() memory (guarded, not swapped) and are
 * zeroed when freed.
 */
#pragma once

#include <glib.h>

typedef struct JrKey JrKey;

typedef struct {
  guint64 ops; /* Argon2id passes */
  gsize   mem; /* Argon2id memory in bytes */
} JrKdfCost;

/* Defaults: 8 passes over 64 MiB, about 0.2 s on a recent laptop. The
 * 64 MiB is only held during the check and freed right after. A 6-digit
 * PIN has a million values, so this cost is what slows offline guessing. */
#define JR_KDF_OPS_DEFAULT 8
#define JR_KDF_MEM_DEFAULT (64u * 1024u * 1024u)
/* libsodium minimums; only for tests. */
#define JR_KDF_OPS_MIN 1
#define JR_KDF_MEM_MIN 8192u

#define JR_PIN_LEN 6
#define JR_RECOVERY_NORM_LEN 25 /* 24 symbols + NUL */

gboolean jr_crypto_init        (void);

gboolean jr_pin_valid          (const char *pin);

JrKey   *jr_key_generate       (void);
void     jr_key_free           (JrKey *key);
gboolean jr_key_equal          (const JrKey *a, const JrKey *b);

/* Base64 of the raw key, for when no PIN is set. Free with jr_secret_free. */
char    *jr_key_export         (const JrKey *key);
JrKey   *jr_key_import         (const char *b64);

/* Encrypts `key` under `secret`. Returns a printable string (g_free). */
char    *jr_key_wrap           (const JrKey *key, const char *secret, JrKdfCost cost);
/* NULL if the secret is wrong or the string is malformed. */
JrKey   *jr_key_unwrap         (const char *wrapped, const char *secret);

/* nonce || ciphertext. Free with g_free. */
guint8  *jr_seal               (const JrKey *key, const char *text, gsize len, gsize *out_len);
/* NUL-terminated plaintext, or NULL if tampered. Free with jr_secret_free. */
char    *jr_unseal             (const JrKey *key, const guint8 *blob, gsize len);

/* Zeroes then frees a string from this module. NULL is fine. */
void     jr_secret_free        (char *s);

/* "XXXX-XXXX-XXXX-XXXX-XXXX-XXXX", 120 random bits (Crockford base32).
 * Free with jr_secret_free. */
char    *jr_recovery_key_new   (void);
/* Uppercases, drops spaces and dashes, maps O->0 and I/L->1, then checks it.
 * `out` must hold JR_RECOVERY_NORM_LEN bytes. */
gboolean jr_recovery_key_normalize (const char *in, char out[JR_RECOVERY_NORM_LEN]);
