project('MISRC', 'c',
        default_options: ['c_std=c11', 'buildtype=release', 'b_lto=true'],
        meson_version: '>=0.54')


version_target = vcs_tag(command: ['git-version.sh'], fallback: 'unknown_version', input: 'version.h.in', output: 'version.h')

warnings = [
  '-Wall',
  '-Wextra',
  '-Wshadow',
]

cflags = warnings + [
  # Ensure POSIX timing APIs (clock_gettime/nanosleep) are declared on glibc
  '-D_POSIX_C_SOURCE=200809L',
  # glibc default feature set (needed for endian helpers like le16toh)
  '-D_DEFAULT_SOURCE',
]
# HSDAOH_UPSTREAM selects the upstream-hsdaoh reference capture path in
# gui_capture.c (hsdaoh_data_info_t structured callback, stream_id routing).
# That path is incomplete for MISRC: it hard-disables simple-capture (MISRC
# mode) at gui_capture.c:~1407 and has no stream_id==1 (ChB) handler, so it
# drops channel B and skips gui_capture_configure_handler. It is therefore
# NOT used for Linux/macOS, which rely on the complete MISRC raw-callback +
# frame-parser path (both channels). Windows keeps HSDAOH_UPSTREAM=1 to
# preserve PR #12's (@machcnz) Windows HSDAOH connect fix (added alongside
# the -lavrt/-lksuser GUI linker flags below). See the Windows block.

ldflags = [
]

ldflags_capture = [
]

sources_extract = [
  'misrc_capture/misrc_extract.c',
  'common/extract.c',
  version_target
]

sources_capture = [
  'misrc_capture/misrc_capture.c',
  'common/extract.c',
  'common/ringbuffer.c',
  'common/rb_event.c',
  'common/buffer_manager.c',
  'common/flac_writer.c',
  'common/frame_parser.c',
  'common/device_enum.c',
  'common/file_utils.c',
  'common/ringbuffer_writer.c',
  version_target
]

debug_build = get_option('buildtype').startswith('debug')

if not debug_build
  # Extra performance flags
  cflags += [
    '-O3',
    '-funroll-loops',
    '-ftree-vectorize',
  ]
endif

host_cpu_family = host_machine.cpu_family()

host_system = host_machine.system()
windows_static_deps = host_system == 'windows' or host_system == 'cygwin'

if host_system == 'darwin'
  cflags += ['-D_DARWIN_C_SOURCE']
endif

if host_system != 'windows' and host_system != 'cygwin'
  # Expose POSIX APIs like clock_gettime/nanosleep and types like ssize_t.
  cflags += ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE']
endif

if host_cpu_family == 'x86_64'
  nasm_flags = []
  if host_system == 'windows' or host_system == 'cygwin'
    if host_cpu_family == 'x86'
      cflags += '-mstackrealign'
      ldflags += '-Wl,--kill-at'
      nasm_flags += '-DPREFIX'
    endif
    nasm_flags += ['-f', 'win64']
  elif host_system == 'linux' or host_system == 'bsd' # The BSDs are close enough, right?
    if debug_build
      nasm_flags += '-gdwarf'
    endif
    nasm_flags += ['-f', 'elf64']
  elif host_system == 'darwin'
    if debug_build
      nasm_flags += '-gdwarf'
    endif
    nasm_flags += ['--prefix', '_', '-f', 'macho64']
    cflags += ['-DPREFIX']
  else
    error('Unknown host system "@0@".'.format(host_system))
  endif

  nasm_sources = [
    'common/extract.asm',
  ]

  nasm = find_program('nasm')

  outputname = '@BASENAME@.o'
  if host_system == 'windows'
    outputname = '@BASENAME@.obj'
  endif

  nasm_gen = generator(nasm,
                       output: outputname,
                       arguments: nasm_flags + ['@INPUT@', '-o', '@OUTPUT@'])

  nasm_genb = nasm_gen.process(nasm_sources)
  sources_extract += nasm_genb
  sources_capture +=nasm_genb
endif


