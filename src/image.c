/* image.c —— DIB 缓冲区操作 */
#include "image.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

BOOL image_create(ImageBuffer *img, int width, int height)
{
    if (img == NULL || width <= 0 || height <= 0) return FALSE;
    memset(img, 0, sizeof(*img));
    img->dc = NULL;
    img->ref_dc = NULL;

    img->ref_dc = GetDC(NULL);
    if (img->ref_dc == NULL) return FALSE;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;      /* 负数 = top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(img->ref_dc, &bmi, DIB_RGB_COLORS,
                                      &bits, NULL, 0);
    if (bitmap == NULL || bits == NULL) {
        if (bitmap) DeleteObject(bitmap);
        ReleaseDC(NULL, img->ref_dc);
        memset(img, 0, sizeof(*img));
        return FALSE;
    }

    HDC dc = CreateCompatibleDC(img->ref_dc);
    if (dc == NULL) {
        DeleteObject(bitmap);
        ReleaseDC(NULL, img->ref_dc);
        memset(img, 0, sizeof(*img));
        return FALSE;
    }
    SelectObject(dc, bitmap);

    img->width = width;
    img->height = height;
    img->stride = width * 4;
    img->bits = bits;
    img->bitmap = bitmap;
    img->dc = dc;
    return TRUE;
}

void image_free(ImageBuffer *img)
{
    if (img == NULL) return;
    if (img->dc) {
        DeleteDC(img->dc);
        img->dc = NULL;
    }
    if (img->bitmap) {
        DeleteObject(img->bitmap);
        img->bitmap = NULL;
    }
    if (img->ref_dc) {
        ReleaseDC(NULL, img->ref_dc);
        img->ref_dc = NULL;
    }
    img->bits = NULL;
    img->width = img->height = img->stride = 0;
}

void image_darken(const ImageBuffer *src, ImageBuffer *dst, float factor)
{
    if (src == NULL || dst == NULL) return;
    if (src->bits == NULL || dst->bits == NULL) return;
    if (src->width != dst->width || src->height != dst->height) return;

    int f = (int)(factor * 256.0f + 0.5f);
    if (f < 0) f = 0;
    if (f > 256) f = 256;

    const unsigned char *s = (const unsigned char *)src->bits;
    unsigned char *d = (unsigned char *)dst->bits;

    int pixels = src->width * src->height;
    for (int i = 0; i < pixels; i++) {
        int o = i * 4;
        /* BGRA；alpha 不动 */
        d[o + 0] = (unsigned char)((s[o + 0] * f) >> 8);
        d[o + 1] = (unsigned char)((s[o + 1] * f) >> 8);
        d[o + 2] = (unsigned char)((s[o + 2] * f) >> 8);
        d[o + 3] = s[o + 3];
    }
}

BOOL image_crop(const ImageBuffer *src, ImageBuffer *dst, int x, int y, int w, int h)
{
    if (src == NULL || dst == NULL) return FALSE;
    if (w <= 0 || h <= 0) return FALSE;
    if (dst->width != w || dst->height != h) return FALSE;

    int sx = x < 0 ? 0 : x;
    int sy = y < 0 ? 0 : y;
    if (sx + w > src->width)  w = src->width - sx;
    if (sy + h > src->height) h = src->height - sy;
    if (w <= 0 || h <= 0) return FALSE;

    return BitBlt(dst->dc, 0, 0, w, h, src->dc, sx, sy, SRCCOPY) ? TRUE : FALSE;
}

/* stb 的回调：直接写文件 */
static void png_write_to_file(void *context, void *data, int size)
{
    HANDLE file = (HANDLE)context;
    DWORD written = 0;
    if (size > 0) WriteFile(file, data, (DWORD)size, &written, NULL);
}

