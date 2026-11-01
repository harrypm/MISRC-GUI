project('MISRC', 'c',
        default_options: ['c_std=c11', 'buildtype=release', 'b_lto=true'],
        meson_version: '>=0.55')


# Version string: git-version.py is the portable twin of git-version.sh.
# Native-Windows meson/Python cannot execute a POSIX shell script, which left
# local Windows builds on the 'unknown_version' fallback (v1.2.7 updater
# false-positive class). Any Python that can run meson itself can run the
# port; git-version.sh stays the source of truth for CI shell scripts.
# NOTE: the interpreter path must be a plain STRING (full_path()). meson 0.61.2
# (the ubuntu-22.04 runner's system meson: Linux AppImage + Android APK jobs)
# does shutil.which(vcs_cmd[0]) in func_vcs_tag, which raises
# "TypeError: expected str, bytes or os.PathLike object, not ExternalProgram"
# when handed the program object -> meson setup dies (CI runs 40448680208+).
# The script path must ALSO be absolute: meson converts files() to a path
# relative to the BUILD dir, but vcstagger runs the command with
# cwd=source_dir - the relative form only resolved by accident when the
# build dir was a sibling of misrc_tools (build-local), never on CI.
version_prog = find_program('python3', 'python', required: false, native: true)
if version_prog.found()
  version_target = vcs_tag(command: [version_prog.full_path(),
                                     meson.current_source_dir() / 'git-version.py'],
                           fallback: 'unknown_version', input: 'version.h.in', output: 'version.h')
else
  version_target = vcs_tag(command: ['git-version.sh'],
                           fallback: 'unknown_version', input: 'version.h.in', output: 'version.h')
endif

