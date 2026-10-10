/* Tests for key handling and entry encryption (src/core/vault.c). */
#include <string.h>
#include <glib.h>
#include "vault.h"

/* Cheapest Argon2id settings so the tests stay fast. */
static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

static void
test_pin_validation (void)
{
  g_assert_true (jr_pin_valid ("000000"));
  g_assert_true (jr_pin_valid ("123456"));
  g_assert_false (jr_pin_valid ("12345"));
  g_assert_false (jr_pin_valid ("1234567"));
  g_assert_false (jr_pin_valid ("12345a"));
  g_assert_false (jr_pin_valid (""));
  g_assert_false (jr_pin_valid (NULL));
}

static void
test_seal_round_trip (void)
{
  JrKey *key = jr_key_generate ();
  const char *text = "Slow start today. Ünïcödé ✓";
  gsize len = 0;
  guint8 *blob = jr_seal (key, text, strlen (text), &len);
  g_assert_nonnull (blob);
  g_assert_cmpuint (len, >, strlen (text));
  /* Ciphertext must not contain the plaintext. */
  g_assert_null (g_strstr_len ((const char *) blob, (gssize) len, "Slow"));

  char *back = jr_unseal (key, blob, len);
  g_assert_cmpstr (back, ==, text);
  jr_secret_free (back);
  g_free (blob);
  jr_key_free (key);
}

static void
test_seal_empty_and_tamper (void)
{
  JrKey *key = jr_key_generate ();
  gsize len = 0;
  guint8 *blob = jr_seal (key, "", 0, &len);
  char *back = jr_unseal (key, blob, len);
  g_assert_cmpstr (back, ==, "");
  jr_secret_free (back);

  blob[len - 1] ^= 1; /* flip a bit: authentication must fail */
  g_assert_null (jr_unseal (key, blob, len));
  g_assert_null (jr_unseal (key, blob, 3)); /* too short */
  g_free (blob);
  jr_key_free (key);
}

static void
test_wrong_key_cannot_open (void)
{
  JrKey *a = jr_key_generate (), *b = jr_key_generate ();
  gsize len = 0;
  guint8 *blob = jr_seal (a, "secret", 6, &len);
  g_assert_null (jr_unseal (b, blob, len));
  g_free (blob);
  jr_key_free (a);
  jr_key_free (b);
}

static void
test_wrap_with_pin (void)
{
  JrKey *key = jr_key_generate ();
  char *wrapped = jr_key_wrap (key, "123456", FAST);
  g_assert_nonnull (wrapped);
  g_assert_null (strstr (wrapped, "123456"));

  JrKey *back = jr_key_unwrap (wrapped, "123456");
  g_assert_nonnull (back);
  g_assert_true (jr_key_equal (key, back));
  g_assert_null (jr_key_unwrap (wrapped, "654321"));
  g_assert_null (jr_key_unwrap ("v1$garbage", "123456"));
  g_assert_null (jr_key_unwrap (NULL, "123456"));

  jr_key_free (back);
  g_free (wrapped);
  jr_key_free (key);
}

static void
test_plain_export (void)
{
  JrKey *key = jr_key_generate ();
  char *plain = jr_key_export (key);
  JrKey *back = jr_key_import (plain);
  g_assert_nonnull (back);
  g_assert_true (jr_key_equal (key, back));
  g_assert_null (jr_key_import ("not base64!"));
  g_assert_null (jr_key_import ("AAAA"));
  jr_secret_free (plain);
  jr_key_free (back);
  jr_key_free (key);
}

