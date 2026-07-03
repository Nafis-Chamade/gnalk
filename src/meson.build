# Source files organized by category

network_sources = [
  'network/gnalk-http-client.c',
  'network/gnalk-signaling.c',
  'network/gnalk-webrtc.c',
  'network/gnalk-push-client.c',
  'network/gnalk-network-monitor.c',
  'network/gnalk-sdp-utils.c',
  'network/gnalk-login-flow-v2.c',
]

call_sources = [
  'call/gnalk-call-window.c',
  'call/gnalk-call-preview-manager.c',
  'call/gnalk-call-layout.c',
  'call/gnalk-call-control-bar.c',
  'call/gnalk-call-screen-share.c',
  'call/gnalk-call-participant-manager.c',
  'call/gnalk-call-signaling-manager.c',
  'call/gnalk-media-pipeline.c',
  'call/gnalk-receive-pipeline.c',
  'call/gnalk-quality-controller.c',
  'call/gnalk-data-channel.c',
  'call/gnalk-screen-capture.c',
  'call/gnalk-camera-utils.c',
  'call/gnalk-camera-preview.c',
  'call/gnalk-participant-tile.c',
]

data_sources = [
  'data/gnalk-database.c',
  'data/gnalk-conversation-data.c',
  'data/gnalk-message-data.c',
  'data/gnalk-thread-data.c',
  'data/gnalk-account.c',
  'data/gnalk-account-manager.c',
  'data/gnalk-data-repository.c',
]

controllers_sources = [
  'controllers/gnalk-message-controller.c',
  'controllers/gnalk-conversation-controller.c',
  'controllers/gnalk-participant-controller.c',
  'controllers/gnalk-room-controller.c',
  'controllers/gnalk-message-processor.c',
  'controllers/gnalk-app-state.c',
  'controllers/gnalk-sync-manager.c',
  'controllers/gnalk-sync-coordinator.c',
]

utils_sources = [
  'utils/gnalk-idle-utils.c',
  'utils/gnalk-log.c',
  'utils/gnalk-keyring.c',
  'utils/gnalk-avatar-cache.c',
  'utils/gnalk-markdown.c',
  'utils/gnalk-goa.c',
  'utils/gnalk-background-generator.c',
  'utils/gnalk-async-op.c',
  'utils/gnalk-system-message.c',
  'utils/gnalk-message-utils.c',
]

ui_sources = [
  'ui/gnalk-window.c',
  'ui/gnalk-window-login.c',
  'ui/gnalk-window-read-status.c',
  'ui/gnalk-window-message-composer.c',
  'ui/gnalk-window-conversation-list.c',
  'ui/gnalk-window-message-display.c',
  'ui/gnalk-window-room-switch.c',
  'ui/gnalk-window-account-session.c',
  'ui/gnalk-window-file-handling.c',
  'ui/gnalk-window-message-actions.c',
  'ui/gnalk-room-tab-manager.c',
  'ui/gnalk-conversation-row.c',
  'ui/gnalk-conversation-sidebar.c',
  'ui/gnalk-conversation-header.c',
  'ui/gnalk-conversation-actions.c',
  'ui/gnalk-conversation-info-dialog.c',
  'ui/gnalk-participant-profile-dialog.c',
  'ui/gnalk-new-conversation-dialog.c',
  'ui/gnalk-message-row.c',
  'ui/gnalk-message-view.c',
  'ui/gnalk-message-input.c',
  'ui/gnalk-login-view.c',
  'ui/gnalk-preferences-dialog.c',
  'ui/gnalk-typing-indicator.c',
  'ui/gnalk-network-status-bar.c',
  'ui/gnalk-user-status-menu.c',
  'ui/gnalk-participants-list.c',
  'ui/gnalk-incoming-call-banner.c',
  'ui/gnalk-image-gallery.c',
  'ui/gnalk-audio-player.c',
  'ui/gnalk-waveform-seekbar.c',
  'ui/gnalk-voice-recorder.c',
  'ui/gnalk-recording-overlay.c',
  'ui/gnalk-account-switcher.c',
  'ui/gnalk-date-separator.c',
  'ui/gnalk-quote-widget.c',
  'ui/gnalk-upload-preview.c',
  'ui/gnalk-file-preview.c',
  'ui/gnalk-media-capture-dialog.c',
  'ui/gnalk-message-search.c',
  'ui/gnalk-video-player.c',
  'ui/gnalk-video-player-dialog.c',
  'ui/gnalk-video-thumbnail.c',
  'ui/gnalk-thread-row.c',
]

core_sources = [
  'main.c',
  'gnalk-application.c',
  'gnalk-session.c',
  'gnalk-session-manager.c',
  'gnalk-notification-service.c',
]

gnalk_sources = core_sources + network_sources + call_sources + data_sources + controllers_sources + utils_sources + ui_sources

# Include directories for headers in subdirectories
gnalk_inc = include_directories('.', 'network', 'call', 'data', 'controllers', 'utils', 'ui')

gnalk_deps = [
  dependency('gtk4', version: '>= 4.10'),
  dependency('libadwaita-1', version: '>= 1.4'),
  dependency('libsoup-3.0', version: '>= 3.0'),
  dependency('json-glib-1.0'),
  dependency('sqlite3'),
]

libsecret_dep = dependency('libsecret-1', required: false)
if libsecret_dep.found()
  gnalk_deps += libsecret_dep
  add_project_arguments('-DHAVE_LIBSECRET=1', language: 'c')
endif

gst_dep = dependency('gstreamer-1.0', required: false)
gst_video_dep = dependency('gstreamer-video-1.0', required: false)
gst_app_dep = dependency('gstreamer-app-1.0', required: false)
gst_sdp_dep = dependency('gstreamer-sdp-1.0', required: false)
gst_webrtc_dep = dependency('gstreamer-webrtc-1.0', required: false)
gst_rtp_dep = dependency('gstreamer-rtp-1.0', required: false)

if gst_dep.found()
  gnalk_deps += gst_dep
  add_project_arguments('-DHAVE_GSTREAMER=1', language: 'c')
endif

if gst_video_dep.found()
  gnalk_deps += gst_video_dep
endif

if gst_app_dep.found()
  gnalk_deps += gst_app_dep
endif

if gst_sdp_dep.found()
  gnalk_deps += gst_sdp_dep
endif

if gst_webrtc_dep.found()
  gnalk_deps += gst_webrtc_dep
  add_project_arguments('-DHAVE_GSTREAMER_WEBRTC=1', language: 'c')
endif

if gst_rtp_dep.found()
  gnalk_deps += gst_rtp_dep
endif

libportal_dep = dependency('libportal-gtk4', version: '>= 0.6', required: false)
if libportal_dep.found()
  gnalk_deps += libportal_dep
  add_project_arguments('-DHAVE_LIBPORTAL=1', language: 'c')
endif

goa_dep = dependency('goa-1.0', required: false)
if goa_dep.found()
  gnalk_deps += goa_dep
  add_project_arguments('-DHAVE_GOA=1', language: 'c')
endif

gnalk_resources = gnome.compile_resources('gnalk-resources',
  'gnalk.gresource.xml',
  source_dir: ['../ui'],
  c_name: 'gnalk'
)

executable('gnalk', gnalk_sources, gnalk_resources,
  include_directories: gnalk_inc,
  dependencies: gnalk_deps,
  install: true,
)
