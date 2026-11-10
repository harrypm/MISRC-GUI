# Osmocom RTL-SDR

- Upstream [Osmocom RTL-SDR](https://github.com/osmocom/rtl-sdr)
- Tag [v2.0.2](https://github.com/osmocom/rtl-sdr/tree/v2.0.2)
- Source commit `619ac3186ea0ffc092615e1f59f7397e5e6f668c` (2024-04-23 UTC)
- GitHub commit archive SHA-256 `bbae67bccc35dae95b77a6559d5f9d09b0526626a86685577661fffd2fd543a1`

The build helper downloads and verifies the pinned archive, then applies
`patches/0001-misrc.patch` before compiling the static I/Q capture library.

The patch adds an Android-only lookup for the upstream supported VID/PID table
and an open function for a USB Host file descriptor. The latter wraps the
descriptor with libusb and uses the same RTL-SDR initialization as indexed
opens. The caller keeps the descriptor open until the library closes its
USB handle. Android capture also uses a bounded synchronous bulk read
that returns partial I/Q data on timeout. This lets capture stop before
the USB handle and descriptor are closed.

The patch also adds `rtlsdr_is_blog_v4` and
`rtlsdr_set_blog_v4_vga_index`. The device check matches the opened
receiver's R828D tuner and cached USB manufacturer/product strings
(`RTLSDRBlog` / `Blog V4`). The setter accepts indices `0x00`–`0x08` in
manual tuner gain mode with the tuner active. The default is `0x08`.
The library retains the selected index for that device handle and reapplies
it on manual gain writes. A failed I²C write restores the register shadow
so the write can be retried. MISRC enables the GUI control on desktop
builds that detect the patched API.

The R82xx tuning path now returns an error if the PLL fails to lock. This
allows MISRC to reject Hi-Fi RF recording until the receiver has a valid
tuned center frequency.

The upstream CMake and pkg-config metadata report 2.0.1 at this tag.
The tag and commit identify the actual source pin. See [COPYING](COPYING)
and the notices in the original source files.
