/* Tests for keys, locks and entry encryption (src/core/vault.c),
 * LeetCode style (see cases.h): tables of rules and tampering, hidden
 * tests with random keys, secrets and damage, and huge inputs. */
#include <string.h>
#include <glib.h>
#include "cases.h"
#include "vault.h"

/* Cheapest Argon2id settings so the tests stay fast. */
static const JrKdfCost FAST = { JR_KDF_OPS_MIN, JR_KDF_MEM_MIN };

#define OK TRUE
#define REFUSED FALSE

static const char *
yes_no (gboolean ok)
{
  return ok ? "allowed" : "refused";
}

/* Constraints: a PIN is exactly 6 ASCII digits. */
static void
test_pin_rules (void)
{
  static const struct {
    const char *name;
    const char *input;
    gboolean ok;
  } cases[] = {
    { "six digits", "123456", OK },
    { "all zeros", "000000", OK },
    { "five digits", "12345", REFUSED },
    { "seven digits", "1234567", REFUSED },
    { "a letter", "12345a", REFUSED },
    { "a space inside", "12 456", REFUSED },
    { "space before", " 123456", REFUSED },
    { "space after", "123456 ", REFUSED },
    { "plus sign", "+12345", REFUSED },
    { "minus sign", "-12345", REFUSED },
    { "full-width digits", "\xef\xbc\x91\xef\xbc\x92\xef\xbc\x93\xef\xbc\x94\xef\xbc\x95\xef\xbc\x96", REFUSED },
    { "Arabic-Indic digits", "\xd9\xa1\xd9\xa2\xd9\xa3\xd9\xa4\xd9\xa5\xd9\xa6", REFUSED },
    { "empty", "", REFUSED },
    { "NULL", NULL, REFUSED },
  };
  JrCases c = { .table = "pin-rules" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char *norm = jr_lock_secret_normalize (JR_LOCK_PIN, cases[i].input);
      gboolean ok = norm != NULL;
      jr_case (&c, cases[i].name,
               ok == cases[i].ok && ok == jr_pin_valid (cases[i].input) &&
                   (!ok || strcmp (norm, cases[i].input) == 0),
               "Input \"%s\"  Expected %s  Got %s", cases[i].input ? cases[i].input : "(null)",
               yes_no (cases[i].ok), yes_no (ok));
      jr_secret_free (norm);
    }
  jr_cases_done (&c);
}

/* Constraints: a passphrase is valid UTF-8, at most 256 bytes as typed,
 * and at least 12 characters (not bytes) that are not all spaces. Spaces
 * are kept as typed; Unicode forms are unified (NFC) so the same words
 * typed another way still match. */
static void
test_passphrase_rules (void)
{
  g_autofree char *max = g_strnfill (256, 'a');
  g_autofree char *over = g_strnfill (257, 'a');
  const struct {
    const char *name;
    const char *input;
    const char *expected; /* NULL: refused */
  } cases[] = {
    { "words", "river lamp quiet oak", "river lamp quiet oak" },
    { "exactly 12 characters", "twelve chars", "twelve chars" },
    { "11 characters", "eleven char", NULL },
    { "spaces only", "            ", NULL },
    { "tabs and spaces only", "\t \t \t \t \t \t ", NULL },
    { "spaces kept as typed", "  river lamp quiet oak  ", "  river lamp quiet oak  " },
    { "256 bytes", max, max },
    { "257 bytes", over, NULL },
    { "12 accented letters (24 bytes)",
      "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
      "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9" },
    { "11 accented letters (22 bytes)",
      "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", NULL },
    { "accent typed separately (NFC)", "cafe\xcc\x81 au lait matin", "caf\xc3\xa9 au lait matin" },
    { "12 emoji", "\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92"
                  "\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92"
                  "\xf0\x9f\x94\x92\xf0\x9f\x94\x92",
      "\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92"
      "\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92\xf0\x9f\x94\x92"
      "\xf0\x9f\x94\x92\xf0\x9f\x94\x92" },
    { "broken UTF-8", "bad utf8 \xff\xfe here", NULL },
    { "a lone accent byte at the end", "river lamp quiet \xc3", NULL },
    { "empty", "", NULL },
    { "NULL", NULL, NULL },
  };
  JrCases c = { .table = "passphrase-rules" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char *norm = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, cases[i].input);
      g_autofree char *in = cases[i].input ? g_strescape (cases[i].input, NULL) : g_strdup ("(null)");
      g_autofree char *want = cases[i].expected ? g_strescape (cases[i].expected, NULL) : g_strdup ("refused");
      g_autofree char *got = norm ? g_strescape (norm, NULL) : g_strdup ("refused");
      jr_case (&c, cases[i].name, g_strcmp0 (norm, cases[i].expected) == 0,
               "Input \"%.60s\"  Expected \"%.60s\"  Got \"%.60s\"", in, want, got);
      jr_secret_free (norm);
    }
  jr_cases_done (&c);
}

