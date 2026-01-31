desktop_file = i18n.merge_file(
  input: 'net.skilpa.gnalk.desktop.in',
  output: 'net.skilpa.gnalk.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'applications'
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('Validate desktop file', desktop_utils, args: [desktop_file])
endif

appstream_file = i18n.merge_file(
  input: 'net.skilpa.gnalk.metainfo.xml.in',
  output: 'net.skilpa.gnalk.metainfo.xml',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'metainfo'
)

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('Validate appstream file', appstreamcli,
    args: ['validate', '--no-net', '--explain', appstream_file])
endif

install_data('net.skilpa.gnalk.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas'
)

compile_schemas = find_program('glib-compile-schemas', required: false)
if compile_schemas.found()
  test('Validate schema file',
    compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()])
endif

# D-Bus service file for background service mode
service_conf = configuration_data()
service_conf.set('APPLICATION_ID', 'net.skilpa.gnalk')
service_conf.set('bindir', get_option('prefix') / get_option('bindir'))

configure_file(
  input: 'net.skilpa.gnalk.service.in',
  output: 'net.skilpa.gnalk.service',
  configuration: service_conf,
  install: true,
  install_dir: get_option('datadir') / 'dbus-1' / 'services'
)

# Autostart desktop file for service mode at login
autostart_file = i18n.merge_file(
  input: 'net.skilpa.gnalk-autostart.desktop.in',
  output: 'net.skilpa.gnalk-autostart.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: get_option('sysconfdir') / 'xdg' / 'autostart'
)

subdir('icons')
