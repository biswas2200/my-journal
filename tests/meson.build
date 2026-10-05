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