/* Constraints: 24 symbols of Crockford base32 in any case, with spaces
 * and dashes anywhere; O reads as 0, I and L as 1. A refused key leaves
 * nothing behind in the output buffer. */
static void
test_recovery_key_rules (void)
{
  static const struct {
    const char *name;
    const char *input;
    const char *expected; /* NULL: refused */
  } cases[] = {
    { "as shown", "0123-4567-89AB-CDEF-GHJK-MNPQ", "0123456789ABCDEFGHJKMNPQ" },
    { "lower case", "0123-4567-89ab-cdef-ghjk-mnpq", "0123456789ABCDEFGHJKMNPQ" },
    { "no dashes", "0123456789ABCDEFGHJKMNPQ", "0123456789ABCDEFGHJKMNPQ" },
    { "spaces instead", "0123 4567 89AB CDEF GHJK MNPQ", "0123456789ABCDEFGHJKMNPQ" },
    { "O, I and L for 0 and 1", "oooo iiii llll 0000 1111 2222", "000011111111000011112222" },
    { "dashes everywhere", "-0-1-2-3-4-5-6-7-8-9-A-B-C-D-E-F-G-H-J-K-M-N-P-Q-", "0123456789ABCDEFGHJKMNPQ" },
    { "23 symbols", "0123-4567-89AB-CDEF-GHJK-MNP", NULL },
    { "25 symbols", "0123-4567-89AB-CDEF-GHJK-MNPQR", NULL },
    { "U is not in the alphabet", "UUUU-UUUU-UUUU-UUUU-UUUU-UUUU", NULL },
    { "a dot", "0123.4567-89AB-CDEF-GHJK-MNPQ", NULL },
    { "a tab", "0123\t4567-89AB-CDEF-GHJK-MNPQ", NULL },
    { "non-ASCII", "0123-4567-89AB-CDEF-GHJK-MNP\xc3\x96", NULL },
    { "only dashes", "-----", NULL },
    { "empty", "", NULL },
    { "NULL", NULL, NULL },
  };
  JrCases c = { .table = "recovery-key-rules" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char out[JR_RECOVERY_NORM_LEN];
      memset (out, 'Z', sizeof out);
      gboolean ok = jr_recovery_key_normalize (cases[i].input, out);
      gboolean clean = TRUE;
      for (gsize k = 0; !ok && k < sizeof out; k++)
        clean = clean && out[k] == '\0';
      jr_case (&c, cases[i].name,
               ok == (cases[i].expected != NULL) && (!ok || strcmp (out, cases[i].expected) == 0) &&
                   (ok || clean),
               "Input \"%s\"  Expected %s  Got %s%s", cases[i].input ? cases[i].input : "(null)",
               cases[i].expected ? cases[i].expected : "refused", ok ? out : "refused",
               ok || clean ? "" : " (and left symbols in the buffer)");
    }
  jr_cases_done (&c);
}

/* "ver$ops$mem$salt$nonce$box" with field `field` replaced. */
static char *
with_field (const char *wrapped, int field, const char *value)
{
  g_auto (GStrv) parts = g_strsplit (wrapped, "$", 0);
  g_free (parts[field]);
  parts[field] = g_strdup (value);
  return g_strjoinv ("$", parts);
}

