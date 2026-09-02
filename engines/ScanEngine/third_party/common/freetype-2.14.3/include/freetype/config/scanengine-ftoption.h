/****************************************************************************
 *
 * scanengine-ftoption.h
 *
 *   Minimal FreeType configuration for ScanEngine's pinned TrueType token
 *   rasterizer.  The upstream defaults are loaded first; optional formats and
 *   external integrations that are outside this use case are then disabled.
 *
 */

#ifndef SCANENGINE_FREETYPE_FTOPTION_H_
#define SCANENGINE_FREETYPE_FTOPTION_H_

#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_SYSTEM_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

#endif /* SCANENGINE_FREETYPE_FTOPTION_H_ */
