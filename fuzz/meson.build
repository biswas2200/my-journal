# Fuzz targets (see README.md). With -Dfuzz=true (clang) each is a
# libFuzzer program; otherwise each replays its saved inputs (seeds and
# every crash found so far) as a normal test, under Valgrind and the
# sanitizers too.
foreach t : ['date', 'text', 'lockout', 'secrets', 'journal_file']
  if get_option('fuzz')
    executable('fuzz_' + t, 'fuzz_' + t + '.c',
      dependencies : core_dep,
      link_args : ['-fsanitize=fuzzer'],
    )
  else
    replay = executable('replay_' + t, ['fuzz_' + t + '.c', 'replay.c'],
      dependencies : core_dep,
    )
    test('fuzz-' + t, isolated,
      args : [replay, meson.current_source_dir() / 'corpus' / t],
      env : test_env,
      suite : ['core', 'fuzz'],
    )
  endif
endforeach

# Writes the seed journals in corpus/journal_file (run by hand).
executable('make_seeds', 'make_seeds.c', dependencies : core_dep)