/* Constraints: what jr_key_wrap saved, as read back from the file; any
 * damage or tampering is refused, and a cost read from the file stays
 * within bounds (1..32 passes, 8 KiB..1 GiB), so a tampered value can
 * never make the app allocate gigabytes. */
static void
test_wrapped_form (void)
{
  JrKey *key = jr_key_generate ();
  char *wrapped = jr_key_wrap (key, "123456", FAST);
  g_auto (GStrv) parts = g_strsplit (wrapped, "$", 0);
  g_autofree char *salt15 = g_base64_encode ((const guchar *) "0123456789abcde", 15);
  g_autofree char *box_flipped = g_strdup (parts[5]);
  box_flipped[3] = box_flipped[3] == 'A' ? 'B' : 'A';
  g_autofree char *box_short = g_strndup (parts[5], strlen (parts[5]) - 4);
  g_autofree char *extra = g_strconcat (wrapped, "$", NULL);
  g_autofree char *missing = g_strndup (wrapped, strrchr (wrapped, '$') - wrapped);
  g_autofree char *spaced = g_strconcat (" ", wrapped, NULL);
  const struct {
    const char *name;
    int field; /* -1: use `whole` */
    const char *value;
    gboolean cost_ok;
  } cases[] = {
    { "as saved", 0, "v1", OK },
    { "version v2", 0, "v2", REFUSED },
    { "version empty", 0, "", REFUSED },
    { "passes 0", 1, "0", REFUSED },
    { "passes 32", 1, "32", OK },
    { "passes 33", 1, "33", REFUSED },
    { "passes negative", 1, "-1", REFUSED },
    { "passes with junk", 1, "1abc", REFUSED },
    { "passes with plus", 1, "+1", REFUSED },
    { "passes overflow", 1, "99999999999999999999", REFUSED },
    { "passes empty", 1, "", REFUSED },
    { "memory below 8 KiB", 2, "8191", REFUSED },
    { "memory 8 KiB", 2, "8192", OK },
    { "memory over 1 GiB", 2, "1073741825", REFUSED },
    { "memory with junk", 2, "8192k", REFUSED },
    { "memory overflow", 2, "99999999999999999999", REFUSED },
    { "salt not base64", 3, "!!!!", OK },
    { "salt one byte short", 3, salt15, OK },
    { "nonce empty", 4, "", OK },
    { "box bit flipped", 5, box_flipped, OK },
    { "box cut short", 5, box_short, OK },
  };
  JrCases c = { .table = "wrapped-form" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *tampered = with_field (wrapped, cases[i].field, cases[i].value);
      JrKdfCost cost = { 0, 0 };
      gboolean cost_ok = jr_key_wrap_cost (tampered, &cost);
      jr_case (&c, cases[i].name, cost_ok == cases[i].cost_ok, "Input field %d = \"%s\"  Expected cost %s  Got %s",
               cases[i].field, cases[i].value, yes_no (cases[i].cost_ok), yes_no (cost_ok));
      /* Only the untouched form may open, and only with the right PIN. */
      gboolean untouched = strcmp (tampered, wrapped) == 0;
      JrKey *back = jr_key_unwrap (tampered, "123456");
      gboolean opened = back != NULL && jr_key_equal (back, key);
      jr_case (&c, cases[i].name, opened == untouched && (back == NULL || opened),
               "Input field %d = \"%s\"  Expected %s  Got %s", cases[i].field, cases[i].value,
               untouched ? "opens" : "refused", back == NULL ? "refused" : opened ? "opens" : "A DIFFERENT KEY");
      jr_key_free (back);
    }
  const struct {
    const char *name;
    const char *whole;
  } whole[] = {
    { "an extra field", extra }, { "a missing field", missing }, { "a space before", spaced },
    { "empty", "" },              { "no fields", "v1" },          { "NULL", NULL },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (whole); i++)
    {
      JrKdfCost cost;
      JrKey *back = jr_key_unwrap (whole[i].whole, "123456");
      jr_case (&c, whole[i].name, !jr_key_wrap_cost (whole[i].whole, &cost) && back == NULL,
               "Input \"%.40s\"  Expected refused  Got %s", whole[i].whole ? whole[i].whole : "(null)",
               back ? "opens" : "a cost");
      jr_key_free (back);
    }
  jr_cases_done (&c);
  g_free (wrapped);
  jr_key_free (key);
}

