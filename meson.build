project(
    'form-server',
    'c',
    default_options: {
        'c_std': 'c23',
        'buildtype': 'debugoptimized',
        'warning_level': '3',
        'b_ndebug': 'if-release',
    },
)

executable(
    'form-server',
    'src/main.c',
    c_args: [
        '-DVERSION="@0@"'.format(meson.project_version()),
        '-Wno-empty-translation-unit',
        '-Wno-pragma-once-outside-header',
    ],
    dependencies: [
        dependency('libevent'),
        dependency('libev'),
        dependency('libuv'),
    ],
    install: true,
)
