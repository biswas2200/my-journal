# Unit tests. Run under sanitizers with:
#   meson setup build-asan -Db_sanitize=address,undefined -Dbuildtype=debug
# and under Valgrind with:
#   meson test -C build --wrap='valgrind --leak-check=full --error-exitcode=1'
test_env = environment()
test_env.set('G_DEBUG', 'fatal-warnings')
test_env.set('G_SLICE', 'always-malloc')

foreach name : ['jdate', 'active_time', 'shade', 'lockout', 'db', 'vault', 'journal', 'text']
  exe = executable('test_' + name, 'test_' + name + '.c', dependencies : core_dep)
  test(name, exe, env : test_env, suite : 'core')
endforeach

# Drives the real window on a private headless display (never on the
# desktop); see run-headless.sh. Skipped if no headless display exists.
ui_test = executable('test_ui', 'test_ui.c', dependencies : ui_dep)
test('ui', find_program('run-headless.sh'), args : [ui_test],
     env : test_env, suite : 'ui', timeout : 120)
