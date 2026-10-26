/* image.h —— 32 位 DIB 缓冲区、压暗、裁剪、PNG 保存 */
#ifndef IMAGE_H
#define IMAGE_H

#include <windows.h>

typedef struct ImageBuffer {
    int      width;
    int      height;
    int      stride;      /* 字节/行（= width * 4） */
    void    *bits;        /* BGRA，top-down */
    HBITMAP  bitmap;
    HDC      dc;          /* 与 bitmap 关联的内存 DC */
    HDC      ref_dc;      /* 创建 DIB 时用的屏幕 DC，释放时归还 */
} ImageBuffer;

/* 创建 width x height 的 32bpp top-down DIB（全透明/全零）。
   失败时 buffer 归零，返回 FALSE。 */
BOOL image_create(ImageBuffer *img, int width, int height);

void image_free(ImageBuffer *img);

/* RGB 每个通道乘以 factor（alpha 保持不变）。src 与 dst 可以是同一块。 */
void image_darken(const ImageBuffer *src, ImageBuffer *dst, float factor);

/* 从 src 的 (x, y, w, h) 裁剪到 dst（dst 需已创建为 w x h）。 */
BOOL image_crop(const ImageBuffer *src, ImageBuffer *dst, int x, int y, int w, int h);

/* 保存为 PNG。内部会把 BGRA 转成 RGBA（stb 期望的顺序）。 */
BOOL image_save_png(const ImageBuffer *img, const wchar_t *path);

/* 编码成 PNG 放进内存（调用方用 heap_free 释放 *out_data）。 */
BOOL image_encode_png(const ImageBuffer *img, void **out_data, size_t *out_size);

/* 编码成 JPEG 放进内存。quality 取 1..100，建议 85。
   截图内容通常是文字，JPEG 在 q85 下体积远小于 PNG 且不影响 OCR。 */
BOOL image_encode_jpeg(const ImageBuffer *img, int quality,
                       void **out_data, size_t *out_size);

/* 按最大边长等比缩放（整数运算，双线性不够就看最近邻）。
   用于把选区压到模型友好的尺寸。scale 只在需要时创建 dst。
   返回 dst 是否可用；不需要缩放时会把 src 的内容复制到 dst。 */
BOOL image_resize_max_side(const ImageBuffer *src, ImageBuffer *dst, int max_side);

/* 按比例缩放到 dst 已创建好的尺寸。 */
BOOL image_scale_to(const ImageBuffer *src, ImageBuffer *dst);

#endif /* IMAGE_H */