static void
test_recovery_key_format (void)
{
  char *rk = jr_recovery_key_new ();
  /* Six groups of four from an unambiguous alphabet: XXXX-XXXX-...-XXXX */
  g_assert_cmpuint (strlen (rk), ==, 29);
  for (int i = 0; i < 29; i++)
    {
      if (i % 5 == 4)
        g_assert_cmpint (rk[i], ==, '-');
      else
        g_assert_nonnull (strchr ("0123456789ABCDEFGHJKMNPQRSTVWXYZ", rk[i]));
    }

  char norm[JR_RECOVERY_NORM_LEN];
  g_assert_true (jr_recovery_key_normalize (rk, norm));
  g_assert_cmpuint (strlen (norm), ==, 24);

  /* Typing it lower-case, without dashes, or with O/I/L for 0/1 still works. */
  char *messy = g_ascii_strdown (rk, -1);
  char norm2[JR_RECOVERY_NORM_LEN];
  g_assert_true (jr_recovery_key_normalize (messy, norm2));
  g_assert_cmpstr (norm, ==, norm2);
  g_assert_true (jr_recovery_key_normalize ("oooo iiii llll 0000 1111 2222", norm2));
  g_assert_cmpstr (norm2, ==, "000011111111000011112222");

  g_assert_false (jr_recovery_key_normalize ("short", norm2));
  g_assert_false (jr_recovery_key_normalize ("UUUU-UUUU-UUUU-UUUU-UUUU-UUUU", norm2));
  g_free (messy);
  jr_secret_free (rk);
}

static void
test_two_recovery_keys_differ (void)
{
  char *a = jr_recovery_key_new (), *b = jr_recovery_key_new ();
  g_assert_cmpstr (a, !=, b);
  jr_secret_free (a);
  jr_secret_free (b);
}

static void
test_lock_secret_rules (void)
{
  char *s;
  /* PIN: exactly six digits. */
  s = jr_lock_secret_normalize (JR_LOCK_PIN, "123456");
  g_assert_cmpstr (s, ==, "123456");
  jr_secret_free (s);
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PIN, "12345"));
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PIN, "12345x"));
  /* Passphrase: at least 12 characters, not just spaces, at most 256 bytes. */
  s = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "river lamp quiet oak");
  g_assert_cmpstr (s, ==, "river lamp quiet oak");
  jr_secret_free (s);
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "eleven char"));
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "              "));
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, NULL));
  char *huge = g_strnfill (257, 'a');
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, huge));
  g_free (huge);
  /* Twelve characters, not twelve bytes: "é" counts once. */
  s = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                                                    "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
  g_assert_nonnull (s);
  jr_secret_free (s);
  /* Unicode forms are unified (NFC). */
  s = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "cafe\xcc\x81 au lait matin");
  g_assert_cmpstr (s, ==, "caf\xc3\xa9 au lait matin");
  jr_secret_free (s);
  g_assert_null (jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, "bad utf8 \xff\xfe here"));
}

static void
test_key_dup_and_cost (void)
{
  JrKey *k = jr_key_generate ();
  JrKey *copy = jr_key_dup (k);
  g_assert_true (jr_key_equal (k, copy));
  jr_key_free (copy);

  char *wrapped = jr_key_wrap (k, "123456", FAST);
  JrKdfCost cost = { 0, 0 };
  g_assert_true (jr_key_wrap_cost (wrapped, &cost));
  g_assert_cmpuint (cost.ops, ==, FAST.ops);
  g_assert_cmpuint (cost.mem, ==, FAST.mem);
  g_assert_false (jr_key_wrap_cost ("nonsense", &cost));
  g_assert_false (jr_key_wrap_cost (NULL, &cost));
  g_free (wrapped);
  jr_key_free (k);
}

static void
test_default_cost_is_strong (void)
{
  /* Each guess of a PIN or passphrase must cost real time and memory. */
  g_assert_cmpuint (JR_KDF_OPS_DEFAULT, >=, 10);
  g_assert_cmpuint (JR_KDF_MEM_DEFAULT, >=, 256u * 1024u * 1024u);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
  g_test_add_func ("/vault/pin", test_pin_validation);
  g_test_add_func ("/vault/seal", test_seal_round_trip);
  g_test_add_func ("/vault/seal-empty-tamper", test_seal_empty_and_tamper);
  g_test_add_func ("/vault/wrong-key", test_wrong_key_cannot_open);
  g_test_add_func ("/vault/wrap", test_wrap_with_pin);
  g_test_add_func ("/vault/plain-export", test_plain_export);
  g_test_add_func ("/vault/recovery", test_recovery_key_format);
  g_test_add_func ("/vault/recovery-unique", test_two_recovery_keys_differ);
  g_test_add_func ("/vault/lock-secret-rules", test_lock_secret_rules);
  g_test_add_func ("/vault/key-dup-cost", test_key_dup_and_cost);
  g_test_add_func ("/vault/default-cost", test_default_cost_is_strong);
  return g_test_run ();
}
