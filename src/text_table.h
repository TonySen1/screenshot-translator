#ifndef TEXT_TABLE_H
#define TEXT_TABLE_H

#include <windows.h>

#define TEXT_TABLE_MAX_COLS 4
#define TEXT_TABLE_MAX_ROWS 64
#define TEXT_TABLE_MAX_CELLS (TEXT_TABLE_MAX_ROWS * TEXT_TABLE_MAX_COLS)

typedef struct TextRegion {
    RECT bounds;
    RECT limit;
    COLORREF background;
    COLORREF foreground;
    BOOL centered;
} TextRegion;

/* cells 按行排列；regions 与 cells 一一对应，可为空。 */
typedef struct TextTable {
    int rows, cols;
    int widths[TEXT_TABLE_MAX_COLS];
    wchar_t **cells;
    int heights[TEXT_TABLE_MAX_ROWS];
    TextRegion *regions;
} TextTable;

struct cJSON;
#ifdef __cplusplus
extern "C" {
#endif
BOOL text_table_read(const struct cJSON *json, TextTable *out);
void text_table_free(TextTable *table);
/* 复制给表格软件：内嵌换行、制表符转空格，保留单元格边界。 */
wchar_t *text_table_tsv(const TextTable *table);
#ifdef __cplusplus
}
#endif

#endif
