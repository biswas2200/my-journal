/* vault: the encryption key, PIN/recovery wrapping and entry sealing.
 *
 * One random 32-byte data key encrypts every entry body with libsodium
 * secretbox (XSalsa20-Poly1305). The data key is never stored in the
 * clear while a lock is set: it is "wrapped" (encrypted) twice, once with
 * a key derived from the PIN or passphrase and once with a key derived
 * from the recovery key, both via Argon2id. A wrong secret fails
 * authentication, so the wrapped key doubles as the salted Argon2id check.
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

/* Defaults: 10 passes over 256 MiB, about 1 s on a recent laptop, on a
 * worker thread. The 256 MiB is only held during the check and freed right
 * after. A 6-digit PIN has a million values, so this cost is what slows
 * offline guessing (a passphrase is the real fix). */
#define JR_KDF_OPS_DEFAULT 10
#define JR_KDF_MEM_DEFAULT (256u * 1024u * 1024u)
/* libsodium minimums; only for tests. */
#define JR_KDF_OPS_MIN 1
#define JR_KDF_MEM_MIN 8192u

#define JR_PIN_LEN 6
#define JR_PASSPHRASE_MIN_CHARS 12
#define JR_PASSPHRASE_MAX_BYTES 256

/* What the journal is locked with. */
typedef enum {
  JR_LOCK_PIN,        /* exactly 6 digits */
  JR_LOCK_PASSPHRASE, /* 12 or more characters */
} JrLockKind;
#define JR_RECOVERY_NORM_LEN 25 /* 24 symbols + NUL */

gboolean jr_crypto_init        (void);

gboolean jr_pin_valid          (const char *pin);
/* Checks a PIN or passphrase and returns the form used for key derivation
 * (passphrases are Unicode-normalized, NFC), or NULL if it is not allowed.
 * Free with jr_secret_free. */
char    *jr_lock_secret_normalize (JrLockKind kind, const char *secret);

JrKey   *jr_key_generate       (void);
void     jr_key_free           (JrKey *key);
gboolean jr_key_equal          (const JrKey *a, const JrKey *b);
JrKey   *jr_key_dup            (const JrKey *key);

/* Base64 of the raw key, for when no PIN is set. Free with jr_secret_free. */
char    *jr_key_export         (const JrKey *key);
JrKey   *jr_key_import         (const char *b64);

/* Encrypts `key` under `secret`. Returns a printable string (g_free):
 * "v1$ops$mem$salt$nonce$box". */
char    *jr_key_wrap           (const JrKey *key, const char *secret, JrKdfCost cost);
/* NULL if the secret is wrong or the string is malformed. */
JrKey   *jr_key_unwrap         (const char *wrapped, const char *secret);

/* A PIN has only a million values, so its wrapped key also needs a device
 * secret: 32 random bytes kept outside the journal file (in the login
 * keyring). The key-encryption key is BLAKE2b keyed with the device
 * secret over Argon2id(PIN), so a copy of the file alone gives nothing to
 * test PIN guesses against. Saved as "d1$..."; with `device` NULL these
 * are jr_key_wrap and jr_key_unwrap ("v1$..."). Unwrapping needs exactly
 * what was used: a "d1" key never opens without its device secret, and a
 * "v1" key never opens with one. */
char    *jr_key_wrap_for       (const JrKey *key, const char *secret, const JrKey *device,
                                JrKdfCost cost);
JrKey   *jr_key_unwrap_for     (const char *wrapped, const char *secret, const JrKey *device);
/* TRUE for a well-formed "d1" key. */
gboolean jr_key_wrap_needs_device (const char *wrapped);
/* The Argon2id cost a wrapped key was made with. */
gboolean jr_key_wrap_cost      (const char *wrapped, JrKdfCost *out);

/* nonce || ciphertext. Free with g_free. */
guint8  *jr_seal               (const JrKey *key, const char *text, gsize len, gsize *out_len);
/* NUL-terminated plaintext, or NULL if tampered. Free with jr_secret_free. */
char    *jr_unseal             (const JrKey *key, const guint8 *blob, gsize len);

/* Zeroes `len` bytes in a way the compiler may not optimize away (plain
 * memset on a buffer about to go out of scope can be dropped). Use it for
 * every secret. NULL with 0 is fine. */
void     jr_wipe               (void *p, gsize len);

/* Zeroes then frees a string from this module. NULL is fine. */
void     jr_secret_free        (char *s);

/* "XXXX-XXXX-XXXX-XXXX-XXXX-XXXX", 120 random bits (Crockford base32).
 * Free with jr_secret_free. */
char    *jr_recovery_key_new   (void);
/* Uppercases, drops spaces and dashes, maps O->0 and I/L->1, then checks it.
 * `out` must hold JR_RECOVERY_NORM_LEN bytes; it is all zeros when the
 * key is refused. */
gboolean jr_recovery_key_normalize (const char *in, char out[JR_RECOVERY_NORM_LEN]);
