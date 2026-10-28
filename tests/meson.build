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

# The network jail. Its own suite: Valgrind cannot run seccomp filters.
exe = executable('test_netjail', 'test_netjail.c', dependencies : core_dep)
test('netjail', isolated, args : [exe], env : test_env, suite : 'sandbox')
exe = executable('test_programs', 'test_programs.c', dependencies : programs_dep)
test('programs', isolated, args : [exe], env : test_env, suite : 'core')

# Drives the real window on a private headless display (never on the
# desktop); see run-headless.sh. Skipped if no headless display exists.
ui_test = executable('test_ui', 'test_ui.c', dependencies : [ui_dep, programs_dep])
test('ui', find_program('run-headless.sh'), args : [ui_test],
     env : test_env, suite : 'ui', timeout : 120)

# The real Journal, started off-screen: memory protected, jailed, clean exit.
test('hardened', find_program('run-headless.sh'),
     args : [files('check-hardened.sh'), diary_exe],
     suite : 'ui', timeout : 60)

# The start screen hands over between stand-in programs as agreed.
test('launcher', find_program('run-headless.sh'),
     args : [files('check-launcher.sh'), launcher_exe],
     suite : 'ui', timeout : 120)
