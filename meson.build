project(
  'vs-logger',
  ['cpp'],
  version: '0.2.2',
  meson_version: '>= 1.1',
  default_options: ['cpp_std=c++23'],
)

crow_dep = dependency('Crow')

log_lib = library(
    'vs-log',
    [
      'lib/logger.cpp',
    ],
    install: true,
    dependencies: [],
    include_directories: ['include'],
)

log_lib_dep = declare_dependency(
  link_with: log_lib,
  include_directories: ['include'],
)

install_subdir('include/vs-logger', install_dir : 'include/vs-logger', strip_directory: false )

executable(
    'example',
    'example.cpp',
    install: false,
    cpp_args: [],
    link_args: [],
    dependencies: [
        log_lib_dep
    ],
    install: false
)

pconf = import('pkgconfig')
pconf.generate(
  log_lib,
  description: 'Logging library exposing entries on websocket endpoint',
  url: 'https://github.com/lazy-eggplant/vs-logger',
  version: meson.project_version(),
)