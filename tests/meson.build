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

# Notes core. Runs under Valgrind with the Journal's core.
foreach name : ['report', 'openuri']
  exe = executable('test_' + name, 'test_' + name + '.c', dependencies : shared_dep)
  test(name, isolated, args : [exe], env : test_env, suite : 'core')
endforeach

foreach name : ['noteline', 'notesdb', 'noteexport', 'markdown', 'svgsafe', 'studyclock', 'notebackup', 'notes_crash']
  exe = executable('test_' + name, 'test_' + name + '.c', dependencies : notes_core_dep)
  test(name, isolated, args : [exe], env : test_env, suite : 'core')
endforeach

# Notes' AI side (pure parts).
foreach name : ['textify', 'netguard', 'aiproto']
  exe = executable('test_' + name, 'test_' + name + '.c', dependencies : notes_ai_dep)
  test(name, isolated, args : [exe], env : test_env, suite : 'core')
endforeach

# Fetching from a local web server (no internet needed).
exe = executable('test_fetch', 'test_fetch.c', dependencies : notes_fetch_dep)
test('fetch', isolated, args : [exe], env : test_env, suite : 'core', timeout : 60)

# The real AI helper against a fake Gemini on this machine.
exe = executable('test_driveproto', 'test_driveproto.c', dependencies : notes_drive_dep)
test('driveproto', isolated, args : [exe], env : test_env, suite : 'core')
exe = executable('test_oauth', 'test_oauth.c', dependencies : notes_drive_dep)
test('oauth', isolated, args : [exe], env : test_env, suite : 'core')

# The real Drive helper against a fake Google Drive on this machine.
exe = executable('test_drive_helper', 'test_drive_helper.c', 'fake_google.c', dependencies : [notes_drive_dep, soup_dep])
test('drive-helper', isolated, args : [exe, drive_exe], env : test_env, suite : 'sandbox', timeout : 60)
# Notes' Drive client: sign-in with a fake Google and a fake browser,
# backups through the real helper.
exe = executable('test_drive_client', 'test_drive_client.c', 'fake_google.c',
  dependencies : [notes_drive_client_dep, soup_dep])
test('drive-client', isolated, args : [exe, drive_exe], env : test_env, suite : 'sandbox', timeout : 60)

exe = executable('test_ai_helper', 'test_ai_helper.c', dependencies : [notes_ai_dep, soup_dep])
test('ai-helper', isolated, args : [exe, ai_exe], env : test_env, suite : 'sandbox', timeout : 60)

# Notes' AI client with fake Google services and the real helper.
exe = executable('test_ai_client', 'test_ai_client.c', dependencies : notes_client_dep)
test('ai-client', isolated, args : [exe, ai_exe], env : test_env, suite : 'sandbox', timeout : 60)

# The network jail. Its own suite: Valgrind cannot run seccomp filters.
exe = executable('test_netjail', 'test_netjail.c', dependencies : core_dep)
test('netjail', isolated, args : [exe], env : test_env, suite : 'sandbox')
# Notes' file lock (Landlock): the journal is out of its reach.
exe = executable('test_notesjail', 'test_notesjail.c', dependencies : notes_core_dep)
test('notesjail', isolated, args : [exe], env : test_env, suite : 'sandbox')
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

# `journal --version` answers without a display.
test('version', find_program('check-version.sh'), args : [launcher_exe, meson.project_version()],
     suite : 'core')

# The Notes editor and window, inside Notes' file lock.
notes_ui_test = executable('test_notes_ui', 'test_notes_ui.c', dependencies : notes_ui_dep)
test('notes-ui', find_program('run-headless.sh'), args : [notes_ui_test],
     env : test_env, suite : 'ui', timeout : 120)

# Years of notes at once (10 000 topics, 100 000 sessions, 1 MB notes),
# with what you feel timed against limits; its own suite, as it takes a
# while (meson test --suite load). JN_LOAD_SLACK=2 for slow machines.
notes_load_test = executable('test_notes_load', 'test_notes_load.c', dependencies : [notes_ui_dep, sqlite_dep])
test('notes-load', find_program('run-headless.sh'), args : [notes_load_test],
     env : test_env, suite : 'load', timeout : 600)

# The real Notes program, alone and under the real start screen. Not
# beside 'launcher': both start the real start screen on one test bus, and
# a second one hands over to the first and exits.
test('notes', find_program('run-headless.sh'),
     args : [files('check-notes.sh'), notes_exe, launcher_exe],
     suite : 'ui', timeout : 60, is_parallel : false)
