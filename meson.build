project('journal', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>= 1.0',
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

glib_dep = dependency('glib-2.0', version : '>= 2.76')
sqlite_dep = dependency('sqlite3', version : '>= 3.35')
sodium_dep = dependency('libsodium', version : '>= 1.0.18')

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
)
core_deps = [glib_dep, sqlite_dep, sodium_dep]
core_lib = static_library('journal-core', core_src, dependencies : core_deps)
core_dep = declare_dependency(
  link_with : core_lib,
  include_directories : core_inc,
  dependencies : core_deps,
)

# App
gtk_dep = dependency('gtk4', version : '>= 4.12')
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
  'src/ui/activity_view.c',
  'src/ui/bar_chart.c',
  'src/ui/security_view.c',
  'src/ui/secret_dialog.c',
  'src/ui/sleep_watch.c',
  'src/ui/widgets.c',
)
ui_lib = static_library('journal-ui', ui_src, resources[1],
  dependencies : [core_dep, gtk_dep],
)
ui_dep = declare_dependency(
  link_with : ui_lib,
  sources : resources,  # compiled into each program so they always register
  include_directories : include_directories('src/ui'),
  dependencies : [core_dep, gtk_dep],
)
journal_exe = executable('journal', 'src/main.c',
  dependencies : ui_dep,
  install : true,
)

install_data('data/io.github.biswas2200.Journal.desktop',
  install_dir : get_option('datadir') / 'applications')
install_data('data/icons/hicolor/scalable/apps/io.github.biswas2200.Journal.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# Refresh the icon and app-menu caches after install when the tools exist;
# they are not needed to build or test.
gnome.post_install(
  gtk_update_icon_cache : find_program('gtk4-update-icon-cache', required : false).found(),
  update_desktop_database : find_program('update-desktop-database', required : false).found(),
)

subdir('tests')