# hsdaoh: prefer the vendored build in .deps/install (mirrors CI) so a bare
# local `meson setup && ninja` cannot silently link a stale system libhsdaoh
# that lacks the v1.0.9 connect fixes (e.g. /usr/local/lib/libhsdaoh.so.0.1.0
# from an old `make install`). Fall back to pkg-config only if the vendored
# install is absent, and warn loudly there so the gap is visible.
fs = import('fs')
hsdaoh_vendored_pc = meson.current_source_dir() / '..' / '.deps' / 'install' / 'lib' / 'pkgconfig' / 'hsdaoh.pc'
if fs.exists(hsdaoh_vendored_pc)
  vendored_pc_dir = meson.current_source_dir() / '..' / '.deps' / 'install' / 'lib' / 'pkgconfig'
  hs_cflags = run_command('pkg-config', '--cflags', 'hsdaoh',
                          env: {'PKG_CONFIG_PATH': vendored_pc_dir}, check: true).stdout().strip().split()
  hs_libs = run_command('pkg-config', '--libs', 'hsdaoh',
                        env: {'PKG_CONFIG_PATH': vendored_pc_dir}, check: true).stdout().strip().split()
  hsdaoh_libdir = run_command('pkg-config', '--variable=libdir', 'hsdaoh',
                              env: {'PKG_CONFIG_PATH': vendored_pc_dir}, check: true).stdout().strip()
  hsdaoh_dep = declare_dependency(
    compile_args: hs_cflags,
    link_args: hs_libs + ['-Wl,-rpath,' + hsdaoh_libdir],
  )
  message('Using vendored hsdaoh from .deps/install (mirrors CI; libdir=' + hsdaoh_libdir + ')')
else
  hsdaoh_dep = dependency('hsdaoh', static: windows_static_deps)
  warning('Vendored hsdaoh not found in .deps/install; falling back to system hsdaoh. ' +
          'Build third_party/hsdaoh into .deps/install (see .github/workflows/build.yml) to mirror CI.')
endif
deps = [ hsdaoh_dep ]
libusb_common_dep = dependency('libusb-1.0', required : false, static: windows_static_deps)
libusb_common_dep_added = false

flac_dep =  dependency('flac', required : false, static: windows_static_deps)
if flac_dep.found()
  deps += [ flac_dep ]
  cflags += ['-DLIBFLAC_ENABLED=1']
  if host_system == 'windows'
    cflags += ['-DFLAC__NO_DLL=1']
  endif
  # libFLAC 1.5.x is the first release with a reliable multithreaded encoder
  # (FLAC__stream_encoder_set_num_threads, API version >= 14). Without
  # HAVE_FLAC_THREADING the set_num_threads call in flac_writer.c is compiled
  # out, pinning each channel's encoder to a single core regardless of the
  # flac_threads setting. That makes level-8 encoding fall behind real-time at
  # 40 MSPS and forces the record spill file to open even on fast SSDs.
  # Hard guardrail: libFLAC < 1.5.0 is not supported for this build. Fail the
  # configure rather than silently emitting a single-threaded binary, so a stale
  # distro libFLAC 1.4.x cannot produce a GUI that spills on fast hardware.
  flac_version = flac_dep.version()
  if flac_version == ''
    # Source/dist-only installs sometimes report an empty version; require a
    # verifiable >= 1.5.0 rather than assuming.
    error('libFLAC found but its version could not be determined; require >= 1.5.0 for multithreaded encode. Install libflac >= 1.5.0.')
  endif
  if not flac_version.version_compare('>=1.5.0')
    error('libFLAC @0@ is too old; require >= 1.5.0 for multithreaded FLAC encode (found @0@). Install libflac >= 1.5.0.'.format(flac_version))
  endif
  cflags += ['-DHAVE_FLAC_THREADING=1']
  message('libFLAC >= 1.5.0 found (@0@), FLAC multithreading enabled'.format(flac_version))
  message('FLAC found, building with FLAC support')
else
  cflags += ['-DLIBFLAC_ENABLED=0']
  message('FLAC not found, building without FLAC support')
endif

soxr_dep =  dependency('soxr', required : false, static: windows_static_deps)
if soxr_dep.found()
  deps += [ soxr_dep ]
  cflags += ['-DLIBSOXR_ENABLED=1']
  message('libsoxr found, building with resample support')
else
  cflags += ['-DLIBSOXR_ENABLED=0']
  message('libsoxr not found, building without resample support')
endif

if host_system == 'linux'
  alsa_dep = dependency('alsa', required : false)
  if alsa_dep.found()
    deps += [ alsa_dep ]
    cflags += ['-DLIBASOUND_ENABLED=1']
    message('ALSA found, building with CXADC ALSA audio capture support')
  else
    cflags += ['-DLIBASOUND_ENABLED=0']
    message('ALSA not found, building without CXADC ALSA audio capture support')
  endif
else
  cflags += ['-DLIBASOUND_ENABLED=0']
endif

fftw3f_dep = dependency('fftw3f', required : false, static: windows_static_deps)
if fftw3f_dep.found()
  cflags += ['-DLIBFFTW_ENABLED=1']
  message('FFTW3 (single precision) found, building with FFT support')