typedef enum { NO_DEVICE, SAME_DEVICE, OTHER_DEVICE } DeviceUse;

/* Constraints: a PIN's key is saved with a device secret kept outside
 * the file ("d1"); everything else without ("v1"). Opening needs the
 * right secret and exactly the right device secret, and the version tag
 * cannot be edited to drop the device secret. */
static void
test_device_bound (void)
{
  static const struct {
    const char *name;
    gboolean bound;      /* saved with a device secret */
    const char *version; /* NULL: as saved, else replaced */
    const char *secret;
    DeviceUse device;
    gboolean opens;
  } cases[] = {
    { "no device: right PIN", FALSE, NULL, "123456", NO_DEVICE, OK },
    { "no device: wrong PIN", FALSE, NULL, "654321", NO_DEVICE, REFUSED },
    { "no device, but one given", FALSE, NULL, "123456", SAME_DEVICE, REFUSED },
    { "device: right PIN and device", TRUE, NULL, "123456", SAME_DEVICE, OK },
    { "device: wrong PIN", TRUE, NULL, "654321", SAME_DEVICE, REFUSED },
    { "device: another laptop's device secret", TRUE, NULL, "123456", OTHER_DEVICE, REFUSED },
    { "device: a copy of the file alone", TRUE, NULL, "123456", NO_DEVICE, REFUSED },
    { "device: tag edited to v1, no device", TRUE, "v1", "123456", NO_DEVICE, REFUSED },
    { "device: tag edited to v1, with device", TRUE, "v1", "123456", SAME_DEVICE, REFUSED },
    { "no device: tag edited to d1", FALSE, "d1", "123456", SAME_DEVICE, REFUSED },
  };
  JrKey *key = jr_key_generate (), *device = jr_key_generate (), *other = jr_key_generate ();
  JrCases c = { .table = "device-bound" };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char *wrapped = jr_key_wrap_for (key, "123456", cases[i].bound ? device : NULL, FAST);
      gboolean needs = jr_key_wrap_needs_device (wrapped);
      jr_case (&c, cases[i].name, needs == cases[i].bound && g_str_has_prefix (wrapped, needs ? "d1$" : "v1$"),
               "Input saved %s a device secret  Expected tag %s  Got \"%.3s\"", cases[i].bound ? "with" : "without",
               cases[i].bound ? "d1" : "v1", wrapped);
      if (cases[i].version != NULL)
        {
          char *edited = with_field (wrapped, 0, cases[i].version);
          g_free (wrapped);
          wrapped = edited;
        }
      const JrKey *use = cases[i].device == SAME_DEVICE ? device : cases[i].device == OTHER_DEVICE ? other : NULL;
      JrKey *back = jr_key_unwrap_for (wrapped, cases[i].secret, use);
      gboolean opened = back != NULL && jr_key_equal (back, key);
      jr_case (&c, cases[i].name, opened == cases[i].opens && (back == NULL || opened),
               "Input PIN %s, %s  Expected %s  Got %s", cases[i].secret,
               cases[i].device == SAME_DEVICE ? "same device" : cases[i].device == OTHER_DEVICE ? "other device" : "no device",
               cases[i].opens ? "opens" : "refused", back == NULL ? "refused" : opened ? "opens" : "A DIFFERENT KEY");
      jr_key_free (back);
      g_free (wrapped);
    }
  const struct {
    const char *input;
    gboolean needs;
  } tags[] = { { "garbage", FALSE }, { "d1", FALSE }, { "", FALSE }, { NULL, FALSE } };
  for (gsize i = 0; i < G_N_ELEMENTS (tags); i++)
    jr_case (&c, "needs-device on junk", jr_key_wrap_needs_device (tags[i].input) == tags[i].needs,
             "Input \"%s\"  Expected no  Got yes", tags[i].input ? tags[i].input : "(null)");
  jr_cases_done (&c);
  jr_key_free (key);
  jr_key_free (device);
  jr_key_free (other);
}