/* 把 BGRA 的 DIB 转成 RGBA（stb 期望的顺序），截图本身不透明 */
static unsigned char *to_rgba(const ImageBuffer *img)
{
    int w = img->width;
    int h = img->height;
    if (w <= 0 || h <= 0) return NULL;

    unsigned char *rgba = (unsigned char *)heap_alloc((size_t)w * (size_t)h * 4);
    if (rgba == NULL) return NULL;

    const unsigned char *s = (const unsigned char *)img->bits;
    for (int i = 0; i < w * h; i++) {
        int o = i * 4;
        rgba[o + 0] = s[o + 2];      /* R <- B */
        rgba[o + 1] = s[o + 1];      /* G */
        rgba[o + 2] = s[o + 0];      /* B <- R */
        rgba[o + 3] = 255;           /* 截图不透明 */
    }
    return rgba;
}

BOOL image_save_png(const ImageBuffer *img, const wchar_t *path)
{
    if (img == NULL || img->bits == NULL || path == NULL) return FALSE;

    int w = img->width;
    int h = img->height;
    if (w <= 0 || h <= 0) return FALSE;

    unsigned char *rgba = to_rgba(img);
    if (rgba == NULL) return FALSE;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        heap_free(rgba);
        return FALSE;
    }

    int ok = stbi_write_png_to_func(png_write_to_file, (void *)file,
                                    w, h, 4, rgba, w * 4);
    CloseHandle(file);
    heap_free(rgba);

    if (!ok) {
        DeleteFileW(path);
        return FALSE;
    }
    return TRUE;
}

/* stb 的回调：追加到可增长的内存缓冲 */
typedef struct MemoryWriter {
    unsigned char *data;
    size_t         size;
    size_t         capacity;
    BOOL           failed;
} MemoryWriter;

static void png_write_to_memory(void *context, void *data, int size)
{
    MemoryWriter *mw = (MemoryWriter *)context;
    if (mw->failed || size <= 0) return;

    size_t need = mw->size + (size_t)size;
    if (need > mw->capacity) {
        size_t next = mw->capacity ? mw->capacity : 65536;
        while (next < need) next *= 2;
        unsigned char *grown = (unsigned char *)heap_realloc(mw->data, next);
        if (grown == NULL) { mw->failed = TRUE; return; }
        mw->data = grown;
        mw->capacity = next;
    }
    memcpy(mw->data + mw->size, data, (size_t)size);
    mw->size = need;
}

BOOL image_encode_png(const ImageBuffer *img, void **out_data, size_t *out_size)
{
    if (img == NULL || img->bits == NULL ||
        out_data == NULL || out_size == NULL) {
        return FALSE;
    }
    *out_data = NULL;
    *out_size = 0;

    int w = img->width;
    int h = img->height;
    if (w <= 0 || h <= 0) return FALSE;

    unsigned char *rgba = to_rgba(img);
    if (rgba == NULL) return FALSE;

    MemoryWriter mw;
    memset(&mw, 0, sizeof(mw));

    int ok = stbi_write_png_to_func(png_write_to_memory, &mw,
                                    w, h, 4, rgba, w * 4);
    heap_free(rgba);

    if (!ok || mw.failed || mw.data == NULL) {
        heap_free(mw.data);
        return FALSE;
    }

    *out_data = mw.data;
    *out_size = mw.size;
    return TRUE;
}

