# FreeType 2.14.3 source subset

This directory contains the source subset used by ScanEngine to reproduce the
font rasterization performed by Pillow's pinned FreeType 2.14.3 build.  It is
compiled from source on both Windows and macOS; no host FreeType or Qt-private
FreeType binary is used.

## Upstream lock

- Version: `2.14.3`
- Archive: `freetype-2.14.3.tar.xz`
- Official URL:
  `https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz`
- Archive size: `2,670,220` bytes
- SHA-256:
  `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`

The files under `include/`, `src/base/`, `src/sfnt/`, `src/truetype/`, and
`src/smooth/` are copied from that archive.  Only the TrueType driver, SFNT
module, and anti-aliased smooth renderer are registered in `ftmodule.h`.
`scanengine-ftoption.h` layers the project configuration over the original
upstream `ftoption.h` and disables LZW, zlib, bzip2, PNG, Brotli, HarfBuzz,
SVG, and legacy Mac resource-fork support.  Those formats and integrations are
not used when rendering the pinned Arial TrueType font.

The project builds these upstream translation units:

- `src/base/ftsystem.c`
- `src/base/ftdebug.c`
- `src/base/ftinit.c`
- `src/base/ftbase.c`
- `src/base/ftbitmap.c`
- `src/base/ftglyph.c`
- `src/base/ftmm.c`
- `src/sfnt/sfnt.c`
- `src/truetype/truetype.c`
- `src/smooth/smooth.c`

On Windows, the static library inherits the repository-wide dynamic CRT
selection (`/MD` in Release, `/MDd` in Debug).  The same source and module
configuration are used by the macOS build.

## License

FreeType is dual-licensed.  ScanEngine uses the BSD-style FreeType License (FTL),
whose required notices are preserved in `LICENSE.TXT` and `docs/FTL.TXT`.
The upstream alternative `docs/GPLv2.TXT` is retained as well.  See
`https://freetype.org/license.html` for the upstream licensing summary.

## Parity oracle

The Windows reference font used for the pinned comparison is
`C:\Windows\Fonts\arial.ttf`, size `1,045,960` bytes, SHA-256
`baa251526d6862712a58e613ef451d8a2b60482142ec6aab1d47fb8e23e21a7c`.
The source subset above reports FreeType `2.14.3` and matches the pinned Python
processor for all 24 representative four-character tokens at 0, 90, 180, and
270 degrees: 96/96 masked RGB cases and 96/96 embedded JPEG byte streams are
exact.