else
  cflags += ['-DLIBFFTW_ENABLED=0']
  message('FFTW3 not found; misrc_gui requires fftw3f and will fail to configure until it is installed')
endif

# FX3 support (optional, Linux uses cyusb, others use libusb)
fx3_enabled = false
if host_system == 'linux'
  cyusb_dep = dependency('libcyusb', required : false)
  if cyusb_dep.found()
    deps += [ cyusb_dep ]
    cflags += ['-DENABLE_FX3=1']
    fx3_enabled = true
    message('libcyusb found, building with FX3 support')
  else
    message('libcyusb not found, building without FX3 support')
  endif
else
  # On non-Linux platforms, use libusb for FX3
  if libusb_common_dep.found()
    if not libusb_common_dep_added
      deps += [ libusb_common_dep ]
      libusb_common_dep_added = true
    endif
    cflags += ['-DENABLE_FX3=1']
    fx3_enabled = true
    message('libusb found, building with FX3 support')
  else
    message('libusb not found, building without FX3 support')
  endif
endif

# DdD (DomesdayDuplicator) support (optional, uses libusb on all platforms)
ddd_enabled = false
if libusb_common_dep.found()
  if not libusb_common_dep_added
    deps += [ libusb_common_dep ]
    libusb_common_dep_added = true
  endif
  cflags += ['-DENABLE_DDD=1']
  ddd_enabled = true
  message('libusb-1.0 found, building with DdD support')
else
  message('libusb-1.0 not found, building without DdD support')
endif


if host_system == 'windows' or host_system == 'cygwin'
  if meson.get_compiler('c').get_id() != 'gcc' and meson.get_compiler('c').get_id() != 'clang'
    sources_extract += [ 'getopt/getopt.c' ]
    sources_capture += [ 'getopt/getopt.c' ]
  endif
  cflags += [ '-DNTDDI_VERSION=NTDDI_WIN10_RS4', '-D_WIN32_WINNT=_WIN32_WINNT_WIN10' ]
  # Windows uses the same complete MISRC raw-callback + frame-parser path as
  # Linux/macOS (both channels + 4ch audio). HSDAOH_UPSTREAM is NOT defined
  # here: that upstream reference path drops ChB (no stream_id==1 handler) and
  # disables MISRC simple-capture. PR #12's Windows HSDAOH connect fix is the
  # -lavrt/-lksuser GUI linker flags on gui_ldflags below, independent of this
  # define, and remain in place.
  ldflags += [ '-static', '-static-libgcc' ]
  if meson.get_compiler('c').get_id() == 'gcc'
    ldflags += [ '-lgomp' ]
  endif
  ldflags_capture += [ '-lmf', '-lmfplat', '-lmfuuid', '-lmfreadwrite', '-lole32', '-lonecore' ]
  sources_capture += 'misrc_capture/simple_capture/simple_capture_mediafoundation.c'
  if host_cpu_family == 'aarch64'
    ldflags += [ '-lwinpthread' ]
    ldflags_capture += [ '-lwinpthread' ]
  endif
elif host_system == 'linux'
  sources_capture += 'misrc_capture/simple_capture/simple_capture_v4l2.c'
  ldflags_capture += [ '-lm' ]
elif host_system == 'darwin'
  add_languages('objc', native: false)
  ldflags_capture += ['-Wl,-framework,Cocoa', '-Wl,-framework,AVFoundation', '-Wl,-framework,CoreMedia', '-Wl,-framework,CoreVideo', '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation', '-Wl,-framework,Security']
  sources_capture += 'misrc_capture/simple_capture/simple_capture_avfoundation.m'
endif

executable('misrc_extract',
              sources_extract,
              dependencies: [],
              link_args: ldflags,
              c_args: cflags,
              install: true)

executable('misrc_capture',
              sources_capture,
              dependencies: deps,
              link_args: ldflags + ldflags_capture,
              c_args: cflags,
              install: true)

# GUI application (requires raylib)
raylib_dep = dependency('raylib', required: false, static: windows_static_deps)

