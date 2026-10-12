project('dcord', ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'c_std=c11', 'buildtype=release',
                    'warning_level=2', 'b_lto=true', 'b_ndebug=if-release'])

subdir('third_party')

gtk_dep = dependency('gtk+-3.0')
deps = [
  gtk_dep,
  dependency('libcurl'),
  dependency('zlib'),
  dependency('libcrypto'),
  dependency('opus'),
  dependency('libpipewire-0.3'),
  dependency('libavformat'),
  dependency('libavcodec'),
  dependency('libavutil'),
  dependency('libswscale'),
  dependency('x11'),
  dependency('xi'),
  dependency('threads'),
  dave_dep,
  qrcodegen_dep,
]

core_src = files(
  'src/util.cpp', 'src/http.cpp', 'src/ws.cpp', 'src/props.cpp', 'src/gateway.cpp',
  'src/store.cpp', 'src/api.cpp', 'src/remote_auth.cpp', 'src/markdown.cpp', 'src/media.cpp',
  'src/audio.cpp', 'src/voice.cpp', 'src/app.cpp',
)
core_lib = static_library('dcordcore', core_src,
  include_directories: json_inc, dependencies: deps)
core_dep = declare_dependency(link_with: core_lib, include_directories: [json_inc, include_directories('src')],
  dependencies: deps)

executable('dcord', 'src/main.cpp', 'src/ui_main.cpp', 'src/ui_chat.cpp', 'src/ui_login.cpp', 'src/ui_extra.cpp', 'src/debug.cpp',
  dependencies: core_dep, install: true)
executable('dcord-probe', 'tests/probe.cpp', 'src/ui_stub.cpp', dependencies: core_dep)

test_md = executable('test_markdown', 'tests/test_markdown.cpp', 'src/markdown.cpp', 'src/util.cpp',
  include_directories: [json_inc, include_directories('src')], dependencies: [dependency('glib-2.0')])
test('markdown', test_md)
executable('xshot', 'tests/xshot.c', dependencies: dependency('x11'))
executable('xunfocus', 'tests/xunfocus.c', dependencies: dependency('x11'))
executable('voice_echo', 'tests/voice_echo.c', dependencies: dependency('libcrypto'))
executable('dave_sender', 'tests/dave_sender.cpp', 'tests/external_sender.cpp',
  include_directories: [mlspp_inc, include_directories('third_party/libdave/includes', 'third_party/libdave/src')],
  cpp_args: tp_args, link_with: [dave_lib, mlspp_lib], dependencies: crypto_dep)
test('voice', find_program('tests/voice_test.sh'), args: meson.current_build_dir(), timeout: 120, is_parallel: false)
