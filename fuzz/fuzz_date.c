/* Fuzz target: typed dates ("4 Oct", "2026-10-04", anything) and ISO
 * days read back from the file. An accepted date is a real day and never
 * after today; an accepted ISO day prints back to exactly its text. */
#include <string.h>
#include "fuzz.h"
#include "jdate.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  fuzz_setup ();
  g_autofree char *text = fuzz_string (data, size);
  /* A normal day, a leap day and the first day of a year. */
  static const JrDay todays[] = { { 2026, 10, 6 }, { 2028, 2, 29 }, { 2027, 1, 1 } };
  for (gsize i = 0; i < G_N_ELEMENTS (todays); i++)
    {
      JrDay d;
      if (jr_day_parse_input (text, todays[i], &d))
        {
          FUZZ_CHECK (jr_day_valid (d));
          FUZZ_CHECK (jr_day_compare (d, todays[i]) <= 0);
          char buf[64];
          jr_day_format_long (d, buf, sizeof buf);
          jr_day_format_short (d, buf, sizeof buf);
          FUZZ_CHECK (jr_weekday_name (d) != NULL);
        }
    }
  JrDay d;
  if (jr_day_from_iso (text, &d))
    {
      char iso[JR_ISO_LEN];
      FUZZ_CHECK (jr_day_valid (d));
      jr_day_to_iso (d, iso);
      FUZZ_CHECK (strcmp (iso, text) == 0);
    }
  return 0;
}
