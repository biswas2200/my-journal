/* vault: see vault.h. */
#include "vault.h"

#include <string.h>
#include <sodium.h>

#define KEY_LEN crypto_secretbox_KEYBYTES
#define NONCE_LEN crypto_secretbox_NONCEBYTES
#define MAC_LEN crypto_secretbox_MACBYTES
#define SALT_LEN crypto_pwhash_SALTBYTES
#define RECOVERY_BYTES 15  /* 120 bits -> 24 base32 symbols */
#define RECOVERY_SYMBOLS 24
/* Upper bounds accepted from the file, so a tampered value cannot make
 * the app allocate gigabytes. */
#define KDF_OPS_MAX 10
#define KDF_MEM_MAX (512u * 1024u * 1024u)

static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct JrKey {
  unsigned char *bytes; /* KEY_LEN bytes of sodium_malloc() memory */
};

gboolean
jr_crypto_init (void)
{
  return sodium_init () >= 0;
}

gboolean
jr_pin_valid (const char *pin)
{
  if (pin == NULL || strlen (pin) != JR_PIN_LEN)
    return FALSE;
  for (int i = 0; i < JR_PIN_LEN; i++)
    if (!g_ascii_isdigit (pin[i]))
      return FALSE;
  return TRUE;
}

static JrKey *
key_alloc (void)
{
  unsigned char *bytes = sodium_malloc (KEY_LEN);
  if (bytes == NULL)
    return NULL;
  JrKey *key = g_new (JrKey, 1);
  key->bytes = bytes;
  return key;
}

JrKey *
jr_key_generate (void)
{
  JrKey *key = key_alloc ();
  if (key != NULL)
    randombytes_buf (key->bytes, KEY_LEN);
  return key;
}

void
jr_key_free (JrKey *key)
{
  if (key == NULL)
    return;
  sodium_free (key->bytes); /* zeroes before releasing */
  g_free (key);
}

gboolean
jr_key_equal (const JrKey *a, const JrKey *b)
{
  return sodium_memcmp (a->bytes, b->bytes, KEY_LEN) == 0;
}

void
jr_secret_free (char *s)
{
  if (s == NULL)
    return;
  sodium_memzero (s, strlen (s));
  g_free (s);
}

char *
jr_key_export (const JrKey *key)
{
  return g_base64_encode (key->bytes, KEY_LEN);
}

/* Decodes base64 and checks the exact length; wipes the temporary copy. */
static guint8 *
decode_exact (const char *b64, gsize want)
{
  if (b64 == NULL || *b64 == '\0')
    return NULL;
  gsize len = 0;
  guint8 *raw = g_base64_decode (b64, &len);
  if (len != want)
    {
      sodium_memzero (raw, len);
      g_free (raw);
      return NULL;
    }
  return raw;
}

JrKey *
jr_key_import (const char *b64)
{
  guint8 *raw = decode_exact (b64, KEY_LEN);
  if (raw == NULL)
    return NULL;
  JrKey *key = key_alloc ();
  if (key != NULL)
    memcpy (key->bytes, raw, KEY_LEN);
  sodium_memzero (raw, KEY_LEN);
  g_free (raw);
  return key;
}

/* Argon2id: secret + salt -> key-encryption key. */
static JrKey *
derive (const char *secret, const guint8 *salt, JrKdfCost cost)
{
  JrKey *kek = key_alloc ();
  if (kek == NULL)
    return NULL;
  if (crypto_pwhash (kek->bytes, KEY_LEN, secret, strlen (secret), salt,
                     cost.ops, cost.mem, crypto_pwhash_ALG_ARGON2ID13) != 0)
    {
      jr_key_free (kek); /* out of memory */
      return NULL;
    }
  return kek;
}

char *
jr_key_wrap (const JrKey *key, const char *secret, JrKdfCost cost)
{
  guint8 salt[SALT_LEN], nonce[NONCE_LEN], box[KEY_LEN + MAC_LEN];
  randombytes_buf (salt, sizeof salt);
  randombytes_buf (nonce, sizeof nonce);

  JrKey *kek = derive (secret, salt, cost);
  if (kek == NULL)
    return NULL;
  crypto_secretbox_easy (box, key->bytes, KEY_LEN, nonce, kek->bytes);
  jr_key_free (kek);

  g_autofree char *s64 = g_base64_encode (salt, sizeof salt);
  g_autofree char *n64 = g_base64_encode (nonce, sizeof nonce);
  g_autofree char *b64 = g_base64_encode (box, sizeof box);
  return g_strdup_printf ("v1$%" G_GUINT64_FORMAT "$%" G_GSIZE_FORMAT "$%s$%s$%s",
                          cost.ops, cost.mem, s64, n64, b64);
}

