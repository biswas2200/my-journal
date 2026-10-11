# Fuzzing

A fuzzer feeds a function millions of generated inputs, keeps the ones
that reach new code, and changes those further. It looks for crashes,
memory errors (AddressSanitizer), undefined behaviour (UBSan), leaks,
hangs and broken promises (`FUZZ_CHECK`).

Every target reads input that does not come from this program: text the
user types, or a journal file that may be damaged.

| Target | Input | Checked beyond "no crash" |
| --- | --- | --- |
| `date` | a typed date ("4 Oct", "2026-10-04", anything) | an accepted date is a real day, never after today; an ISO day prints back to the same text |
| `text` | any bytes, valid UTF-8 or not | words never more than bytes; a trailing space changes nothing; "x " in front adds one |
| `lockout` | the saved wrong-try state | in range; saves and reads back the same; never more than one 30 s wait |
| `secrets` | PINs, passphrases and recovery keys as typed; saved keys, wrapped keys and sealed entries | only one proper form comes out; a refused recovery key leaves nothing behind; forged entries never open |
| `journal_file` | a whole journal file | opens, shows, unlocks, changes and saves without crashing; the state it reports is possible; no NULL day or month reaches the screens |

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

- `journal_file`: a damaged index made the calendar query return a row
  with no day, so the day list was handed NULL (the month list would
  have crashed in `sscanf`). Rows without a day or month are now
  skipped; the input is `corpus/journal_file/crash-null-day-from-damaged-index.db`.
