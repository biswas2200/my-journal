# Fuzzing

A fuzzer feeds a function millions of generated inputs, keeps the ones
that reach new code, and changes those further. It looks for crashes,
memory errors (AddressSanitizer), undefined behaviour (UBSan), leaks,
hangs and broken promises (`FUZZ_CHECK`).

Every target reads input that does not come from this program: text the
user types, a journal file that may be damaged, or, on the Notes side,
what comes from the internet: web pages, Gemini's answers, and the
Markdown and SVG diagrams the AI writes after reading those pages.

| Target | Input | Checked beyond "no crash" |
| --- | --- | --- |
| `date` | a typed date ("4 Oct", "2026-10-04", anything) | an accepted date is a real day, never after today; an ISO day prints back to the same text |
| `text` | any bytes, valid UTF-8 or not | words never more than bytes; a trailing space changes nothing; "x " in front adds one |
| `lockout` | the saved wrong-try state | in range; saves and reads back the same; never more than one 30 s wait |
| `secrets` | PINs, passphrases and recovery keys as typed; saved keys, wrapped keys and sealed entries | only one proper form comes out; a refused recovery key leaves nothing behind; forged entries never open |
| `journal_file` | a whole journal file | opens, shows, unlocks, changes and saves without crashing; the state it reports is possible; no NULL day or month reaches the screens |
| `markdown` | the AI's enhanced copy, a topic reference | sane kinds and levels; no empty span; only http(s) links; citations are only numbers; all text valid UTF-8; a line without markup shown as written |
| `svgsafe` | the AI's SVG diagram | the result is read again by separate rules: `<svg>` root of size 4000 at most; no script, style, link, image, `<use>` or embedded document; no handler, href or style attribute; only `url(#id)`; cleaning it again changes nothing |
| `textify` | a fetched web page (any bytes) and a length limit | valid UTF-8, no control characters but newline and tab; a limit gives the start of the same text, cut on a character boundary |
| `aiproto` | Gemini's answer, the sign-in token answer, the ADC file, the messages between Notes and its AI helper | text that comes out is valid UTF-8; errors never hold the refresh token; a request or answer written and read again comes back the same |
| `noteline` | the lines of a stored note | any line stored as any kind reads back as that kind and text; any stored line reads as a kind and stays it |

## On GitHub

The `fuzz` job in `.github/workflows/ci.yml` runs each target for two
minutes on every push, with libFuzzer, AddressSanitizer and UBSan. The
inputs it finds are cached for the next run, so it goes deeper over
time. A crash fails the job; download the `fuzz-crash-<target>`
artifact to get the input.

## On every test run

Without clang, each target is linked to `replay.c` and runs as a normal
test (`meson test --suite fuzz`, also under Valgrind and the sanitizers)
over its folder in `corpus/`: the seeds and every input that ever found
a bug. After fixing a crash, put its input there so it can never come
back.

`replay.c` can also fuzz on its own, without coverage guidance:

```bash
build/fuzz/replay_date -mutate 1000000 fuzz/corpus/date
```

## With clang locally

```bash
sudo apt install clang
```

```bash
CC=clang meson setup build-fuzz -Dfuzz=true -Db_sanitize=address,undefined -Db_lundef=false -Dbuildtype=debugoptimized -Dwerror=false
```

```bash
meson compile -C build-fuzz && tools/fuzz.sh journal_file 600
```

## Seeds

`corpus/journal_file` and the sealed entry and wrapped keys in
`corpus/secrets` are written by `make_seeds` (no real journal is read):

```bash
build/fuzz/make_seeds fuzz/corpus/journal_file fuzz/corpus/secrets
```

The other seeds are small text files.

## Found so far

Notes (each input is in `corpus/<target>/` and a named case in the
unit tests):

- `textify`: read one byte past the end of every page, which comes
  from the network with no zero byte after it
  (`crash-overread-one-byte.bin`).
- `svgsafe`: only the first `url(` in a value was checked, so
  `fill="url(#g) url(https://…)"` kept a reference to another site
  (`crash-second-url-not-checked.svg`).
- `markdown`: a line ending in a broken UTF-8 character was read past
  its end; broken UTF-8 now becomes U+FFFD first
  (`crash-overread-broken-utf8.bin`).
- `aiproto`: json-glib's immutable parser aborts the whole program on
  some broken JSON (`null{…}`); a bad answer could have closed Notes.
  The normal parser is used now (`crash-json-glib-immutable-abort.json`).
- `svgsafe`, after five minutes: GMarkup reads on after the root
  element, so a second `<svg>` escaped the size limit; and an attribute
  given twice was written twice, which XML readers refuse. A second
  root is refused now, and a repeated attribute kept once
  (`crash-second-root-and-repeated-attributes.svg`).
- `markdown`, while writing these targets: a long line of unclosed
  marks (`[x](` 50,000 times) took 3.7 s to read, since each mark looks
  for its end to the end of the line; a paragraph over 16 KB is now
  shown unformatted, which takes milliseconds
  (`tests/test_markdown.c`, `/markdown/long-lines`).
- `markdown`, small: math of only braces (`$}}$`) came out as an empty
  span (`crash-empty-math-span.md`).

Journal:

- `journal_file`: a damaged index made the calendar query return a row
  with no day, so the day list was handed NULL (the month list would
  have crashed in `sscanf`). Rows without a day or month are now
  skipped; the input is `corpus/journal_file/crash-null-day-from-damaged-index.db`.
