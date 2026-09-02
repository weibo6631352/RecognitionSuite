#pragma once

// Minimal pdfium C API used by official page_to_image
// Rendering entry point: FPDF_RenderPageBitmap.

#include <stddef.h>

#if defined(_WIN32)
#define FPDF_CALLCONV __stdcall
#else
#define FPDF_CALLCONV
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* FPDF_DOCUMENT;
typedef void* FPDF_PAGE;
typedef void* FPDF_BITMAP;
typedef const char* FPDF_BYTESTRING;
typedef unsigned int FPDF_DWORD;
typedef int FPDF_BOOL;

#define FPDF_ANNOT 0x01
#define FPDFBitmap_BGR 2

void FPDF_CALLCONV FPDF_InitLibrary(void);
void FPDF_CALLCONV FPDF_DestroyLibrary(void);
FPDF_DOCUMENT FPDF_CALLCONV FPDF_LoadMemDocument(const void* data_buf,
                                                 int size,
                                                 FPDF_BYTESTRING password);
void FPDF_CALLCONV FPDF_CloseDocument(FPDF_DOCUMENT document);
FPDF_PAGE FPDF_CALLCONV FPDF_LoadPage(FPDF_DOCUMENT document, int page_index);
void FPDF_CALLCONV FPDF_ClosePage(FPDF_PAGE page);
double FPDF_CALLCONV FPDF_GetPageWidth(FPDF_PAGE page);
double FPDF_CALLCONV FPDF_GetPageHeight(FPDF_PAGE page);
void FPDF_CALLCONV FPDF_RenderPageBitmap(FPDF_BITMAP bitmap,
                                         FPDF_PAGE page,
                                         int start_x,
                                         int start_y,
                                         int size_x,
                                         int size_y,
                                         int rotate,
                                         int flags);
FPDF_BITMAP FPDF_CALLCONV FPDFBitmap_CreateEx(int width,
                                              int height,
                                              int format,
                                              void* first_scan,
                                              int stride);
FPDF_BOOL FPDF_CALLCONV FPDFBitmap_FillRect(FPDF_BITMAP bitmap,
                                            int left,
                                            int top,
                                            int width,
                                            int height,
                                            FPDF_DWORD color);
void* FPDF_CALLCONV FPDFBitmap_GetBuffer(FPDF_BITMAP bitmap);
int FPDF_CALLCONV FPDFBitmap_GetStride(FPDF_BITMAP bitmap);
void FPDF_CALLCONV FPDFBitmap_Destroy(FPDF_BITMAP bitmap);

#ifdef __cplusplus
}
#endif
