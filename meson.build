project('Plugin', 'c', 'cpp',
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c99', 'cpp_std=c++17'],
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

use_asmjit = false

sources_expr = [
  # expr
  'expr/vslog.cpp',
  'expr/cpufeatures.cpp',
  'expr/kernel/cpulevel.cpp',
  'expr/exprfilter.cpp',
]

sources_expr2 = [
  # expr2
  'expr2/exprfilter.cpp',
  'expr2/reactor/CPUID.cpp',
  'expr2/reactor/Debug.cpp',
  'expr2/reactor/EmulatedIntrinsics.cpp',
  'expr2/reactor/ExecutableMemory.cpp',
  'expr2/reactor/LLVMAsm.cpp',
  'expr2/reactor/LLVMJIT.cpp',
  'expr2/reactor/LLVMReactor.cpp',
  'expr2/reactor/LLVMReactorDebugInfo.cpp',
  'expr2/reactor/OptimalIntrinsics.cpp',
  'expr2/reactor/Reactor.cpp',
  'expr2/reactor/ReactorDebugInfo.cpp',
]

sources_ngx = [
  # DLISR
  'ngx/ngx.cc',
  'ngx/ngximpl.cc',
]

sources_vfx = [
  # DLVFX
  'vfx/vfx.cc',
  'vfx/nvvfx/src/NVVideoEffectsProxy.cpp',
  'vfx/nvvfx/src/nvCVImageProxy.cpp',
]

sources_banding = [
  # Cambi
  'banding/cambifilter.c',
  'banding/libvmaf/picture.c',
  'banding/libvmaf/cambi.c',
  'banding/libvmaf/ref.c',
  'banding/libvmaf/mem.c',
  #'banding/libvmaf/opt.c',
  #'banding/libvmaf/test.c',
  #'banding/libvmaf/test_cambi.c',
]

sources_common = [
  # main plugin
  'plugin.cpp',
]

deps = []

if use_asmjit
  sources = sources_common + sources_expr
  incdir = include_directories('.')
  if host_machine.cpu_family().startswith('x86')
    add_project_arguments('-DVS_TARGET_CPU_X86', '-mavx', language: 'cpp')
  else
    error('the asmjit backend only support x86.')
  endif
else
  sources = sources_common + sources_expr2
  incdir = include_directories('expr2/reactor')
  deps += dependency('llvm', version: ['>= 10.0', '< 13'], method: 'config-tool', static: true,
    modules: [
      'asmprinter', 'executionengine', 'target', 'orcjit', 'native',
    ])
endif

if target_machine.system() == 'windows'
  # DLISR and DLVFX only supports Windows.
  sources += sources_ngx + sources_vfx
  add_project_arguments('-DHAVE_NGX', '-DHAVE_VFX', language: 'cpp')
endif

sources += sources_banding

vapoursynth_dep = dependency('vapoursynth').partial_dependency(compile_args: true, includes: true)

libs = []

shared_module('akarin', sources,
  dependencies: deps + [ vapoursynth_dep, version_h ],
  link_with: libs,
  install: true,
  include_directories: incdir,
  install_dir: join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
  gnu_symbol_visibility: 'hidden'
)
