project('journal', 'c',
  version : '1.0.0',
  license : 'MIT',
  license_files : 'LICENSE',
  meson_version : '>= 1.1',
  default_options : [
    'c_std=c11',
    'warning_level=2',   # -Wall -Wextra
    'werror=true',       # -Werror
    'buildtype=release',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(
  '-D_GNU_SOURCE',
  '-DG_LOG_DOMAIN="journal"',
  language : 'c',
)
if get_option('fuzz')
  if cc.get_id() != 'clang'
    error('-Dfuzz=true needs clang: CC=clang meson setup build-fuzz -Dfuzz=true')
  endif
  # libFuzzer is guided by coverage of all our code, not just the targets;
  # undefined behaviour stops the run like a crash.
  add_project_arguments('-fsanitize=fuzzer-no-link', '-fno-sanitize-recover=all', language : 'c')
endif

glib_dep = dependency('glib-2.0', version : '>= 2.76')
sqlite_dep = dependency('sqlite3', version : '>= 3.35')
sodium_dep = dependency('libsodium', version : '>= 1.0.18')
# The Journal's no-network filter (see src/core/netjail.h).
seccomp_dep = dependency('libseccomp', version : '>= 2.5')

# Core: plain logic with no UI, so every rule can be unit tested.
core_inc = include_directories('src/core')
core_src = files(
  'src/core/jdate.c',
  'src/core/active_time.c',
  'src/core/shade.c',
  'src/core/lockout.c',
  'src/core/db.c',
  'src/core/vault.c',
  'src/core/journal.c',
  'src/core/text.c',
  'src/core/harden.c',
  'src/core/netjail.c',
)
core_deps = [glib_dep, sqlite_dep, sodium_dep, seccomp_dep]
core_lib = static_library('journal-core', core_src, dependencies : core_deps)
core_dep = declare_dependency(
  link_with : core_lib,
  include_directories : core_inc,
  dependencies : core_deps,
)

# How the launcher and its two programs hand over (exit codes only).
# Its own library: the launcher and Notes link this and nothing else of
# the Journal.
programs_lib = static_library('journal-programs', 'src/core/programs.c', dependencies : glib_dep)
programs_dep = declare_dependency(
  link_with : programs_lib,
  include_directories : core_inc,
  dependencies : glib_dep,
)

# Notes' own core: plain logic with no UI and nothing of the Journal.
notes_core_inc = include_directories('src/notes/core')
gio_dep = dependency('gio-2.0', version : '>= 2.76')
# A coverage build must still write its .gcda files from inside the
# Notes file lock; only such builds open those object folders.
notes_core_args = []
if get_option('b_coverage')
  notes_core_args += '-DJN_GCOV_ROOT="@0@"'.format(meson.project_build_root())
endif
notes_core_lib = static_library('notes-core',
  'src/notes/core/noteline.c',
  'src/notes/core/notesdb.c',
  'src/notes/core/notesjail.c',
  'src/notes/core/noteexport.c',
  'src/notes/core/markdown.c',
  'src/notes/core/svgsafe.c',
  c_args : notes_core_args,
  dependencies : [glib_dep, gio_dep, sqlite_dep],
)
notes_core_dep = declare_dependency(
  link_with : notes_core_lib,
  include_directories : notes_core_inc,
  dependencies : [glib_dep, gio_dep, sqlite_dep],
)

# The AI side of Notes: pure logic (no network, no UI), tested alone.
json_dep = dependency('json-glib-1.0', version : '>= 1.6')
soup_dep = dependency('libsoup-3.0', version : '>= 3.0')
notes_ai_lib = static_library('notes-ai',
  'src/notes/ai/textify.c',
  'src/notes/ai/netguard.c',
  'src/notes/ai/aiproto.c',
  dependencies : [glib_dep, gio_dep, json_dep],
)
notes_ai_dep = declare_dependency(
  link_with : notes_ai_lib,
  include_directories : include_directories('src/notes/ai'),
  dependencies : [glib_dep, gio_dep, json_dep],
)
# Fetching Sources (network): only the AI helper and its tests.
notes_fetch_lib = static_library('notes-fetch', 'src/notes/ai/fetch.c',
  dependencies : [notes_ai_dep, soup_dep],
)
notes_fetch_dep = declare_dependency(
  link_with : notes_fetch_lib,
  dependencies : [notes_ai_dep, soup_dep],
)
# Notes asking the helper (token from your sign-in, then journal-ai).
notes_client_lib = static_library('notes-client', 'src/notes/ai/client.c',
  dependencies : [notes_ai_dep, soup_dep],
)
notes_client_dep = declare_dependency(
  link_with : notes_client_lib,
  dependencies : [notes_ai_dep, soup_dep],
)

# App
gtk_dep = dependency('gtk4', version : '>= 4.12')
# The login keyring, which holds the secret a PIN needs (see keyring.h).
secret_dep = dependency('libsecret-1', version : '>= 0.20')
gnome = import('gnome')
resources = gnome.compile_resources('journal-resources',
  'data/journal.gresource.xml',
  source_dir : 'data',
  c_name : 'journal',
)
ui_src = files(
  'src/ui/window.c',
  'src/ui/lock_view.c',
  'src/ui/day_view.c',
  'src/ui/day_popover.c',
  'src/ui/keyring.c',
  'src/ui/activity_view.c',
  'src/ui/bar_chart.c',
  'src/ui/security_view.c',
  'src/ui/secret_dialog.c',
  'src/ui/sleep_watch.c',
  'src/ui/widgets.c',
)
ui_lib = static_library('journal-ui', ui_src, resources[1],
  dependencies : [core_dep, gtk_dep, secret_dep],
)
ui_dep = declare_dependency(
  link_with : ui_lib,
  sources : resources,  # compiled into each program so they always register
  include_directories : include_directories('src/ui'),
  dependencies : [core_dep, gtk_dep, secret_dep],
)
# Three programs. `journal` (the start screen) runs the other two, which
# live in libexec: the Journal in its network jail, and Notes.
program_dir = get_option('prefix') / get_option('libexecdir')
diary_exe = executable('journal-diary', 'src/diary/main.c',
  dependencies : [ui_dep, programs_dep],
  install : true,
  install_dir : get_option('libexecdir'),
)
notes_resources = gnome.compile_resources('notes-resources',
  'data/notes/notes.gresource.xml',
  source_dir : 'data/notes',
  c_name : 'notes',
)
rsvg_dep = dependency('librsvg-2.0', version : '>= 2.52')
notes_ui_lib = static_library('notes-ui',
  'src/notes/ui/editor.c',
  'src/notes/ui/md_view.c',
  'src/notes/ui/notes_window.c',
  notes_resources[1],
  dependencies : [notes_core_dep, notes_client_dep, gtk_dep, rsvg_dep, programs_dep],
)
notes_ui_dep = declare_dependency(
  link_with : notes_ui_lib,
  sources : notes_resources,
  include_directories : include_directories('src/notes/ui'),
  dependencies : [notes_core_dep, notes_client_dep, gtk_dep, rsvg_dep, programs_dep],
)
notes_exe = executable('journal-notes', 'src/notes/main.c',
  dependencies : notes_ui_dep,
  c_args : '-DJN_HELPER_PATH="@0@"'.format(program_dir / 'journal-ai'),
  install : true,
  install_dir : get_option('libexecdir'),
)
# Notes' AI helper: one locked-down run per "Enhance".
ai_exe = executable('journal-ai', 'src/notes/ai/helper.c',
  dependencies : [notes_fetch_dep, notes_core_dep],
  install : true,
  install_dir : get_option('libexecdir'),
)
launcher_exe = executable('journal', 'src/launcher/main.c',
  dependencies : [gtk_dep, programs_dep],
  c_args : '-DJR_PROGRAM_DIR="@0@"'.format(program_dir),
  install : true,
)

install_data('data/io.github.biswas2200.Journal.desktop',
  'data/io.github.biswas2200.Journal.Diary.desktop',
  'data/io.github.biswas2200.Journal.Notes.desktop',
  install_dir : get_option('datadir') / 'applications')
# Second network lock for the Journal; tools/install-apparmor.sh loads it.
configure_file(input : 'data/apparmor/journal-diary.in', output : 'journal-diary.apparmor',
  configuration : { 'DIARY' : program_dir / 'journal-diary' },
  install : true, install_dir : get_option('datadir') / 'journal' / 'apparmor')
install_data('data/icons/hicolor/scalable/apps/io.github.biswas2200.Journal.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# Refresh the icon and app-menu caches after install when the tools exist;
# they are not needed to build or test.
gnome.post_install(
  gtk_update_icon_cache : find_program('gtk4-update-icon-cache', required : false).found(),
  update_desktop_database : find_program('update-desktop-database', required : false).found(),
)

subdir('tests')
subdir('fuzz')
