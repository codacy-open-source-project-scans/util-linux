python_module = import('python')

python = python_module.find_installation(
    get_option('python'),
    required : get_option('build-python'),
    disabler : true)
build_python = python.found()

pylibmount_sources = '''
  pylibmount.c
  pylibmount.h
  fs.c
  tab.c
'''.split()

if LINUX
  pylibmount_sources += 'context.c'
endif

if build_python
  python.extension_module(
    'pylibmount',
    pylibmount_sources,
    include_directories : [dir_include, dir_libmount],
    subdir : 'libmount',
    link_with : lib_mount,
    dependencies : python.dependency(),
    c_args : [
      '-Wno-cast-function-type',

      # https://github.com/util-linux/util-linux/issues/2366
      python.language_version().version_compare('>=3.12') ?
        [ '-Wno-error=redundant-decls' ] : [],
    ],
    install : true)

  python.install_sources(
    '__init__.py',
    subdir : 'libmount',
    pure : false)
endif