/* Constraints: any text up to the size of an entry. Sealed text never
 * shows in the output, and any change to a sealed entry, or the wrong
 * key, is refused. */
static void
test_seal (void)
{
  static const struct {
    const char *name;
    const char *text;
  } texts[] = {
    { "empty", "" },
    { "one letter", "a" },
    { "a sentence", "Slow start today." },
    { "Unicode", "\xc3\x9c" "n\xc3\xaf" "c\xc3\xb6" "d\xc3\xa9 \xe2\x9c\x93 \xe6\x97\xa5\xe6\x9c\xac" },
    { "new lines", "line one\nline two\r\n" },
  };
  JrKey *key = jr_key_generate (), *other = jr_key_generate ();
  JrCases c = { .table = "seal" };
  for (gsize i = 0; i < G_N_ELEMENTS (texts); i++)
    {
      gsize len = 0, n = strlen (texts[i].text);
      guint8 *blob = jr_seal (key, texts[i].text, n, &len);
      char *back = jr_unseal (key, blob, len);
      jr_case (&c, texts[i].name, g_strcmp0 (back, texts[i].text) == 0 && len == n + 40,
               "Input \"%s\"  Expected the same text back (%" G_GSIZE_FORMAT " bytes sealed)  Got \"%s\" (%" G_GSIZE_FORMAT ")",
               texts[i].text, n + 40, back ? back : "refused", len);
      jr_secret_free (back);
      jr_case (&c, texts[i].name, n < 4 || g_strstr_len ((const char *) blob, (gssize) len, texts[i].text) == NULL,
               "Input \"%s\"  Expected not visible when sealed  Got visible", texts[i].text);
      char *wrong = jr_unseal (other, blob, len);
      jr_case (&c, texts[i].name, wrong == NULL, "Input \"%s\" with the wrong key  Expected refused  Got \"%s\"",
               texts[i].text, wrong);
      jr_secret_free (wrong);
      g_free (blob);
    }
  /* Damage in each part of a sealed entry: nonce, tag, text. */
  gsize len = 0;
  guint8 *blob = jr_seal (key, "secret words", 12, &len);
  const struct {
    const char *name;
    gsize at;
  } flips[] = { { "nonce changed", 0 }, { "tag changed", 24 }, { "text changed", 40 }, { "last byte changed", 51 } };
  for (gsize i = 0; i < G_N_ELEMENTS (flips); i++)
    {
      blob[flips[i].at] ^= 1;
      char *back = jr_unseal (key, blob, len);
      jr_case (&c, flips[i].name, back == NULL, "Input byte %" G_GSIZE_FORMAT " flipped  Expected refused  Got \"%s\"",
               flips[i].at, back);
      jr_secret_free (back);
      blob[flips[i].at] ^= 1;
    }
  const gsize cuts[] = { 0, 1, 39, len - 1 };
  for (gsize i = 0; i < G_N_ELEMENTS (cuts); i++)
    {
      char *back = jr_unseal (key, blob, cuts[i]);
      jr_case (&c, "cut short", back == NULL, "Input %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes  Expected refused  Got \"%s\"",
               cuts[i], len, back);
      jr_secret_free (back);
    }
  char *null_blob = jr_unseal (key, NULL, 52);
  jr_case (&c, "NULL", null_blob == NULL, "Input NULL  Expected refused  Got text");
  g_free (blob);
  jr_cases_done (&c);
  jr_key_free (key);
  jr_key_free (other);
}