BOOL image_encode_jpeg(const ImageBuffer *img, int quality,
                       void **out_data, size_t *out_size)
{
    if (img == NULL || img->bits == NULL ||
        out_data == NULL || out_size == NULL) {
        return FALSE;
    }
    *out_data = NULL;
    *out_size = 0;

    int w = img->width;
    int h = img->height;
    if (w <= 0 || h <= 0) return FALSE;

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    /* JPEG 只保留 RGB；DIB 是 BGRA，按 3 字节打包 */
    size_t pixels = (size_t)w * (size_t)h;
    unsigned char *rgb = (unsigned char *)heap_alloc(pixels * 3);
    if (rgb == NULL) return FALSE;

    const unsigned char *s = (const unsigned char *)img->bits;
    for (size_t i = 0; i < pixels; i++) {
        size_t src = i * 4;
        size_t dst = i * 3;
        rgb[dst + 0] = s[src + 2];   /* R <- B */
        rgb[dst + 1] = s[src + 1];   /* G */
        rgb[dst + 2] = s[src + 0];   /* B <- R */
    }

    MemoryWriter mw;
    memset(&mw, 0, sizeof(mw));

    int ok = stbi_write_jpg_to_func(png_write_to_memory, &mw,
                                    w, h, 3, rgb, quality);
    heap_free(rgb);

    if (!ok || mw.failed || mw.data == NULL) {
        heap_free(mw.data);
        return FALSE;
    }

    *out_data = mw.data;
    *out_size = mw.size;
    return TRUE;
}

BOOL image_scale_to(const ImageBuffer *src, ImageBuffer *dst)
{
    if (src == NULL || dst == NULL) return FALSE;
    if (src->bits == NULL || dst->bits == NULL) return FALSE;
    if (src->width <= 0 || src->height <= 0) return FALSE;
    if (dst->width <= 0 || dst->height <= 0) return FALSE;

    const unsigned char *s = (const unsigned char *)src->bits;
    unsigned char *d = (unsigned char *)dst->bits;

    /* 双线性插值。用 16.16 定点避免浮点转换开销。 */
    int sw = src->width, sh = src->height;
    int dw = dst->width, dh = dst->height;

    long long x_step = ((long long)(sw - 1) << 16) / (dw > 1 ? dw - 1 : 1);
    long long y_step = ((long long)(sh - 1) << 16) / (dh > 1 ? dh - 1 : 1);

    for (int y = 0; y < dh; y++) {
        long long fy = y_step * y;
        int y0 = (int)(fy >> 16);
        int wy = (int)(fy & 0xFFFF);
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;

        const unsigned char *row0 = s + (size_t)y0 * src->stride;
        const unsigned char *row1 = s + (size_t)y1 * src->stride;

        for (int x = 0; x < dw; x++) {
            long long fx = x_step * x;
            int x0 = (int)(fx >> 16);
            int wx = (int)(fx & 0xFFFF);
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;

            int o00 = x0 * 4, o01 = x1 * 4;
            int out = (y * dw + x) * 4;

            for (int c = 0; c < 4; c++) {
                int p00 = row0[o00 + c], p01 = row0[o01 + c];
                int p10 = row1[o00 + c], p11 = row1[o01 + c];
                int top = p00 + (((p01 - p00) * wx) >> 16);
                int bottom = p10 + (((p11 - p10) * wx) >> 16);
                int value = top + (((bottom - top) * wy) >> 16);
                if (value < 0) value = 0;
                if (value > 255) value = 255;
                d[out + c] = (unsigned char)value;
            }
        }
    }
    return TRUE;
}

BOOL image_resize_max_side(const ImageBuffer *src, ImageBuffer *dst, int max_side)
{
    if (src == NULL || dst == NULL || max_side <= 0) return FALSE;
    if (src->bits == NULL || src->width <= 0 || src->height <= 0) return FALSE;

    int w = src->width;
    int h = src->height;
    int longest = w > h ? w : h;

    if (longest <= max_side) {
        /* 不需要缩放，原样复制一份 */
        if (!image_create(dst, w, h)) return FALSE;
        if (!BitBlt(dst->dc, 0, 0, w, h, src->dc, 0, 0, SRCCOPY)) {
            image_free(dst);
            return FALSE;
        }
        return TRUE;
    }

    double scale = (double)max_side / (double)longest;
    int nw = (int)(w * scale + 0.5);
    int nh = (int)(h * scale + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    if (!image_create(dst, nw, nh)) return FALSE;
    if (!image_scale_to(src, dst)) {
        image_free(dst);
        return FALSE;
    }
    return TRUE;
}
