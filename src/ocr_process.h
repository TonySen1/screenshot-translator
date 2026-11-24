/* ocr_process.h —— 常驻 PaddleOCR 子进程的本地管道协议 */
#ifndef OCR_PROCESS_H
#define OCR_PROCESS_H

#include "image.h"

struct cJSON;
typedef struct OcrResult {
    wchar_t *text;
    wchar_t *error;
    struct cJSON *lines;  /* 行文本和相对选区的四点坐标，供后续原位排版使用 */
    struct cJSON *table;  /* 可靠识别行列时才存在 */
} OcrResult;

/* 首次在主线程启动，不等待加载。之后由单个翻译线程独占调用。 */
void ocr_preload(const wchar_t *exe_dir);
BOOL ocr_recognize(const ImageBuffer *image, OcrResult *out);
void ocr_result_free(OcrResult *out);
/* 仅在没有翻译线程使用时调用。异常退出由系统关闭 Job 并回收子进程。 */
void ocr_stop(void);

#endif