static void
test_key_basics (void)
{
  JrCases c = { .table = "key-basics" };
  JrKey *k = jr_key_generate (), *copy = jr_key_dup (k), *other = jr_key_generate ();
  jr_case (&c, "copy is equal", jr_key_equal (k, copy), "Expected equal  Got different");
  jr_case (&c, "two new keys differ", !jr_key_equal (k, other), "Expected different  Got equal");
  char *plain = jr_key_export (k);
  JrKey *back = jr_key_import (plain);
  jr_case (&c, "export and import", back != NULL && jr_key_equal (k, back), "Expected the same key  Got %s",
           back ? "another" : "refused");
  const char *bad[] = { "not base64!", "AAAA", "", NULL };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      JrKey *b = jr_key_import (bad[i]);
      jr_case (&c, "import refuses junk", b == NULL, "Input \"%s\"  Expected refused  Got a key",
               bad[i] ? bad[i] : "(null)");
      jr_key_free (b);
    }
  char *wrapped = jr_key_wrap (k, "123456", FAST);
  JrKdfCost cost = { 0, 0 };
  jr_case (&c, "cost is read back", jr_key_wrap_cost (wrapped, &cost) && cost.ops == FAST.ops && cost.mem == FAST.mem,
           "Expected %" G_GUINT64_FORMAT "/%" G_GSIZE_FORMAT "  Got %" G_GUINT64_FORMAT "/%" G_GSIZE_FORMAT,
           FAST.ops, FAST.mem, cost.ops, cost.mem);
  jr_case (&c, "the PIN is not in the saved form", strstr (wrapped, "123456") == NULL, "Expected hidden  Got visible");
  /* Each guess of a PIN or passphrase must cost real time and memory. */
  jr_case (&c, "default cost is strong", JR_KDF_OPS_DEFAULT >= 10 && JR_KDF_MEM_DEFAULT >= 256u * 1024u * 1024u,
           "Expected >= 10 passes over 256 MiB");
  /* jr_wipe is the one way secrets are cleared: unlike memset, the
   * compiler may not drop it even right before the buffer goes away. */
  char buf[16];
  g_strlcpy (buf, "123456", sizeof buf);
  jr_wipe (buf, sizeof buf);
  gboolean zero = TRUE;
  for (gsize i = 0; i < sizeof buf; i++)
    zero = zero && buf[i] == 0;
  jr_wipe (NULL, 0); /* harmless */
  jr_case (&c, "wipe zeroes", zero, "Expected all zero  Got bytes left");
  jr_cases_done (&c);
  g_free (wrapped);
  jr_secret_free (plain);
  jr_key_free (back);
  jr_key_free (other);
  jr_key_free (copy);
  jr_key_free (k);
}

/* A random PIN, or a random passphrase of 12-40 characters. */
static char *
random_secret (JrLockKind *kind)
{
  static const char *const words[] = { "river", "lamp", "quiet", "oak", "caf\xc3\xa9", "\xe6\x97\xa5",
                                       "\xf0\x9f\x94\x92", "Z", " ", "9" };
  if (g_test_rand_bit ())
    {
      *kind = JR_LOCK_PIN;
      return g_strdup_printf ("%06d", g_test_rand_int_range (0, 1000000));
    }
  *kind = JR_LOCK_PASSPHRASE;
  GString *s = g_string_new ("passphrase:");
  int n = g_test_rand_int_range (1, 12);
  for (int i = 0; i < n; i++)
    g_string_append (s, words[g_test_rand_int_range (0, G_N_ELEMENTS (words))]);
  return g_string_free (s, FALSE);
}

/* Hidden test: random keys, secrets and device secrets. The right ones
 * open; another secret or a changed character of the saved form never
 * opens a different key (it is refused, or at most decodes to the same
 * bytes when only unused base64 padding bits changed). */