JrKey *
jr_key_unwrap (const char *wrapped, const char *secret)
{
  if (wrapped == NULL || secret == NULL)
    return NULL;

  JrKey *result = NULL;
  guint8 *salt = NULL, *nonce = NULL, *box = NULL;
  char **parts = g_strsplit (wrapped, "$", 0);
  if (g_strv_length (parts) != 6 || strcmp (parts[0], "v1") != 0)
    goto out;

  JrKdfCost cost = {
    g_ascii_strtoull (parts[1], NULL, 10),
    (gsize) g_ascii_strtoull (parts[2], NULL, 10),
  };
  if (cost.ops < JR_KDF_OPS_MIN || cost.ops > KDF_OPS_MAX ||
      cost.mem < JR_KDF_MEM_MIN || cost.mem > KDF_MEM_MAX)
    goto out;

  salt = decode_exact (parts[3], SALT_LEN);
  nonce = decode_exact (parts[4], NONCE_LEN);
  box = decode_exact (parts[5], KEY_LEN + MAC_LEN);
  if (salt == NULL || nonce == NULL || box == NULL)
    goto out;

  JrKey *kek = derive (secret, salt, cost);
  if (kek == NULL)
    goto out;
  result = key_alloc ();
  if (result != NULL &&
      crypto_secretbox_open_easy (result->bytes, box, KEY_LEN + MAC_LEN, nonce, kek->bytes) != 0)
    {
      jr_key_free (result); /* wrong secret */
      result = NULL;
    }
  jr_key_free (kek);

out:
  g_free (salt);
  g_free (nonce);
  g_free (box);
  g_strfreev (parts);
  return result;
}

guint8 *
jr_seal (const JrKey *key, const char *text, gsize len, gsize *out_len)
{
  gsize total = NONCE_LEN + MAC_LEN + len;
  guint8 *blob = g_malloc (total);
  randombytes_buf (blob, NONCE_LEN);
  crypto_secretbox_easy (blob + NONCE_LEN, (const unsigned char *) text, len, blob, key->bytes);
  *out_len = total;
  return blob;
}

char *
jr_unseal (const JrKey *key, const guint8 *blob, gsize len)
{
  if (blob == NULL || len < NONCE_LEN + MAC_LEN)
    return NULL;
  gsize plain_len = len - NONCE_LEN - MAC_LEN;
  char *text = g_malloc (plain_len + 1);
  if (crypto_secretbox_open_easy ((unsigned char *) text, blob + NONCE_LEN, len - NONCE_LEN,
                                  blob, key->bytes) != 0)
    {
      g_free (text);
      return NULL;
    }
  text[plain_len] = '\0';
  return text;
}

char *
jr_recovery_key_new (void)
{
  guint8 raw[RECOVERY_BYTES];
  randombytes_buf (raw, sizeof raw);

  /* 6 groups of 4 symbols with dashes: 24 + 5 + NUL. */
  char *out = g_malloc (RECOVERY_SYMBOLS + 5 + 1);
  guint32 acc = 0;
  int bits = 0, pos = 0, symbols = 0;
  for (gsize i = 0; i < sizeof raw; i++)
    {
      acc = (acc << 8) | raw[i];
      bits += 8;
      while (bits >= 5)
        {
          bits -= 5;
          if (symbols > 0 && symbols % 4 == 0)
            out[pos++] = '-';
          out[pos++] = CROCKFORD[(acc >> bits) & 31];
          symbols++;
        }
    }
  out[pos] = '\0';
  sodium_memzero (raw, sizeof raw);
  return out;
}

gboolean
jr_recovery_key_normalize (const char *in, char out[JR_RECOVERY_NORM_LEN])
{
  if (in == NULL)
    return FALSE;
  int n = 0;
  for (const char *p = in; *p != '\0'; p++)
    {
      char c = g_ascii_toupper (*p);
      if (c == ' ' || c == '-')
        continue;
      if (c == 'O')
        c = '0';
      else if (c == 'I' || c == 'L')
        c = '1';
      if (strchr (CROCKFORD, c) == NULL || n == RECOVERY_SYMBOLS)
        {
          sodium_memzero (out, JR_RECOVERY_NORM_LEN);
          return FALSE;
        }
      out[n++] = c;
    }
  out[n] = '\0';
  return n == RECOVERY_SYMBOLS;
}
