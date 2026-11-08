# liba52 0.8.0

MISRC builds the portable single-precision AC-3 decoder from the
[Adélie a52dec 0.8.0 release](https://distfiles.adelielinux.org/source/a52dec/a52dec-0.8.0.tar.gz).
The build verifies the archive SHA-256
`03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9`
before applying [misrc.patch](misrc.patch).

The patch adds a portable decoder configuration, a bounded byte-oriented bit
reader, malformed-frame checks, the bit allocator's log-add endpoint fix,
in-place decoder reset, and thread-safe initialization of shared IMDCT tables.

The original sources by Michel Lespinasse and Aaron Holtzman are licensed
under GPL-2.0-or-later. See [COPYING](COPYING) and the source notices.