static void
test_hidden_wrap (void)
{
  JrCases c = { .table = "hidden: wrap" };
  for (int n = 0; n < 300; n++)
    {
      JrLockKind kind, other_kind;
      char *secret = random_secret (&kind), *other_secret = random_secret (&other_kind);
      char *norm = jr_lock_secret_normalize (kind, secret);
      JrKey *key = jr_key_generate ();
      JrKey *device = g_test_rand_bit () ? jr_key_generate () : NULL;
      char *wrapped = jr_key_wrap_for (key, norm ? norm : secret, device, FAST);
      JrKey *back = jr_key_unwrap_for (wrapped, norm ? norm : secret, device);
      jr_case (&c, "round trip", back != NULL && jr_key_equal (back, key), "Input secret \"%s\"%s  Expected opens  Got %s",
               secret, device ? " with a device secret" : "", back ? "a different key" : "refused");
      jr_key_free (back);
      if (g_strcmp0 (secret, other_secret) != 0)
        {
          back = jr_key_unwrap_for (wrapped, other_secret, device);
          jr_case (&c, "another secret", back == NULL, "Input \"%s\" saved, \"%s\" tried  Expected refused  Got a key",
                   secret, other_secret);
          jr_key_free (back);
        }
      g_autofree char *damaged = g_strdup (wrapped);
      gsize at = (gsize) g_test_rand_int_range (0, (gint32) strlen (damaged));
      damaged[at] = "Av1$0+/=9Zd"[g_test_rand_int_range (0, 11)];
      back = jr_key_unwrap_for (damaged, norm ? norm : secret, device);
      gboolean same = strcmp (damaged, wrapped) == 0;
      jr_case (&c, "one character changed", back == NULL || jr_key_equal (back, key),
               "Input \"%s\" (character %" G_GSIZE_FORMAT ")  Expected refused%s  Got A DIFFERENT KEY", damaged, at,
               same ? " or the same" : "");
      jr_key_free (back);
      g_free (wrapped);
      jr_key_free (device);
      jr_key_free (key);
      jr_secret_free (norm);
      g_free (secret);
      g_free (other_secret);
    }
  jr_cases_done (&c);
}

/* Hidden test: new recovery keys are well formed, unique, and read back
 * from however someone might type them. */
static void
test_hidden_recovery_keys (void)
{
  JrCases c = { .table = "hidden: recovery keys" };
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (int n = 0; n < 2000; n++)
    {
      char *rk = jr_recovery_key_new ();
      gboolean shape = strlen (rk) == 29;
      for (int i = 0; shape && i < 29; i++)
        shape = i % 5 == 4 ? rk[i] == '-' : strchr ("0123456789ABCDEFGHJKMNPQRSTVWXYZ", rk[i]) != NULL;
      jr_case (&c, "shape", shape, "Input new key  Expected XXXX-XXXX-XXXX-XXXX-XXXX-XXXX  Got \"%s\"", rk);
      jr_case (&c, "unique", g_hash_table_add (seen, g_strdup (rk)), "Input new key  Expected never seen  Got \"%s\" again", rk);

      char norm[JR_RECOVERY_NORM_LEN], messy_norm[JR_RECOVERY_NORM_LEN];
      jr_recovery_key_normalize (rk, norm);
      GString *messy = g_string_new (NULL);
      for (const char *p = rk; *p; p++)
        {
          if (*p == '-')
            {
              g_string_append (messy, g_test_rand_bit () ? " " : g_test_rand_bit () ? "" : "-");
              continue;
            }
          char ch = *p == '0' && g_test_rand_bit () ? 'O' : *p == '1' && g_test_rand_bit () ? "IL"[g_test_rand_bit ()] : *p;
          g_string_append_c (messy, g_test_rand_bit () ? g_ascii_tolower (ch) : ch);
        }
      gboolean ok = jr_recovery_key_normalize (messy->str, messy_norm);
      jr_case (&c, "typed another way", ok && strcmp (norm, messy_norm) == 0, "Input \"%s\" for %s  Expected %s  Got %s",
               messy->str, rk, norm, ok ? messy_norm : "refused");
      g_string_free (messy, TRUE);
      jr_secret_free (rk);
    }
  g_hash_table_unref (seen);
  jr_cases_done (&c);
}

/* Hidden test: random text of random length seals and opens; one flipped
 * bit anywhere is refused. */
