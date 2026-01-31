project('gnalk', 'c',
  version: '0.1.0',
  meson_version: '>= 0.59.0',
  default_options: [
    'warning_level=2',
    'werror=false',
    'c_std=gnu11',
  ],
)

i18n = import('i18n')
gnome = import('gnome')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'gnalk')
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
config_h.set_quoted('APPLICATION_ID', 'net.skilpa.gnalk')

configure_file(
  output: 'gnalk-config.h',
  configuration: config_h,
)

add_project_arguments([
  '-I' + meson.project_build_root(),
  '-DGETTEXT_PACKAGE="gnalk"',
], language: 'c')

subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
