project('camweave-camera', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])

gtk = dependency('gtk4', version: '>=4.6')
gst = dependency('gstreamer-1.0', version: '>=1.20')
gstapp = dependency('gstreamer-app-1.0', version: '>=1.20')
data_dir = join_paths(get_option('prefix'), get_option('datadir'), 'camweave')
executable('camweave-camera', 'src/main.c', dependencies: [gtk, gst, gstapp],
  c_args: ['-DAPP_DATA_DIR="' + data_dir + '"'], install: true)
install_data('web/watch.html', install_dir: data_dir)