static void
test_hidden_seal (void)
{
  JrCases c = { .table = "hidden: seal" };
  JrKey *key = jr_key_generate ();
  for (int n = 0; n < 2000; n++)
    {
      gsize len = (gsize) g_test_rand_int_range (0, 3000), sealed_len = 0;
      char *text = g_malloc (len + 1);
      for (gsize i = 0; i < len; i++)
        text[i] = (char) g_test_rand_int_range (1, 256);
      text[len] = '\0';
      guint8 *blob = jr_seal (key, text, len, &sealed_len);
      char *back = jr_unseal (key, blob, sealed_len);
      jr_case (&c, "round trip", back != NULL && memcmp (back, text, len + 1) == 0,
               "Input %" G_GSIZE_FORMAT " random bytes  Expected the same back  Got %s", len, back ? "other bytes" : "refused");
      jr_secret_free (back);
      gsize at = (gsize) g_test_rand_int_range (0, (gint32) sealed_len);
      blob[at] ^= (guint8) (1u << g_test_rand_int_range (0, 8));
      back = jr_unseal (key, blob, sealed_len);
      jr_case (&c, "one bit flipped", back == NULL,
               "Input %" G_GSIZE_FORMAT " bytes, byte %" G_GSIZE_FORMAT " changed  Expected refused  Got text", len, at);
      jr_secret_free (back);
      g_free (blob);
      g_free (text);
    }
  jr_cases_done (&c);
  jr_key_free (key);
}

/* Max size: far past every limit; refused or handled, never a crash. */
static void
test_huge_input (void)
{
  JrCases c = { .table = "huge input" };
  g_autofree char *letters = g_strnfill (100000, 'a');
  char *norm = jr_lock_secret_normalize (JR_LOCK_PASSPHRASE, letters);
  jr_case (&c, "100,000-byte passphrase", norm == NULL, "Expected refused  Got allowed");
  jr_secret_free (norm);
  norm = jr_lock_secret_normalize (JR_LOCK_PIN, letters);
  jr_case (&c, "100,000-byte PIN", norm == NULL, "Expected refused  Got allowed");
  jr_secret_free (norm);

  char out[JR_RECOVERY_NORM_LEN];
  g_autofree char *dashes = g_strnfill (100000, '-');
  g_autofree char *padded = g_strconcat (dashes, "0123456789ABCDEFGHJKMNPQ", dashes, NULL);
  jr_case (&c, "recovery key in 200,000 dashes", jr_recovery_key_normalize (padded, out) && strcmp (out, "0123456789ABCDEFGHJKMNPQ") == 0,
           "Expected read  Got refused");
  g_autofree char *symbols = g_strnfill (100000, 'A');
  jr_case (&c, "100,000 symbols", !jr_recovery_key_normalize (symbols, out), "Expected refused  Got read");

  JrKey *key = jr_key_generate ();
  gsize len = 10 * 1024 * 1024, sealed = 0;
  char *big = g_malloc (len + 1);
  memset (big, 'x', len);
  big[len] = '\0';
  guint8 *blob = jr_seal (key, big, len, &sealed);
  char *back = jr_unseal (key, blob, sealed);
  jr_case (&c, "10 MB entry", back != NULL && strlen (back) == len && memcmp (back, big, len) == 0,
           "Expected the same 10 MB back  Got %s", back ? "other text" : "refused");
  jr_secret_free (back);
  g_free (blob);
  g_free (big);
  jr_key_free (key);
  jr_cases_done (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_true (jr_crypto_init ());
  g_test_add_func ("/vault/pin-rules", test_pin_rules);
  g_test_add_func ("/vault/passphrase-rules", test_passphrase_rules);
  g_test_add_func ("/vault/recovery-key-rules", test_recovery_key_rules);
  g_test_add_func ("/vault/wrapped-form", test_wrapped_form);
  g_test_add_func ("/vault/device-bound", test_device_bound);
  g_test_add_func ("/vault/seal", test_seal);
  g_test_add_func ("/vault/key-basics", test_key_basics);
  g_test_add_func ("/vault/hidden/wrap", test_hidden_wrap);
  g_test_add_func ("/vault/hidden/recovery-keys", test_hidden_recovery_keys);
  g_test_add_func ("/vault/hidden/seal", test_hidden_seal);
  g_test_add_func ("/vault/huge-input", test_huge_input);
  return g_test_run ();
}
