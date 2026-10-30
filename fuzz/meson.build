# Fuzz targets (see README.md). With -Dfuzz=true (clang) each is a
# libFuzzer program; otherwise each replays its saved inputs (seeds and
# every crash found so far) as a normal test, under Valgrind and the
# sanitizers too. Notes targets link only Notes' own code.
targets = {
  'date' : core_dep, 'text' : core_dep, 'lockout' : core_dep, 'secrets' : core_dep, 'journal_file' : core_dep,
  'markdown' : notes_core_dep, 'svgsafe' : notes_core_dep, 'noteline' : notes_core_dep,
  'textify' : notes_ai_dep, 'aiproto' : notes_ai_dep,
}
foreach t, dep : targets
  if get_option('fuzz')
    executable('fuzz_' + t, 'fuzz_' + t + '.c',
      dependencies : dep,
      link_args : ['-fsanitize=fuzzer'],
    )
  else
    replay = executable('replay_' + t, ['fuzz_' + t + '.c', 'replay.c'],
      dependencies : dep,
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