if raylib_dep.found()
  if not fftw3f_dep.found()
    error('FFTW3 (fftw3f) is required for misrc_gui. Install fftw3f and reconfigure (use --wipe if reusing an existing build directory).')
  endif
  sources_gui = [
    # Core
    'misrc_gui/core/misrc_gui.c',
    'misrc_gui/core/gui_settings.c',
    # Input
    'misrc_gui/input/gui_capture.c',
    'misrc_gui/input/gui_cxadc.c',
    'misrc_gui/input/gui_simulated.c',
    'misrc_gui/input/gui_playback.c',
    # Processing
    'misrc_gui/processing/gui_extract.c',
    'misrc_gui/processing/gui_display_thread.c',
    # Signal
    'misrc_gui/signal/gui_headswitch_lock.c',
    'misrc_gui/signal/gui_trigger.c',
    'misrc_gui/signal/gui_cvbs.c',
    'misrc_gui/signal/gui_histogram.c',
    # Visualization
    'misrc_gui/visualization/gui_oscilloscope.c',
    'misrc_gui/visualization/gui_phosphor_rt.c',
    'misrc_gui/visualization/gui_fft.c',
    'misrc_gui/visualization/gui_panel.c',
    'misrc_gui/visualization/gui_histogram_panel.c',
    'misrc_gui/visualization/gui_text.c',
    'misrc_gui/visualization/gui_vu_meter.c',
    'misrc_gui/visualization/panel_registry.c',
    # UI
    'misrc_gui/ui/gui_ui.c',
    'misrc_gui/ui/gui_dropdown.c',
    'misrc_gui/ui/gui_popup.c',
    'misrc_gui/ui/clay_renderer_raylib.c',
    # Output
    'misrc_gui/output/gui_record.c',
    'misrc_gui/output/gui_audio.c',
    'common/extract.c',
    'common/ringbuffer.c',
    'common/rb_event.c',
    'common/buffer_manager.c',
    'common/flac_writer.c',
    'common/frame_parser.c',
    'common/device_enum.c',
    'common/file_utils.c',
    'common/ringbuffer_writer.c',
    version_target
  ]

  if host_cpu_family == 'x86_64'
    sources_gui += nasm_genb
  endif

  # Add FX3 support to GUI if available
  if fx3_enabled
    sources_gui += 'misrc_gui/input/gui_fx3.c'
  endif

  # Add DdD support to GUI if available
  if ddd_enabled
    sources_gui += 'misrc_gui/input/gui_ddd.c'
  endif

  gui_deps = deps + [ raylib_dep, fftw3f_dep ]

  gui_cflags = cflags + [
    '-O3',                    # Maximum optimization (should already be set by buildtype=release)
    '-funroll-loops',         # Unroll loops for better performance
    '-ftree-vectorize',       # Enable auto-vectorization
    '-ffast-math',            # Fast floating-point (safe for video/audio processing)
  ]

  # GUI doesn't need all the ldflags_capture (e.g., onecore conflicts with raylib's CloseWindow)
  gui_ldflags = ldflags

  if host_system == 'windows' or host_system == 'cygwin'
    gui_ldflags += ['-lopengl32', '-lgdi32', '-lwinmm']
    # Need mincore for VirtualAlloc2/MapViewOfFile3 used by ringbuffer
    gui_ldflags += ['-lmincore']
    # simple_capture for device enumeration
    sources_gui += 'misrc_capture/simple_capture/simple_capture_mediafoundation.c'
    gui_ldflags += ['-lmf', '-lmfplat', '-lmfuuid', '-lmfreadwrite', '-lole32', '-lavrt', '-lksuser', '-luuid', '-lshell32']
  elif host_system == 'linux'
    gui_ldflags += ['-lGL', '-lm', '-lpthread', '-ldl', '-lrt', '-lX11']
    sources_gui += 'misrc_capture/simple_capture/simple_capture_v4l2.c'
  elif host_system == 'darwin'
    gui_ldflags += ['-Wl,-framework,OpenGL', '-Wl,-framework,Cocoa', '-Wl,-framework,IOKit', '-Wl,-framework,CoreAudio', '-Wl,-framework,CoreVideo']
    gui_ldflags += ['-Wl,-framework,AVFoundation', '-Wl,-framework,CoreMedia', '-Wl,-framework,Security']
    sources_gui += 'misrc_capture/simple_capture/simple_capture_avfoundation.m'
  endif

  gui_includes = include_directories('misrc_gui/ui')
  gui_win_subsystem = 'console'
  if host_system == 'windows' or host_system == 'cygwin'
    gui_win_subsystem = 'windows'
  endif
  gui_win_resources = []
  if host_system == 'windows' or host_system == 'cygwin'
    windows_mod = import('windows')
    gui_win_resources = windows_mod.compile_resources('misrc_gui/assets/misrc_gui.rc')
  endif

  executable('misrc_gui',
              sources_gui + gui_win_resources,
              dependencies: gui_deps,
              include_directories: gui_includes,
              link_args: gui_ldflags,
              c_args: gui_cflags,
              win_subsystem: gui_win_subsystem,
              install: true)

  message('raylib found, building GUI application')
else
  message('raylib not found, skipping GUI application')
endif