warnings = [
  '-Wall',
  '-Wextra',
  '-Wshadow',
  # GCC >= 14 and clang >= 16 make these four hard errors by default (C99+),
  # but the older toolchains still used on some CI runners (ubuntu-22.04
  # gcc 11, Apple clang, NDK clang 14) only warn. A missing libc include
  # (e.g. malloc/free without <stdlib.h>) then breaks ONLY the MSYS2 Windows
  # jobs, after a full vendored-deps rebuild (run 37035393223: gui_record_direct.c
  # failed there while Linux/macOS passed). Promoting them to -Werror here
  # makes every platform fail at the first compile instead, and keeps old
  # compilers at the same strictness the Windows toolchain already enforces.
  '-Werror=implicit-function-declaration',
  '-Werror=implicit-int',
  '-Werror=int-conversion',
  '-Werror=incompatible-pointer-types',
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
  'common/file_utils.c',
  'common/utf8_argv.c',
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
  'common/utf8_argv.c',
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
if host_system == 'windows'
  ldflags += ['-lshell32']
endif

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
hsdaoh_vendored_prefix = meson.current_source_dir() / '..' / '.deps' / 'install'
# A local checkout may also contain a Linux .deps/install. Keep Windows
# archives in the existing install-win prefix separate from ELF libraries.
if host_system == 'windows' and fs.exists(meson.current_source_dir() / '..' / '.deps' / 'install-win' / 'lib' / 'pkgconfig' / 'hsdaoh.pc')
  hsdaoh_vendored_prefix = meson.current_source_dir() / '..' / '.deps' / 'install-win'
endif
hsdaoh_vendored_pc = hsdaoh_vendored_prefix / 'lib' / 'pkgconfig' / 'hsdaoh.pc'
if fs.exists(hsdaoh_vendored_pc)
  vendored_pc_dir = hsdaoh_vendored_prefix / 'lib' / 'pkgconfig'
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
  message('Using vendored hsdaoh from ' + hsdaoh_vendored_prefix + ' (mirrors CI; libdir=' + hsdaoh_libdir + ')')
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

# FX3 support: build the vendored cyusb compatibility shim from source on
# ALL platforms (Linux, Windows, macOS) when libusb-1.0 is present. The real
# Cypress cyusb library is Linux-only and not packaged by distros; the previous
# external libcyusb pkg-config lookup had no package and was faked via an
# ephemeral /tmp cwrap that broke the build after reboot. Building the shim
# from source in-tree makes FX3 native on every platform: no external
# libcyusb package needed anywhere, no system libcyusb can shadow a vendored
# copy (the dep is built in-tree), and local == CI. The shim is a faithful
# drop-in: cyusb_handle aliases libusb_device_handle, matching the existing
# call sites in gui_fx3.c which pass handles directly to libusb_* functions.
# The shim uses LIBUSB_API_VERSION + __has_include so it builds on every OS.
fx3_enabled = false
if libusb_common_dep.found()
  if not libusb_common_dep_added
    deps += [ libusb_common_dep ]
    libusb_common_dep_added = true
  endif
  cyusb_inc = include_directories('../third_party/cyusb')
  cyusb_compat_lib = static_library('cyusb_compat',
    '../third_party/cyusb/cyusb.c',
    include_directories: cyusb_inc,
    dependencies: [ libusb_common_dep ],
  )
  cyusb_dep = declare_dependency(
    link_with: cyusb_compat_lib,
    include_directories: cyusb_inc,
    dependencies: [ libusb_common_dep ],
  )
  deps += [ cyusb_dep ]
  cflags += ['-DENABLE_FX3=1']
  fx3_enabled = true
  message('Vendored cyusb shim + libusb-1.0 found, building with FX3 support')
else
  message('libusb-1.0 not found, building without FX3 support')
endif

# DdD (DomesdayDuplicator) support (optional, uses libusb on all platforms)
ddd_enabled = false
if libusb_common_dep.found()
  if not libusb_common_dep_added
    deps += [ libusb_common_dep ]
    libusb_common_dep_added = true
  endif
  cflags += ['-DENABLE_DDD=1']
  sources_capture += 'common/ddd_protocol.c'
  ddd_enabled = true
  message('libusb-1.0 found, building with DdD support')
else
  message('libusb-1.0 not found, building without DdD support')
endif

# RTL-SDR support (optional, librtlsdr + libusb-1.0 on all platforms).
# The RTL-SDR raw I/Q backend (gui_rtlsdr.c) is only compiled when librtlsdr
# is found; the device-agnostic Demod panel (gui_demod.c) is always compiled.
rtlsdr_enabled = false
rtlsdr_pc = dependency('librtlsdr', required : false, static: windows_static_deps)
if rtlsdr_pc.found()
  if not libusb_common_dep_added
    # librtlsdr needs libusb-1.0 at link time on Windows static builds.
    deps += [ libusb_common_dep ]
    libusb_common_dep_added = true
  endif
  if windows_static_deps
    # MSYS2's rtl-sdr package names its static archive librtlsdr_static.a
    # (there is no librtlsdr.a), and the .pc Libs ("-lrtlsdr") resolves to the
    # DLL import lib. With the global -static ldflag, carry over only the
    # compile-side (include dirs) of the pkg-config dependency and link the
    # static archive explicitly by filename in the GUI link step below.
    rtlsdr_dep = rtlsdr_pc.partial_dependency(compile_args: true, includes: true)
  else
    rtlsdr_dep = rtlsdr_pc
  endif
  deps += [ rtlsdr_dep ]
  cflags += ['-DENABLE_RTLSDR=1']
  rtlsdr_enabled = true
  message('librtlsdr found, building with RTL-SDR support')
else
  message('librtlsdr not found, building without RTL-SDR support (Demod panel still built)')
endif


if host_system == 'windows' or host_system == 'cygwin'
  if meson.get_compiler('c').get_id() != 'gcc' and meson.get_compiler('c').get_id() != 'clang'
    sources_extract += [ 'getopt/getopt.c' ]
    sources_capture += [ 'getopt/getopt.c' ]
  endif
  cflags += [ '-DNTDDI_VERSION=NTDDI_WIN10_RS4', '-D_WIN32_WINNT=_WIN32_WINNT_WIN10' ]
  # The below line must be included to build HSDAOH 
  # Windows keeps HSDAOH_UPSTREAM=1 to use the upstream-hsdaoh reference capture
  # path which includes PR #12's (@machcnz) Windows HSDAOH connect fix via
  # libusb/WinUSB. Without this define, hsdaoh_open() fails to claim the MS2130
  # USB interface on Windows and the device cannot connect. The MISRC raw-callback
  # + frame-parser path (used on Linux/macOS for both channels) does not have the
  # Windows-specific USB claim sequence needed for the MS2130.
  # The -lavrt/-lksuser GUI linker flags below are also part of PR #12's fix.
  cflags += [ '-DHSDAOH_UPSTREAM=1' ]
  ldflags += [ '-static', '-static-libgcc' ]
  if meson.get_compiler('c').get_id() == 'gcc'
    ldflags += [ '-lgomp' ]
  elif soxr_dep.found()
    # MSYS2's clang-built static libsoxr.a is compiled with OpenMP enabled
    # (soxr.c uses #pragma omp; filter.c uses omp_* locks), so the -static
    # link needs the LLVM OpenMP runtime -lomp (shipped by
    # mingw-w64-clang-aarch64-llvm-openmp; libomp.a). The gcc environments
    # get the equivalent from -lgomp above. Run 37053411644 (Windows arm64)
    # failed with: ld.lld: error: undefined symbol: __kmpc_fork_call /
    # omp_init_lock ... >>> referenced by libsoxr.a(soxr.c.obj) /
    # libsoxr.a(filter.c.obj).
    ldflags += [ '-lomp' ]
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
elif host_system == 'android'
  # No V4L2 on Android; simple_capture_android.c is a no-device stub (CXADC
  # capture is out of scope for the basic Android release).
  sources_capture += 'misrc_capture/simple_capture/simple_capture_android.c'
  ldflags_capture += [ '-lm' ]
elif host_system == 'darwin'
  add_languages('objc', native: false)
  ldflags_capture += ['-Wl,-framework,Cocoa', '-Wl,-framework,AVFoundation', '-Wl,-framework,CoreMedia', '-Wl,-framework,CoreVideo', '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation', '-Wl,-framework,Security']
  sources_capture += 'misrc_capture/simple_capture/simple_capture_avfoundation.m'
endif

if host_system == 'android'
  # raylib was cross-built with GRAPHICS_API_OPENGL_ES3 (rcore_android.c +
  # rlgl ES3 paths). GUI sources that include rlgl.h must match, else
  # rlActiveDrawBuffers references the glDrawBuffersEXT extension symbol
  # which is absent on Android GLES and fails at link time.
  cflags += ['-DGRAPHICS_API_OPENGL_ES3']
endif

# The CLI tools (misrc_extract, misrc_capture) are not built for Android: an
# APK has no terminal to run them, and they would only add cross-link risk.
# The Android build target is the libmisrc_gui.so NativeActivity library below.
if host_system != 'android'
  extract_exe = executable('misrc_extract',
                sources_extract,
                dependencies: [],
                link_args: ldflags,
                c_args: cflags,
                install: true)

  path_io_test = executable('path_io_test',
    ['test/path_io_test.c', 'common/file_utils.c', 'common/utf8_argv.c'],
    c_args: cflags, link_args: ldflags)
  path_test_python = find_program('python3', 'python', required: false)
  if path_test_python.found()
    test('path_io', path_test_python,
      args: [files('test/path_io_test.py'), path_io_test.full_path(), extract_exe.full_path()],
      depends: [path_io_test, extract_exe])
  endif

  executable('misrc_capture',
                sources_capture,
                dependencies: deps,
                link_args: ldflags + ldflags_capture,
                c_args: cflags,
                install: true)
endif

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
    # Release updater (releases page button + auto-patch manifest + confirmed
    # self-update; unit-tested by test/gui_updater_test.c)
    'misrc_gui/core/gui_updater.c',
    # Input
    'misrc_gui/input/gui_capture.c',
    'misrc_gui/input/gui_cxadc.c',
    'misrc_gui/input/gui_simulated.c',
    'misrc_gui/input/gui_playback.c',
    'misrc_gui/input/gui_playback_length_cache.c',
    # Processing
    'misrc_gui/processing/gui_extract.c',
    'misrc_gui/processing/gui_display_thread.c',
    # Signal
    'misrc_gui/signal/gui_headswitch_lock.c',
    'misrc_gui/signal/gui_trigger.c',
    'misrc_gui/signal/gui_cvbs.c',
    'misrc_gui/signal/gui_vhs_fm.c',
    'misrc_gui/signal/gui_rf_preview.c',
    'misrc_gui/signal/gui_rf_resampler.c',
    'misrc_gui/processing/gui_rf_preview_worker.c', 'misrc_gui/signal/gui_beta_hifi.c',
    'misrc_gui/processing/gui_rf_preview_policy.c',
    'misrc_gui/processing/gui_rf_preview_pressure.c',
    'misrc_gui/processing/gui_rf_stream_marks.c',
    'misrc_gui/signal/gui_histogram.c',
    'misrc_gui/signal/gui_demod.c',
    # Test Gen: on-demand PAL/NTSC/SECAM test signal (hacktv values)
    'misrc_gui/signal/gui_testgen.c',
    # Visualization
    'misrc_gui/visualization/gui_oscilloscope.c',
    'misrc_gui/visualization/gui_phosphor_rt.c',
    'misrc_gui/visualization/gui_fft.c',
    'misrc_gui/visualization/gui_panel.c',
    'misrc_gui/visualization/gui_histogram_panel.c',
    'misrc_gui/visualization/gui_spectrogram.c',
    'misrc_gui/visualization/gui_text.c',
    'misrc_gui/visualization/gui_vu_meter.c',
    'misrc_gui/visualization/panel_registry.c',
    # UI
    'misrc_gui/ui/gui_font.c',
    'misrc_gui/ui/gui_ui_scale.c',
    'misrc_gui/ui/gui_ui.c',
    'misrc_gui/ui/gui_chrome_hide.c',
    'misrc_gui/ui/gui_device_label.c',
    'misrc_gui/ui/gui_dropdown.c',
    'misrc_gui/ui/gui_popup.c',
    'misrc_gui/ui/clay_renderer_raylib.c',
    # Server/Client networking (cxadc_vhs_server-style peer mode)
    'misrc_gui/net/gui_net.c',
    'misrc_gui/net/gui_monitor_transport.c',
    'misrc_gui/net/gui_monitor_settings.c',
    'misrc_gui/net/gui_monitor_local_save.c',
    'misrc_gui/net/monitor_local_save_proxy.c',
    'misrc_gui/net/monitor_local_save_files.c',
    'misrc_gui/net/monitor_local_save_protocol.c',
    'misrc_gui/net/gui_remote_fs.c',
    'misrc_gui/net/gui_remote_display.c',
    'misrc_gui/net/gui_monitor_video_wire.c',
    'misrc_gui/net/gui_monitor_video_i420.c',
    'misrc_gui/net/gui_monitor_video_framebox.c',
    'misrc_gui/net/gui_monitor_video_codec.c',
    'misrc_gui/net/gui_monitor_video_server.c',
    'misrc_gui/net/gui_monitor_video_client.c',
    # Output
    'misrc_gui/output/gui_record.c',
    'misrc_gui/output/gui_record_direct.c',
    'misrc_gui/output/gui_audio.c',
    'common/extract.c',
    'common/ringbuffer.c',
    'common/rb_event.c',
    'common/buffer_manager.c',
    'common/flac_writer.c',
    'common/frame_parser.c',
    'common/device_enum.c',
    'common/file_utils.c',
    'common/utf8_argv.c',
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
    sources_gui += 'common/ddd_protocol.c'
    sources_gui += 'misrc_gui/input/gui_ddd.c'
    sources_gui += 'misrc_gui/input/gui_ddd_v1.c'
    sources_gui += 'misrc_gui/input/gui_ddd_async.c'
    sources_gui += 'misrc_gui/input/gui_ddd_clockgen.c'
    sources_gui += 'misrc_gui/ui/gui_ddd_fifo_status.c'
  endif

  # Add RTL-SDR support to GUI if available
  if rtlsdr_enabled
    sources_gui += 'misrc_gui/input/gui_rtlsdr.c'
  endif

  # M-Server/M-Client use Zstd-1 for original phosphor/FFT preview.
  zstd_dep = dependency('libzstd', required: true, static: windows_static_deps)
  gui_deps = deps + [ raylib_dep, fftw3f_dep ]
  gui_deps += [ zstd_dep ]
  # OpenH264 is required for encoded monitor video but optional for legacy
  # waveform/FFT-only builds. Explicitly report disabled video when absent.
  openh264_dep = dependency('openh264', required: false, static: windows_static_deps or host_system == 'android')
  if openh264_dep.found()
    gui_deps += [openh264_dep]
    # GUI is C, while the static OpenH264 archive uses the C++ runtime.
    # Link the runtime AFTER OpenH264 to resolve RTTI/new/delete on MinGW
    # and libc++ on Android. Unmodified builds stay C-only.
    if windows_static_deps
      gui_deps += [meson.get_compiler('c').find_library('stdc++', required: true)]
    elif host_system == 'android'
      gui_deps += [meson.get_compiler('c').find_library('c++', required: true)]
    endif
    message('MISRC monitor H.264 video codec enabled')
    openh264_define = '-DMISRC_HAVE_OPENH264=1'
  else
    warning('OpenH264 development package missing: monitor video is unavailable')
    openh264_define = '-DMISRC_HAVE_OPENH264=0'
  endif

  gui_cflags = cflags + [openh264_define] + [
    '-O3',                    # Maximum optimization (should already be set by buildtype=release)
    '-funroll-loops',         # Unroll loops for better performance
    '-ftree-vectorize',       # Enable auto-vectorization
    '-ffast-math',            # Fast floating-point (safe for video/audio processing)
  ]
  # Keep rlgl.h and phosphor shader syntax consistent with the vendored raylib
  # OpenGL 2.1 build on Linux ARM64. Other platforms retain their existing path.
  if host_system == 'linux' and host_cpu_family == 'aarch64'
    gui_cflags += ['-DGRAPHICS_API_OPENGL_21']
  endif

  # GUI doesn't need all the ldflags_capture (e.g., onecore conflicts with raylib's CloseWindow)
  gui_ldflags = ldflags

  if host_system == 'windows' or host_system == 'cygwin'
    # raylib 5.5 is built from source with its bundled GLFW (USE_EXTERNAL_GLFW
    # defaults to OFF), so the GLFW objects are compiled into libraylib.a and
    # no separate glfw link is needed. Do NOT add -l:libglfw3.dll.a here: that
    # import library is only shipped by MSYS2's mingw glfw package, which the CI
    # MSYS2 install list does not include, so the flag breaks the link with
    # "cannot find -l:libglfw3.dll.a". OpenGL/GDI/winmm are still required.
    gui_ldflags += ['-lopengl32', '-lgdi32', '-lwinmm']
    # RTL-SDR: link the static archive by filename (MSYS2 names it
    # librtlsdr_static.a; the .pc -lrtlsdr resolves to the DLL import lib
    # which -static rejects), with the .pc's OWN libdir on the search path:
    # the .pc may resolve from a different MSYS2 environment's pkgconfig
    # dir than the compiler's (e.g. rtl-sdr installed only under mingw64
    # while the GUI builds with the ucrt64 toolchain for CI parity) - ld's
    # default search path only covers the compiler's own environment, so
    # the by-filename archive link failed with "cannot find
    # -l:librtlsdr_static.a". CI is unaffected (it installs no rtlsdr:
    # RTLSDR stays disabled there).
    if rtlsdr_enabled
      gui_ldflags += ['-l:librtlsdr_static.a']
      rtlsdr_libdir = rtlsdr_pc.get_variable(pkgconfig : 'libdir', default_value : '')
      if rtlsdr_libdir != ''
        gui_ldflags += ['-L' + rtlsdr_libdir]
      endif
    endif
    # Need mincore for VirtualAlloc2/MapViewOfFile3 used by ringbuffer
    gui_ldflags += ['-lmincore']
    # Winsock for the server/client networking module (gui_net.c)
    gui_ldflags += ['-lws2_32']
    # simple_capture for device enumeration
    sources_gui += 'misrc_capture/simple_capture/simple_capture_mediafoundation.c'
    # Windows modal move/size loop render driver (subclass + WM_TIMER
    # frames; backlog item 2 - the window froze during move/resize because
    # DefWindowProc's modal loop blocked raylib's event poll)
    sources_gui += 'misrc_gui/core/gui_win32_modal.c'
    gui_ldflags += ['-lmf', '-lmfplat', '-lmfuuid', '-lmfreadwrite', '-lole32', '-lavrt', '-lksuser', '-luuid', '-lshell32']
    # oleaut32 for the cxadc-win WMI config path in gui_cxadc.c
    # (VariantClear/SysFreeString on the ROOT\WMI IWbemServices calls)
    gui_ldflags += ['-loleaut32']
  elif host_system == 'linux'
    gui_ldflags += ['-lGL', '-lm', '-lpthread', '-ldl', '-lrt', '-lX11']
    sources_gui += 'misrc_capture/simple_capture/simple_capture_v4l2.c'
  elif host_system == 'android'
    # NativeActivity + raylib rcore_android.c use EGL + GLES. raylib 5.5 links
    # GLESv2 (see cmake/LibraryConfigurations.cmake); we also link GLESv3
    # because the cross raylib was built with GRAPHICS_API_OPENGL_ES3 so rlgl
    # uses ES3 entry points. -landroid/-llog for ANativeActivity/android_log.
    # No -lX11/-lGL on Android (ANativeWindow replaces X11).
    # NOTE: no -lpthread on Android — bionic's libc provides pthread (there is
    # no separate libpthread; -lpthread fails to link). -ldl is a bionic stub.
    gui_ldflags += ['-lEGL', '-lGLESv2', '-lGLESv3', '-landroid', '-llog', '-lOpenSLES', '-lm', '-ldl']
    sources_gui += 'misrc_capture/simple_capture/simple_capture_android.c'
    # JNI bridge: MainActivity.nativeSetUsbFd -> android_usb_get_fd() so the
    # hsdaoh capture path can wrap the Java-granted USB fd with uvc_wrap().
    sources_gui += 'misrc_gui/input/android_usb_jni.c'
  elif host_system == 'darwin'
    gui_ldflags += ['-Wl,-framework,OpenGL', '-Wl,-framework,Cocoa', '-Wl,-framework,IOKit', '-Wl,-framework,CoreAudio', '-Wl,-framework,CoreVideo']
    gui_ldflags += ['-Wl,-framework,AVFoundation', '-Wl,-framework,CoreMedia', '-Wl,-framework,Security']
    sources_gui += 'misrc_capture/simple_capture/simple_capture_avfoundation.m'
    sources_gui += 'misrc_gui/input/gui_cxadc_macos.c'
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

  # Headless CLI capture mode: compile misrc_capture.c into the GUI binary
  # (non-Android only) with main renamed to misrc_capture_main, so the GUI
  # binary can run as the full misrc_capture CLI when capture args are passed.
  # The static lib's extern symbols (usage/list_devices/audio_file_writer/etc.)
  # are unique to this TU and don't collide with the GUI's; common symbols are
  # resolved from the GUI's own common/ objects at link time.
  # version_target is listed so the generated version.h is built before this TU
  # compiles (misrc_capture.c does #include "../version.h") and the build root
  # is on its include path -- without it, a fresh parallel build can compile
  # this TU before version.h exists (the standalone misrc_capture gets this via
  # version_target in sources_capture).
  gui_extra_link_with = []
  if host_system != 'android'
    misrc_capture_cli_lib = static_library('misrc_capture_cli',
                'misrc_capture/misrc_capture.c',
                version_target,
                include_directories: include_directories('misrc_capture'),
                dependencies: deps,
                c_args: cflags + ['-Dmain=misrc_capture_main'])
    gui_extra_link_with += [ misrc_capture_cli_lib ]
  endif

  if host_system == 'android'
    # NativeActivity loads a shared library libmisrc_gui.so that exports
    # ANativeActivity_onCreate (pulled from raylib.a by the -u link flag in the
    # cross-file). raylib's rcore_android.c defines android_main() which calls
    # the app's main(), so the existing misrc_gui.c main() is reused unchanged.
    # shared_library gives the lib* prefix + .so suffix on unix automatically.
    shared_library('misrc_gui',
                sources_gui + gui_win_resources,
                dependencies: gui_deps,
                include_directories: gui_includes,
                link_args: gui_ldflags,
                c_args: gui_cflags,
                install: true)
  else
    # Captured into misrc_gui_exe so the update_cli test can depend on the
    # real built binary (it drives the headless update CLI end to end).
    misrc_gui_exe = executable('misrc_gui',
                sources_gui + gui_win_resources,
                dependencies: gui_deps,
                include_directories: gui_includes,
                link_args: gui_ldflags,
                link_with: gui_extra_link_with,
                c_args: gui_cflags,
                win_subsystem: gui_win_subsystem,
                install: true)
  endif

  if host_system != 'android'
    font_test = executable('gui_font_test',
      ['test/gui_font_test.c', 'common/file_utils.c'],
      dependencies: [raylib_dep], c_args: cflags, link_args: gui_ldflags)
    test('gui_font', font_test)

    # Exercise the real overlay layout/click code with drawing stubs, without
    # opening a window or linking capture backends. LTO drops unrelated code
    # from the included waveform implementation, including in debug builds.
    overlay_test_deps = []
    foreach dep : gui_deps
      overlay_test_deps += dep.partial_dependency(compile_args: true, includes: true)
    endforeach
    overlay_test_deps += meson.get_compiler('c').find_library('m', required: false)
    # The harness includes the waveform implementation, which now calls the
    # shared chrome-hide helper (double-click overlay toggle), so compile it
    # in too. Same for the other panel harnesses below.
    waveform_overlay_test = executable('gui_waveform_overlay_test',
      ['test/gui_waveform_overlay_harness.c', 'misrc_gui/ui/gui_chrome_hide.c'],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_waveform_overlay', waveform_overlay_test)

    fft_label_test = executable('gui_fft_label_test',
      ['test/gui_fft_label_harness.c', 'misrc_gui/ui/gui_chrome_hide.c',
       'misrc_gui/visualization/gui_text.c'],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_fft_label', fft_label_test)

    settings_path_ldflags = ldflags
    if host_system == 'windows'
      settings_path_ldflags += ['-lole32']
    else
      settings_path_ldflags += ['-lm']
    endif
    # gui_settings.c (included by the harness) calls gui_monitor_settings_copy
    # since the M-Server/M-Client modes landed (bdcbb7a); link the real
    # implementation instead of a stub. It is self-contained (libc only).
    settings_paths_test = executable('gui_settings_paths_test',
      ['test/gui_settings_paths_test.c', 'misrc_gui/ui/gui_ui_scale.c', 'common/file_utils.c',
       'common/ddd_protocol.c', 'misrc_gui/net/gui_monitor_settings.c'],
      dependencies: overlay_test_deps, include_directories: gui_includes,
      c_args: gui_cflags + ['-fwhole-program'], link_args: settings_path_ldflags,
      override_options: ['b_lto=true', 'optimization=2'])
    test('gui_settings_paths', settings_paths_test)

    # Release updater: manifest parse (flat JSON), {tag} asset URL building,
    # semver compare + dev-tag fallback, tag extraction/safety, archive
    # naming, and the no-network failure paths of the confirmed self-update
    # pipeline (reserved-invalid host for the download step). Includes the
    # production gui_updater.c directly (repo test pattern).
    gui_updater_test = executable('gui_updater_test',
      ['test/gui_updater_test.c'],
      dependencies: overlay_test_deps, include_directories: gui_includes,
      c_args: cflags, link_args: settings_path_ldflags,
      override_options: ['b_lto=true', 'optimization=2'])
    test('gui_updater', gui_updater_test)

    # Update CLI end-to-end test: drives the REAL built misrc_gui binary
    # through the headless update flags (--check-update / --update-dry-run /
    # --install-update --yes) against a local test server that serves the
    # manifest, the releases/latest redirect and the asset zip, with an
    # isolated HOME. Asserts the key: value output, the --install-update
    # refusal without --yes, and the full install (old binary archived by
    # rename, payload replaced). Loopback http only - gui_updater's URL
    # scheme rule allows exactly that for this test.
    if path_test_python.found()
      test('update_cli', path_test_python,
        args: [files('test/update_cli_test.py'), misrc_gui_exe.full_path()],
        depends: [misrc_gui_exe])
    endif
    if flac_dep.found()
      # gui_record.c writes through the mls_proxy_* API since bdcbb7a; the
      # CPU-only harnesses link the stdio-passthrough stub (never the networked
      # local-save stack). Same rule as gui_playback_audio_stub.c.
      record_paths_test = executable('gui_record_paths_test',
        ['test/gui_record_paths_test.c', 'common/file_utils.c', version_target,
         'test/monitor_local_save_stub.c'],
        dependencies: overlay_test_deps + [flac_dep], include_directories: gui_includes,
        c_args: cflags + ['-fwhole-program'], link_args: ldflags,
        override_options: ['b_lto=true', 'optimization=2'])
      test('gui_record_paths', record_paths_test)
    endif

    # Exercise the production stats sizing with deterministic font metrics.
    # As above, LTO drops unrelated UI/backend code from the included source.
    # gui_ui.c's timeline name budget reads the selected playback file info
    # (gui_playback_get_selected_file_info): satisfied by the info stub TU
    # (NOT the audio stub - the playback harnesses compile the real
    # gui_playback.c and would collide).
    stats_layout_test = executable('gui_stats_layout_test',
      ['test/gui_stats_layout_harness.c', 'misrc_gui/ui/gui_ui_scale.c',
       'common/file_utils.c', 'test/gui_playback_info_stub.c', version_target],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_stats_layout', stats_layout_test)

    # Check connection-button text and padding with the same CPU-only setup.
    toolbar_layout_test = executable('gui_toolbar_layout_test',
      ['test/gui_toolbar_layout_harness.c', 'common/file_utils.c',
       'test/gui_playback_info_stub.c', version_target],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_toolbar_layout', toolbar_layout_test)

    viewport_layout_test = executable('gui_viewport_layout_test',
      ['test/gui_viewport_layout_harness.c', 'common/file_utils.c',
       'test/gui_playback_info_stub.c', version_target],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_viewport_layout', viewport_layout_test)

    spectrogram_lifetime_test = executable('gui_spectrogram_playback_lifetime_test',
      ['test/gui_spectrogram_playback_lifetime_test.c', 'misrc_gui/ui/gui_chrome_hide.c'],
      dependencies: overlay_test_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_spectrogram_playback_lifetime', spectrogram_lifetime_test)

    # Playback file-info resolution + decoder IO layer (STREAMINFO patch)
    # coverage. Synthesizes both FLAC header conventions with libFLAC, so it
    # needs the FLAC dependency at link time (not just compile args). The
    # paused-analysis path also FFTs on worker threads (gui_fft.c), so FFTW
    # is linked as well.
    if flac_dep.found() and fftw3f_dep.found()
      playback_resolve_test = executable('gui_playback_resolve_test',
        ['test/gui_playback_resolve_harness.c', 'test/gui_playback_audio_stub.c',
         'common/file_utils.c',
         'misrc_gui/input/gui_playback_length_cache.c',
         'misrc_gui/visualization/gui_fft.c',
         'misrc_gui/ui/gui_chrome_hide.c'],
        dependencies: overlay_test_deps + [flac_dep, fftw3f_dep, dependency('threads')],
        include_directories: gui_includes,
        c_args: gui_cflags,
        override_options: ['b_lto=true', 'optimization=2'],
      )
      test('gui_playback_resolve', playback_resolve_test)
    endif
    rf_input_stamps_test = executable('gui_rf_input_stamps_test',
      ['test/gui_rf_input_stamps_test.c', 'test/monitor_local_save_stub.c',
       'common/file_utils.c'],
      dependencies: overlay_test_deps + [dependency('threads')],
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_rf_input_stamps', rf_input_stamps_test)
    rf_routes_test = executable('gui_rf_routes_test',
      'test/gui_rf_routes_test.c',
      dependencies: overlay_test_deps + [dependency('threads')],
      include_directories: gui_includes,
      c_args: gui_cflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_rf_routes', rf_routes_test)

    rf_geometry_test = executable('gui_rf_preview_geometry_test',
      'test/gui_rf_preview_geometry_test.c',
      c_args: cflags,
      dependencies: [meson.get_compiler('c').find_library('m', required: false)],
    )
    test('gui_rf_preview_geometry', rf_geometry_test)

    # Synthetic baseband composite contracts for the ported CVBS profile +
    # VBI sync model + burst-locked colour (gui_cvbs.c included directly,
    # trigger_analyze_cvbs_levels links from gui_trigger.c; LTO drops UI paths).
    # gui_cvbs.c now carries the fixed-rate input resampler, so the soxr
    # link dependency is needed wherever it is compiled.
    cvbs_baseband_deps = overlay_test_deps + [dependency('threads')]
    if soxr_dep.found()
      cvbs_baseband_deps += [soxr_dep]
    endif
    cvbs_baseband_test = executable('gui_cvbs_test',
      ['test/gui_cvbs_test.c', 'misrc_gui/signal/gui_trigger.c'],
      dependencies: cvbs_baseband_deps,
      include_directories: gui_includes,
      c_args: gui_cflags,
      link_args: ldflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_cvbs_baseband', cvbs_baseband_test)

    # gui_audio.c (included by both harnesses) redirects stdio through the
    # mls_proxy_* macros; the harnesses already self-define those (their own
    # file_open_utf8 stubs get renamed by the macro). The only extra symbol
    # is gui_monitor_server_active (stubbed inside each harness).
    audio_hifi_monitor_test = executable('gui_audio_hifi_monitor_test',
      'test/gui_audio_hifi_monitor_test.c',
      dependencies: overlay_test_deps + [dependency('threads')],
      include_directories: gui_includes, c_args: gui_cflags, link_args: ldflags)
    test('gui_audio_hifi_monitor', audio_hifi_monitor_test)
    if soxr_dep.found()
      audio_soxr_lifecycle_test = executable('gui_audio_soxr_lifecycle_test',
        'test/gui_audio_soxr_lifecycle_test.c',
        dependencies: overlay_test_deps + [dependency('threads')],
        include_directories: gui_includes, c_args: gui_cflags, link_args: ldflags)
      test('gui_audio_soxr_lifecycle', audio_soxr_lifecycle_test)
    endif


    capture_watchdog_test = executable('gui_capture_watchdog_test',
      ['test/gui_capture_watchdog_test.c', 'common/file_utils.c',
       'test/gui_playback_audio_stub.c'],
      dependencies: overlay_test_deps + [dependency('threads')],
      include_directories: gui_includes,
      c_args: gui_cflags,
      link_args: ldflags,
      override_options: ['b_lto=true', 'optimization=2'],
    )
    test('gui_capture_watchdog', capture_watchdog_test)
    if flac_dep.found()
      rf_seek_test = executable('gui_rf_playback_seek_test',
        ['test/gui_rf_playback_seek_test.c', 'test/gui_playback_audio_stub.c',
         'common/file_utils.c',
         'misrc_gui/input/gui_playback_length_cache.c',
         'misrc_gui/visualization/gui_fft.c',
         'misrc_gui/ui/gui_chrome_hide.c'],
        dependencies: overlay_test_deps + [flac_dep, fftw3f_dep],
        include_directories: gui_includes,
        c_args: gui_cflags,
        link_args: ldflags,
        override_options: ['b_lto=true', 'optimization=2'],
      )
      test('gui_rf_playback_seek', rf_seek_test)
      rf_start_test = executable('gui_rf_playback_start_test',
        ['test/gui_rf_playback_start_test.c', 'test/gui_playback_audio_stub.c',
         'common/file_utils.c',
         'misrc_gui/input/gui_playback_length_cache.c',
         'misrc_gui/visualization/gui_fft.c',
         'misrc_gui/ui/gui_chrome_hide.c'],
        dependencies: overlay_test_deps + [flac_dep, fftw3f_dep, dependency('threads')],
        include_directories: gui_includes,
        c_args: gui_cflags,
        link_args: ldflags,
        override_options: ['b_lto=true', 'optimization=2'],
      )
      test('gui_rf_playback_start', rf_start_test)

      # Manual exact-media integration: requires one second of 20 MHz NTSC
      # s16le RF and an output directory, so it is not a default CI test.
      rf_reconnect_sources = [
        'test/gui_rf_playback_reconnect_test.c', 'test/gui_playback_audio_stub.c',
        'test/monitor_local_save_stub.c',
        'common/file_utils.c',
        'misrc_gui/input/gui_playback_length_cache.c',
        'misrc_gui/visualization/gui_fft.c',
        'misrc_gui/ui/gui_chrome_hide.c',
        'common/extract.c', 'common/ringbuffer.c',
        'common/rb_event.c', 'common/buffer_manager.c',
        'misrc_gui/processing/gui_rf_stream_marks.c',
        'misrc_gui/signal/gui_vhs_fm.c',
        'misrc_gui/signal/gui_rf_preview.c',
        'misrc_gui/signal/gui_rf_resampler.c',
        'misrc_gui/processing/gui_rf_preview_worker.c', 'misrc_gui/signal/gui_beta_hifi.c',
        'misrc_gui/processing/gui_rf_preview_policy.c',
        'misrc_gui/processing/gui_rf_preview_pressure.c',
      ]
      if host_cpu_family == 'x86_64'
        rf_reconnect_sources += nasm_genb
      endif
      rf_reconnect_deps = overlay_test_deps + [flac_dep, fftw3f_dep, dependency('threads')]
      if soxr_dep.found()
        rf_reconnect_deps += soxr_dep
      endif
      if host_system == 'windows'
        rf_reconnect_deps += [meson.get_compiler('c').find_library('psapi'),
                              meson.get_compiler('c').find_library('onecore')]
      endif
      executable('gui_rf_playback_reconnect_test', rf_reconnect_sources,
        dependencies: rf_reconnect_deps,
        include_directories: gui_includes,
        c_args: gui_cflags,
        link_args: ldflags,
        build_by_default: false,
        override_options: ['b_lto=true', 'optimization=2'],
      )
    endif

    # This helper compiles and runs a native fixture itself; cross builds
    # must use Meson's executable wrapper for their normal C test targets.
    timeline_cc = meson.get_compiler('c').cmd_array()
    if not meson.is_cross_build() and timeline_cc.length() == 1
      timeline_python = find_program('python3', 'python', required: false)
      if timeline_python.found()
        test('gui_rf_playback_timeline', timeline_python,
          args: [files('test/gui_rf_playback_timeline_test.py'), '--cc', timeline_cc[0],
                 '--temp-dir', meson.current_build_dir()],
        )
      endif
    endif
  endif

  message('raylib found, building GUI application')
else
  message('raylib not found, skipping GUI application')
endif

# Persistent length caching is independent of FLAC and the GUI.
playback_length_cache_test = executable('gui_playback_length_cache_test',
  ['test/gui_playback_length_cache_test.c', 'misrc_gui/input/gui_playback_length_cache.c'],
  c_args: cflags, link_args: ldflags)
test('gui_playback_length_cache', playback_length_cache_test)

# RF DSP tests are independent of the native GUI and capture backends.
rf_test_deps = [dependency('threads'), meson.get_compiler('c').find_library('m', required: false)]
if soxr_dep.found()
  rf_test_deps += [soxr_dep]
endif
rf_test_cflags = cflags + ['-O3', '-ffast-math', '-funroll-loops', '-ftree-vectorize']
rf_decoder_test = executable('gui_rf_preview_test',
  ['test/gui_rf_preview_test.c', 'misrc_gui/signal/gui_rf_preview.c'],
  dependencies: rf_test_deps, c_args: rf_test_cflags, link_args: ldflags)
test('gui_rf_preview', rf_decoder_test, timeout: 120)
rf_secam_test = executable('gui_rf_secam_test',
  ['test/gui_rf_secam_test.c', 'misrc_gui/signal/gui_rf_preview.c'],
  dependencies: rf_test_deps, c_args: rf_test_cflags, link_args: ldflags)
test('gui_rf_secam', rf_secam_test, timeout: 60)
rf_mesecam_test = executable('gui_rf_mesecam_test',
  ['test/gui_rf_mesecam_test.c', 'misrc_gui/signal/gui_rf_preview.c'],
  dependencies: rf_test_deps, c_args: rf_test_cflags, link_args: ldflags)
test('gui_rf_mesecam', rf_mesecam_test, timeout: 180)
rf_resampler_test = executable('gui_rf_resampler_test',
  ['test/gui_rf_resampler_test.c', 'misrc_gui/signal/gui_rf_resampler.c'],
  dependencies: rf_test_deps, c_args: rf_test_cflags, link_args: ldflags)
test('gui_rf_resampler', rf_resampler_test)
rf_worker_test = executable('gui_rf_preview_worker_test',
  ['test/gui_rf_preview_worker_test.c', 'misrc_gui/processing/gui_rf_preview_worker.c',
   'misrc_gui/processing/gui_rf_preview_policy.c'],
  dependencies: [dependency('threads'), meson.get_compiler('c').find_library('m', required: false)], c_args: cflags, link_args: ldflags)
test('gui_rf_preview_worker', rf_worker_test)
if host_machine.system() == 'windows'
  rf_handoff_test = executable('gui_rf_preview_handoff_test',
    ['test/gui_rf_preview_handoff_test.c', 'misrc_gui/processing/gui_rf_preview_policy.c'],
    dependencies: [dependency('threads')], c_args: cflags, link_args: ldflags)
  test('gui_rf_preview_handoff', rf_handoff_test)
  rf_idle_cpu_probe = executable('gui_rf_preview_idle_cpu_probe',
    ['test/gui_rf_preview_idle_cpu_probe.c', 'misrc_gui/processing/gui_rf_preview_policy.c'],
    dependencies: [dependency('threads')], c_args: rf_test_cflags, link_args: ldflags)
  test('gui_rf_preview_idle_cpu', rf_idle_cpu_probe)
endif
rf_file_harness = executable('gui_rf_preview_file',
  ['test/gui_rf_preview_file.c', 'misrc_gui/signal/gui_rf_preview.c',
   'misrc_gui/signal/gui_rf_resampler.c', 'misrc_gui/processing/gui_rf_preview_worker.c', 'misrc_gui/signal/gui_beta_hifi.c',
   'misrc_gui/processing/gui_rf_preview_policy.c'],
  dependencies: rf_test_deps, c_args: rf_test_cflags, link_args: ldflags)
rf_marks_test = executable('gui_rf_stream_marks_test',
  ['test/gui_rf_stream_marks_test.c','misrc_gui/processing/gui_rf_stream_marks.c'],
  dependencies: [dependency('threads')], c_args: cflags, link_args: ldflags)
test('gui_rf_stream_marks', rf_marks_test)
rf_pressure_test = executable('gui_rf_preview_pressure_test',
  ['test/gui_rf_preview_pressure_test.c', 'misrc_gui/processing/gui_rf_preview_pressure.c'],
  c_args: cflags)
test('gui_rf_preview_pressure', rf_pressure_test)
rf_policy_test = executable('gui_rf_preview_policy_test',
  ['test/gui_rf_preview_policy_test.c', 'misrc_gui/processing/gui_rf_preview_policy.c'],
  c_args: cflags)
test('gui_rf_preview_policy', rf_policy_test)

if flac_dep.found()
  rf_record_deps = rf_test_deps + [flac_dep]
  if host_machine.system() == 'windows'
    rf_record_deps += [meson.get_compiler('c').find_library('psapi'),
                       meson.get_compiler('c').find_library('onecore')]
  endif
  rf_record_harness = executable('gui_rf_preview_record_load',
    ['test/gui_rf_preview_record_load.c', 'common/buffer_manager.c',
     'common/ringbuffer.c', 'common/rb_event.c', 'common/flac_writer.c',
     'misrc_gui/signal/gui_rf_preview.c', 'misrc_gui/signal/gui_rf_resampler.c',
     'misrc_gui/processing/gui_rf_preview_worker.c', 'misrc_gui/signal/gui_beta_hifi.c',
     'misrc_gui/processing/gui_rf_preview_policy.c',
     'misrc_gui/processing/gui_rf_preview_pressure.c'],
    dependencies: rf_record_deps, c_args: rf_test_cflags, link_args: ldflags)
endif

# Dependency-free DDD protocol/policy tests run even when libusb or raylib is
# unavailable, so the firmware contract remains covered on lean CI hosts.
ddd_protocol_test = executable('ddd_protocol_test',
  ['test/ddd_protocol_test.c', 'common/ddd_protocol.c'],
  dependencies: [],
  c_args: cflags,
)
test('ddd_protocol', ddd_protocol_test)

device_label_test = executable('gui_device_label_test',
  ['test/gui_device_label_test.c', 'misrc_gui/ui/gui_device_label.c'],
  dependencies: [],
  c_args: cflags,
)
test('gui_device_label', device_label_test)

ddd_fifo_status_test = executable('gui_ddd_fifo_status_test',
  ['test/gui_ddd_fifo_status_test.c',
   'misrc_gui/ui/gui_ddd_fifo_status.c',
   'common/ddd_protocol.c'],
  dependencies: [],
  c_args: cflags,
)
test('gui_ddd_fifo_status', ddd_fifo_status_test)

ddd_async_policy_test = executable('gui_ddd_async_policy_test',
  'test/gui_ddd_async_policy_test.c',
  dependencies: [],
  c_args: cflags,
)
test('gui_ddd_async_policy', ddd_async_policy_test)

# Byte-exact direct RAW record path: the real tap push + writer thread
# against the real buffer manager/ringbuffer, with an in-memory spill stub
# (no raylib/GUI dependency).
record_direct_test_deps = [meson.get_compiler('c').find_library('pthread', required: false)]
if host_system == 'windows'
  # The real ringbuffer uses VirtualAlloc2 and MapViewOfFile3.
  record_direct_test_deps += [meson.get_compiler('c').find_library('onecore')]
endif
gui_record_direct_test = executable('gui_record_direct_test',
  ['test/gui_record_direct_harness.c',
   'misrc_gui/output/gui_record_direct.c',
   'common/buffer_manager.c',
   'common/ringbuffer.c',
   'common/rb_event.c'],
  dependencies: record_direct_test_deps,
  c_args: cflags,
)
test('gui_record_direct', gui_record_direct_test)

# CXADC measured feed-rate tier snapping (dependency-free header).
cxadc_rate_tiers_test = executable('cxadc_rate_tiers_test',
  'test/cxadc_rate_tiers_test.c',
  dependencies: [],
  c_args: cflags,
)
test('cxadc_rate_tiers', cxadc_rate_tiers_test)

if ddd_enabled
  ddd_async_fault_test = executable('gui_ddd_async_fault_test',
    ['test/gui_ddd_async_fault_test.c',
     'misrc_gui/input/gui_ddd_async.c',
     'common/ddd_protocol.c'],
    dependencies: [libusb_common_dep],
    c_args: cflags,
  )
  test('gui_ddd_async_fault', ddd_async_fault_test)
endif

gui_text_edit_test = executable('gui_text_edit_test', 'test/gui_text_edit_test.c', c_args: cflags)
test('gui_text_edit', gui_text_edit_test)

# NTSC Beta Hi-Fi streaming contracts; production DSP, no audio device required.
beta_hifi_test = executable('gui_beta_hifi_test',
  ['test/gui_beta_hifi_test.c', 'misrc_gui/signal/gui_beta_hifi.c'],
  c_args: cflags, dependencies: [meson.get_compiler('c').find_library('m', required: false)])
test('gui_beta_hifi', beta_hifi_test, timeout: 120)
beta_hifi_state_test = executable('gui_beta_hifi_state_test',
  'test/gui_beta_hifi_state_test.c',
  c_args: cflags, dependencies: [meson.get_compiler('c').find_library('m', required: false)])
test('gui_beta_hifi_state', beta_hifi_state_test)

if host_system == 'darwin'
  macos_ringbuffer_test = executable('macos_ringbuffer_test',
    ['test/macos_ringbuffer_test.c', 'common/ringbuffer.c'], c_args: cflags)
  test('macos_ringbuffer', macos_ringbuffer_test)
endif

cxadc_samples_test = executable('cxadc_samples_test', 'test/cxadc_samples_test.c', c_args: cflags)
test('cxadc_samples', cxadc_samples_test)
