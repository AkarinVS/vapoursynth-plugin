project('Plugin', 'cpp',
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++11'],
  meson_version: '>=0.48.0',
  version: '8'
)

version_h = declare_dependency(
    sources: vcs_tag(
        command: ['git', 'describe', '--tags', '--long', '--always'],
        input: 'version.h.in',
        output: 'version.h'
    )
)

sources = [
  # expr
  'expr/vslog.cpp',
  'expr/cpufeatures.cpp',
  'expr/kernel/cpulevel.cpp',
  'expr/exprfilter.cpp',
  # main plugin
  'plugin.cpp',
]

vapoursynth_dep = dependency('vapoursynth').partial_dependency(compile_args: true, includes: true)

libs = []

if host_machine.cpu_family().startswith('x86')
  add_project_arguments('-DVS_TARGET_CPU_X86', '-mavx', language: 'cpp')
endif

shared_module('akarin', sources,
  dependencies: [ vapoursynth_dep, version_h ],
  link_with: libs,
  install: true,
  install_dir: join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
  gnu_symbol_visibility: 'hidden'
)
