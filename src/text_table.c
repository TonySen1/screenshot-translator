#include "text_table.h"
#include "cJSON.h"
#include "util.h"

#include <wchar.h>

void text_table_free(TextTable *table)
{
    if (table->cells) {
        for (int i = 0; i < table->rows * table->cols; i++) heap_free(table->cells[i]);
        heap_free(table->cells);
    }
    *table = (TextTable){0};
}

BOOL text_table_read(const cJSON *json, TextTable *out)
{
    *out = (TextTable){0};
    const cJSON *widths = cJSON_GetObjectItemCaseSensitive(json, "widths");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(json, "rows");
    if (!cJSON_IsArray(widths) || !cJSON_IsArray(rows)) return FALSE;
    const cJSON *heights = cJSON_GetObjectItemCaseSensitive(json, "heights");
    const cJSON *regions = cJSON_GetObjectItemCaseSensitive(json, "regions");
    int nrows = cJSON_GetArraySize(rows), ncols = cJSON_GetArraySize(widths);
    if (nrows < 1 || nrows > TEXT_TABLE_MAX_ROWS || ncols < 1 || ncols > TEXT_TABLE_MAX_COLS) return FALSE;
    if (ncols == 1 && (!cJSON_IsArray(regions) || cJSON_GetArraySize(regions) != nrows)) return FALSE;
    TextTable table = {0};
    table.rows = nrows;
    table.cols = ncols;
    for (int col = 0; col < ncols; col++) {
        const cJSON *width = cJSON_GetArrayItem(widths, col);
        if (!cJSON_IsNumber(width) || width->valueint < 1 || width->valueint > 8192) return FALSE;
        table.widths[col] = width->valueint;
    }
    table.cells = (wchar_t **)heap_alloc((size_t)nrows * ncols * sizeof(*table.cells));
    if (cJSON_IsArray(regions) && cJSON_GetArraySize(regions) == nrows * ncols) {
        table.regions = (TextRegion *)heap_alloc((size_t)nrows * ncols * sizeof(*table.regions));
        if (!table.regions) goto invalid;
        for (int i = 0; i < nrows * ncols; i++) {
            const cJSON *item = cJSON_GetArrayItem(regions, i);
            const cJSON *bounds = cJSON_GetObjectItemCaseSensitive(item, "bounds");
            const cJSON *limit = cJSON_GetObjectItemCaseSensitive(item, "limit");
            const cJSON *bg = cJSON_GetObjectItemCaseSensitive(item, "background");
            if (!cJSON_IsArray(bounds) || cJSON_GetArraySize(bounds) != 4 ||
                !cJSON_IsArray(limit) || cJSON_GetArraySize(limit) != 4 ||
                !cJSON_IsArray(bg) || cJSON_GetArraySize(bg) != 3) goto invalid;
            LONG *dst[] = {&table.regions[i].bounds.left, &table.regions[i].bounds.top,
                           &table.regions[i].bounds.right, &table.regions[i].bounds.bottom};
            LONG *lim[] = {&table.regions[i].limit.left, &table.regions[i].limit.top,
                           &table.regions[i].limit.right, &table.regions[i].limit.bottom};
            for (int k = 0; k < 4; k++) {
                const cJSON *v = cJSON_GetArrayItem(bounds, k);
                const cJSON *w = cJSON_GetArrayItem(limit, k);
                if (!cJSON_IsNumber(v) || !cJSON_IsNumber(w)) goto invalid;
                *dst[k] = v->valueint; *lim[k] = w->valueint;
            }
            const cJSON *fg = cJSON_GetObjectItemCaseSensitive(item, "foreground");
            if (cJSON_IsArray(fg) && cJSON_GetArraySize(fg) == 3) {
                int frgb[3];
                for (int k = 0; k < 3; k++) {
                    const cJSON *v = cJSON_GetArrayItem(fg, k);
                    if (!cJSON_IsNumber(v)) goto invalid;
                    frgb[k] = v->valueint < 0 ? 0 : v->valueint > 255 ? 255 : v->valueint;
                }
                table.regions[i].foreground = RGB(frgb[0], frgb[1], frgb[2]);
            } else {
                table.regions[i].foreground = RGB(255, 255, 255);
            }
            int rgb[3];
            for (int k = 0; k < 3; k++) {
                const cJSON *v = cJSON_GetArrayItem(bg, k);
                if (!cJSON_IsNumber(v)) goto invalid;
                rgb[k] = v->valueint < 0 ? 0 : v->valueint > 255 ? 255 : v->valueint;
            }
            table.regions[i].background = RGB(rgb[0], rgb[1], rgb[2]);
            table.regions[i].centered = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "centered"));
        }
    }
    if (!table.cells) return FALSE;
    if (cJSON_IsArray(heights) && cJSON_GetArraySize(heights) == nrows) {
        for (int row = 0; row < nrows; row++) {
            const cJSON *height = cJSON_GetArrayItem(heights, row);
            if (cJSON_IsNumber(height) && height->valueint > 0 && height->valueint <= 8192) {
                table.heights[row] = height->valueint;
            }
        }
    }
    for (int row = 0; row < nrows; row++) {
        const cJSON *items = cJSON_GetArrayItem(rows, row);
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) != ncols) goto invalid;
        for (int col = 0; col < ncols; col++) {
            const char *text = cJSON_GetStringValue(cJSON_GetArrayItem(items, col));
            wchar_t **cell = &table.cells[row * ncols + col];
            if (!text || !(*cell = u8_to_u16(text))) goto invalid;
        }
    }
    *out = table;
    return TRUE;
invalid:
    text_table_free(&table);
    return FALSE;
}

wchar_t *text_table_tsv(const TextTable *table)
{
    size_t size = (size_t)table->rows * (table->cols + 1) + 1;
    for (int i = 0; i < table->rows * table->cols; i++) size += wcslen(table->cells[i]);
    wchar_t *out = (wchar_t *)heap_alloc(size * sizeof(*out));
    if (!out) return NULL;
    wchar_t *p = out;
    for (int row = 0; row < table->rows; row++) {
        if (row) { *p++ = L'\r'; *p++ = L'\n'; }
        for (int col = 0; col < table->cols; col++) {
            if (col) *p++ = L'\t';
            const wchar_t *s = table->cells[row * table->cols + col];
            for (; *s; s++) {
                if (*s == L'\r' && s[1] == L'\n') continue;
                *p++ = (*s == L'\t' || *s == L'\r' || *s == L'\n') ? L' ' : *s;
            }
        }
    }
    *p = 0;
    return out;
}
