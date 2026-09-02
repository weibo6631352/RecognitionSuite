/* Portable libjpeg jconfig for ScanEngine's platform compilers.

   Do not copy jpeg-9f jconfig.vc: its Windows BGR custom order does not match
   the official RGB 4:4:4 (Pillow) encode used as the Hybrid oracle.
   JPEG_USE_RGB_CUSTOM stays unset so RGB_RED=0 / RGB_BLUE=2. */

#define HAVE_PROTOTYPES
#define HAVE_UNSIGNED_CHAR
#define HAVE_UNSIGNED_SHORT
#define HAVE_STDDEF_H
#define HAVE_STDLIB_H
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN

#ifndef HAVE_BOOLEAN
typedef unsigned char boolean;
#define HAVE_BOOLEAN
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#ifdef JPEG_INTERNALS
#undef RIGHT_SHIFT_IS_UNSIGNED
#endif
