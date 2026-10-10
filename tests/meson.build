# Unit tests. Run under sanitizers with:
#   meson setup build-asan -Db_sanitize=address,undefined -Dbuildtype=debug
# and under Valgrind with:
#   JR_TEST_WRAPPER='valgrind --leak-check=full --error-exitcode=1' meson test -C build --suite core
test_env = environment()
test_env.set('G_DEBUG', 'fatal-warnings')
test_env.set('G_SLICE', 'always-malloc')

isolated = find_program('run-isolated.sh')

foreach name : ['jdate', 'active_time', 'shade', 'lockout', 'db', 'vault', 'journal', 'text', 'harden', 'crash']
  exe = executable('test_' + name, 'test_' + name + '.c', dependencies : core_dep)
  # run-isolated.sh: test files live in a private TMPDIR removed afterwards.
  test(name, isolated, args : [exe], env : test_env, suite : 'core')
endforeach

# Drives the real window on a private headless display (never on the
# desktop); see run-headless.sh. Skipped if no headless display exists.
ui_test = executable('test_ui', 'test_ui.c', dependencies : ui_dep)
test('ui', find_program('run-headless.sh'), args : [ui_test],
     env : test_env, suite : 'ui', timeout : 120)

# The real program, started off-screen: memory protected, clean exit.
test('hardened', find_program('run-headless.sh'),
     args : [find_program('check-hardened.sh'), journal_exe],
     suite : 'ui', timeout : 60)
