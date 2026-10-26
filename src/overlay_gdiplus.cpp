/* overlay_gdiplus.cpp —— GDI+ 测量与绘制（唯一的 C++ 编译单元） */
#include "overlay_gdiplus.h"

#include <objidl.h>
#include <gdiplus.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

using namespace Gdiplus;

static ULONG_PTR g_gdiplus_token = 0;
static BOOL      g_gdiplus_ready = FALSE;

/* ---------- 颜色 ---------- */

static Color argb_to_color(unsigned int argb)
{
    return Color((BYTE)((argb >> 24) & 0xFF),
                 (BYTE)((argb >> 16) & 0xFF),
                 (BYTE)((argb >> 8) & 0xFF),
                 (BYTE)(argb & 0xFF));
}

/* ---------- 圆角矩形路径 ---------- */

static void add_round_rect(GraphicsPath *path, const RectF &r, float radius)
{
    if (radius <= 0.0f) {
        path->AddRectangle(r);
        return;
    }

    float d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;

    path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path->AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path->AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
}

/* ---------- 字体 ---------- */

static Font *make_font(const WCHAR *const *families, int count, int pixel_size)
{
    for (int i = 0; i < count; i++) {
        FontFamily family(families[i]);
        if (family.GetLastStatus() != Ok) continue;

        Font *font = new Font(&family, (REAL)pixel_size, FontStyleRegular, UnitPixel);
        if (font->GetLastStatus() == Ok) return font;
        delete font;
    }
    return NULL;
}

static Font *create_body_font(int pixel_size)
{
    /* 中文优先雅黑；没有就退回 Segoe UI。 */
    static const WCHAR *families[] = { L"Microsoft YaHei UI", L"Microsoft YaHei",
                                       L"Segoe UI", L"Arial" };
    Font *font = make_font(families, 4, pixel_size);
    if (font != NULL) return font;

    FontFamily fallback(L"Arial");
    return new Font(&fallback, (REAL)pixel_size, FontStyleRegular, UnitPixel);
}

static Font *create_emoji_font(int pixel_size)
{
    /* GDI+ 不做字体回退，emoji 码位必须显式换成 emoji 字体，
       否则雅黑会画成豆腐块。 */
    static const WCHAR *families[] = { L"Segoe UI Emoji", L"Segoe UI Symbol" };
    return make_font(families, 2, pixel_size);
}

/* ---------- 文本单元切分 ---------- */

#define FONT_BODY   0
#define FONT_EMOJI  1

#define KIND_LATIN   0
#define KIND_SPACE   1
#define KIND_CJK     2
#define KIND_EMOJI   3
#define KIND_NEWLINE 4

#define LAYOUT_MAX_UNITS 8192

typedef struct TextUnit {
    int start;
    int len;
    int font;
    int kind;
} TextUnit;

typedef struct TextRun {
    int   start;
    int   len;
    int   font;
    float x;
    float y;
} TextRun;

typedef struct TextLayout {
    TextUnit *units;
    int       unit_count;

    TextRun  *runs;
    int       run_count;

    float     width;
    float     height;
} TextLayout;

static int decode_cp(const WCHAR *s, int *advance)
{
    unsigned int c = (unsigned int)s[0];
    if (c >= 0xD800 && c <= 0xDBFF &&
        s[1] >= (WCHAR)0xDC00 && s[1] <= (WCHAR)0xDFFF) {
        *advance = 2;
        return (int)(0x10000 + ((c - 0xD800) << 10) +
                     ((unsigned int)s[1] - 0xDC00));
    }
    *advance = 1;
    return (int)c;
}

static BOOL is_emoji_cp(int cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||   /* pictographs / symbols */
           (cp >= 0x2600  && cp <= 0x27BF)  ||   /* misc symbols, dingbats */
           (cp >= 0x2B00  && cp <= 0x2BFF)  ||   /* arrows, shapes */
           (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||   /* regional indicators */
           (cp >= 0xFE00  && cp <= 0xFE0F)  ||   /* variation selectors */
           (cp >= 0x1F3FB && cp <= 0x1F3FF) ||   /* skin tones */
           cp == 0x200D || cp == 0x20E3 || cp == 0x2139;
}

static BOOL is_combining_cp(int cp)
{
    return cp == 0x200D || cp == 0x20E3 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

static BOOL is_cjk_cp(int cp)
{
    return (cp >= 0x2E80  && cp <= 0x9FFF)  ||
           (cp >= 0xF900  && cp <= 0xFAFF)  ||
           (cp >= 0xFE30  && cp <= 0xFE4F)  ||
           (cp >= 0xFF00  && cp <= 0xFFEF)  ||
           (cp >= 0x3000  && cp <= 0x303F);
}

static void build_units(const WCHAR *text, TextUnit *units, int max_units,
                        int *out_count)
{
    int n = 0;
    int i = 0;

    while (text[i] != 0 && n < max_units) {
        int adv = 1;
        int cp = decode_cp(text + i, &adv);

        /* 组合符号并回前一个单元，保证 ZWJ 序列不被拆开 */
        if (n > 0 && is_combining_cp(cp)) {
            units[n - 1].len += adv;
            i += adv;
            continue;
        }

        int kind;
        if (cp == L'\r') {
            /* CRLF 当成一个换行 */
            if (text[i + 1] == L'\n') adv = 2;
            kind = KIND_NEWLINE;
        } else if (cp == L'\n') {
            kind = KIND_NEWLINE;
        } else if (is_emoji_cp(cp)) {
            kind = KIND_EMOJI;
        } else if (cp == L' ' || cp == L'\t') {
            kind = KIND_SPACE;
        } else if (is_cjk_cp(cp)) {
            kind = KIND_CJK;
        } else {
            kind = KIND_LATIN;
        }

        /* 连续的拉丁字母/数字合成一个不可拆的词；连续空格也合并 */
        if (n > 0 && units[n - 1].kind == kind &&
            (kind == KIND_LATIN || kind == KIND_SPACE)) {
            units[n - 1].len += adv;
            i += adv;
            continue;
        }

        units[n].start = i;
        units[n].len   = adv;
        units[n].font  = (kind == KIND_EMOJI) ? FONT_EMOJI : FONT_BODY;
        units[n].kind  = kind;
        n++;
        i += adv;
    }

    /* 丢掉末尾多余的空格，避免右侧多出一截空白 */
    while (n > 0 && units[n - 1].kind == KIND_SPACE) n--;

    *out_count = n;
}

/* ---------- 排版 ---------- */

static void layout_init(TextLayout *lay)
{
    lay->units      = (TextUnit *)malloc(sizeof(TextUnit) * LAYOUT_MAX_UNITS);
    lay->runs       = (TextRun  *)malloc(sizeof(TextRun)  * LAYOUT_MAX_UNITS);
    lay->unit_count = 0;
    lay->run_count  = 0;
    lay->width      = 0.0f;
    lay->height     = 0.0f;
}

static void layout_free(TextLayout *lay)
{
    free(lay->units);
    free(lay->runs);
    memset(lay, 0, sizeof(*lay));
}

/* 量一个 run 的精确宽度。DrawString 画的是整段文本，
   所以行宽也必须用整段来量 —— 逐单元累加会偏小，
   导致最后几个字被文本框裁掉。 */
static float measure_run(Graphics *g, const WCHAR *text, int start, int len,
                         Font *font, StringFormat *fmt)
{
    if (len <= 0) return 0.0f;

    RectF box;
    if (g->MeasureString(text + start, len, font, PointF(0.0f, 0.0f),
                         fmt, &box) != Ok) {
        return 0.0f;
    }
    return box.Width;
}

static BOOL layout_text(Graphics *g, const WCHAR *text,
                        Font *body, Font *emoji, StringFormat *unit_format,
                        float max_width, TextLayout *out)
{
    if (out->units == NULL || out->runs == NULL) return FALSE;

    build_units(text, out->units, LAYOUT_MAX_UNITS, &out->unit_count);
    if (out->unit_count <= 0) return FALSE;

    Font *fonts[2] = { body, emoji };

    /* 行高用正文字体；emoji 在行内垂直居中，不会把行撑歪 */
    float line_h = (float)body->GetHeight(g);
    if (line_h <= 0.0f) line_h = (float)body->GetSize();

    float pen_x = 0.0f;
    float pen_y = 0.0f;
    float max_line_w = 0.0f;
    BOOL  line_empty = TRUE;

    for (int i = 0; i < out->unit_count; i++) {
        const TextUnit *u = &out->units[i];

        if (u->kind == KIND_NEWLINE) {
            if (pen_x > max_line_w) max_line_w = pen_x;
            pen_x = 0.0f;
            pen_y += line_h;
            line_empty = TRUE;
            continue;
        }

        Font *font = fonts[u->font];
        if (font == NULL) font = body;

        RectF box;
        if (g->MeasureString(text + u->start, u->len, font,
                             PointF(0.0f, 0.0f), unit_format, &box) != Ok) {
            continue;
        }
        float w = box.Width;

        /* 行首空格直接丢掉 */
        if (u->kind == KIND_SPACE && line_empty) continue;

        /* 放不下就换行。判断用的是"把这一单元接到当前行尾之后，
           整行有多宽" —— 必须整段量，逐单元累加会偏小。 */
        if (!line_empty) {
            float y_probe = pen_y + (line_h - box.Height) * 0.5f;
            int run_index = out->run_count - 1;

            BOOL same_run = (run_index >= 0 &&
                             out->runs[run_index].font == u->font &&
                             out->runs[run_index].start +
                                 out->runs[run_index].len == u->start &&
                             fabsf(out->runs[run_index].y - y_probe) < 0.01f);

            float line_w = same_run
                ? measure_run(g, text, out->runs[run_index].start,
                              out->runs[run_index].len + u->len,
                              font, unit_format)
                : pen_x + w;

            if (line_w > max_width) {
                if (pen_x > max_line_w) max_line_w = pen_x;
                pen_x = 0.0f;
                pen_y += line_h;
                line_empty = TRUE;
                if (u->kind == KIND_SPACE) continue;
            }
        }

        /* box.Y 是 MeasureString 给出的基线偏移（通常为负），
           减去它会让整行跟着上移，上下留白就不对称了。
           这里让每一行在自己的 line_h 里垂直居中。 */
        float glyph_h = box.Height;
        float y = pen_y + (line_h - glyph_h) * 0.5f;

        /* 同一行、同一字体、字符连续 —— 合成一个 run，少调几次 DrawString。 */
        if (out->run_count > 0 && out->run_count < LAYOUT_MAX_UNITS) {
            TextRun *last = &out->runs[out->run_count - 1];
            if (last->font == u->font &&
                last->start + last->len == u->start &&
                fabsf(last->y - y) < 0.01f) {
                last->len += u->len;
                pen_x += w;
                line_empty = FALSE;
                continue;
            }
        }

        if (out->run_count >= LAYOUT_MAX_UNITS) break;

        TextRun *run = &out->runs[out->run_count++];
        run->start = u->start;
        run->len   = u->len;
        run->font  = u->font;
        run->x     = pen_x;
        run->y     = y;

        pen_x += w;
        line_empty = FALSE;
    }

    if (pen_x > max_line_w) max_line_w = pen_x;

    /* 关键：宽度按每行合并后的 run 重新量一遍。
       逐单元累加偏小，会让最后几个字被裁掉。 */
    {
        float line_top = -1.0f;
        float line_w = 0.0f;
        float best = 0.0f;

        for (int i = 0; i < out->run_count; i++) {
            const TextRun *r = &out->runs[i];
            if (line_top < 0.0f || fabsf(r->y - line_top) > 0.01f) {
                if (line_w > best) best = line_w;
                line_top = r->y;
                line_w = r->x;
            }
            Font *font = (r->font == FONT_EMOJI && emoji != NULL) ? emoji : body;
            float end_x = r->x + measure_run(g, text, r->start, r->len,
                                             font, unit_format);
            if (end_x > line_w) line_w = end_x;
        }
        if (line_w > best) best = line_w;

        out->width = best;
    }

    out->height = pen_y + (line_empty ? 0.0f : line_h);
    return out->run_count > 0;
}

static void setup_unit_format(StringFormat *fmt)
{
    /* 单元级绘制/测量：不换行、不裁切、尾随空格计入宽度 */
    fmt->SetFormatFlags(StringFormatFlagsNoWrap |
                        StringFormatFlagsNoClip |
                        StringFormatFlagsMeasureTrailingSpaces);
    fmt->SetTrimming(StringTrimmingNone);
    fmt->SetAlignment(StringAlignmentNear);
    fmt->SetLineAlignment(StringAlignmentNear);
}

/* ---------- 工具栏按钮 ---------- */

typedef struct ToolbarLayout {
    RectF text_box;      /* 文本区（含 1px 边框） */
    RectF toolbar;       /* 胶囊工具栏，贴在文本区右下角外侧 */
    RectF buttons[4];    /* 索引 1..3 有意义，对应 OVERLAY_BTN_* */
} ToolbarLayout;

static void compute_layout(const OverlayTheme *theme, float width, float height,
                           ToolbarLayout *out)
{
    float pad = (float)theme->padding;

    /* 整个窗口只显示文字框；工具栏画在它下方外侧留出的那块空白里 */
    out->text_box.X = pad;
    out->text_box.Y = pad;
    out->text_box.Width  = width - pad * 2.0f;
    out->text_box.Height = height - pad * 2.0f
                           - (float)theme->gap
                           - (float)theme->toolbar_h;
    if (out->text_box.Height < 1.0f) out->text_box.Height = 1.0f;

    /* 胶囊贴在文字框的右下角外侧 */
    float btn = (float)theme->button_size;
    float inner_pad = ((float)theme->toolbar_h - btn) * 0.5f;
    const int count = 3;

    out->toolbar.Width  = btn * (float)count + inner_pad * 2.0f;
    out->toolbar.Height = (REAL)theme->toolbar_h;
    out->toolbar.X = out->text_box.X + out->text_box.Width - out->toolbar.Width;
    out->toolbar.Y = out->text_box.Y + out->text_box.Height
                     + (float)theme->gap;

    if (out->toolbar.X < out->text_box.X) out->toolbar.X = out->text_box.X;

    for (int i = 0; i < 4; i++) {
        out->buttons[i].Width = btn;
        out->buttons[i].Height = btn;
        out->buttons[i].X = out->toolbar.X + inner_pad + btn * (float)(i - 1);
        out->buttons[i].Y = out->toolbar.Y + inner_pad;
    }
}

/* 文档/复制两个图标原来几乎占满整个按钮框，而 X 因为有内缩，
   实际笔画只占 56% 左右，看上去就小一圈。
   这里给它们同样的内缩，三个图标的视觉大小才一致。 */
static RectF inset_icon(const RectF &box, float ratio)
{
    float dx = box.Width  * ratio;
    float dy = box.Height * ratio;
    return RectF(box.X + dx, box.Y + dy,
                 box.Width - dx * 2.0f, box.Height - dy * 2.0f);
}

static void draw_icon_doc(Graphics *g, Pen *pen, const RectF &outer)
{
    /* 文档：一个竖长方形 + 右上角缺角 + 三条横线 */
    RectF box = inset_icon(outer, 0.20f);
    float w = box.Width;
    float h = box.Height;
    float fold = w * 0.32f;

    PointF pts[6];
    pts[0] = PointF(box.X, box.Y);
    pts[1] = PointF(box.X + w - fold, box.Y);
    pts[2] = PointF(box.X + w, box.Y + fold);
    pts[3] = PointF(box.X + w, box.Y + h);
    pts[4] = PointF(box.X, box.Y + h);
    pts[5] = PointF(box.X, box.Y);
    g->DrawLines(pen, pts, 6);

    /* 折角 */
    PointF fold_pts[3];
    fold_pts[0] = PointF(box.X + w - fold, box.Y);
    fold_pts[1] = PointF(box.X + w - fold, box.Y + fold);
    fold_pts[2] = PointF(box.X + w, box.Y + fold);
    g->DrawLines(pen, fold_pts, 3);

    for (int i = 0; i < 3; i++) {
        float y = box.Y + h * (0.46f + 0.19f * (float)i);
        g->DrawLine(pen, box.X + w * 0.22f, y, box.X + w * 0.78f, y);
    }
}

static void draw_icon_copy(Graphics *g, Pen *pen, const RectF &outer)
{
    /* 复制：两个错开叠放的圆角矩形 */
    RectF box = inset_icon(outer, 0.20f);
    float w = box.Width;
    float h = box.Height;
    float off = w * 0.22f;

    GraphicsPath back;
    add_round_rect(&back, RectF(box.X + off, box.Y, w - off, h - off), w * 0.12f);
    g->DrawPath(pen, &back);

    GraphicsPath front;
    add_round_rect(&front, RectF(box.X, box.Y + off, w - off, h - off), w * 0.12f);
    g->DrawPath(pen, &front);
}

static void draw_icon_close(Graphics *g, Pen *pen, const RectF &box)
{
    float inset = box.Width * 0.26f;
    float x0 = box.X + inset, y0 = box.Y + inset;
    float x1 = box.X + box.Width - inset, y1 = box.Y + box.Height - inset;
    g->DrawLine(pen, x0, y0, x1, y1);
    g->DrawLine(pen, x1, y0, x0, y1);
}

static void draw_toolbar(Graphics *g, const OverlayTheme *theme,
                         const ToolbarLayout *lay,
                         int hover_button, int pressed_button, int doc_enabled)
{
    /* 胶囊底 */
    GraphicsPath pill;
    /* 圆角用固定值而不是高度的一半：半高会画成完整胶囊，
       在这么矮的工具栏上显得过于圆。 */
    float pill_radius = (float)theme->toolbar_radius;
    if (pill_radius > lay->toolbar.Height * 0.5f) {
        pill_radius = lay->toolbar.Height * 0.5f;
    }
    add_round_rect(&pill, lay->toolbar, pill_radius);
    /* 工具栏底色跟着文字框走，两者用同一个半透明色：
       分成两个色阶时，在半透明背景下会看出明显色差。 */
    SolidBrush pill_bg(argb_to_color(theme->bg_argb));
    g->FillPath(&pill_bg, &pill);

    /* 和译文框一样描一条 1px 边，两块区域的质感才统一 */
    {
        Pen pill_border(argb_to_color(theme->border_argb), 1.0f);
        g->DrawPath(&pill_border, &pill);
    }

    for (int id = OVERLAY_BTN_DOC; id <= OVERLAY_BTN_CLOSE; id++) {
        const RectF *btn = &lay->buttons[id];
        BOOL hovered = (hover_button == id);
        BOOL pressed = (pressed_button == id) && hovered;

        if (hovered) {
            GraphicsPath hl;
            add_round_rect(&hl, *btn, (REAL)theme->button_radius);
            SolidBrush hl_bg(argb_to_color(pressed ? theme->icon_press_argb
                                                   : theme->icon_bg_argb));
            g->FillPath(&hl_bg, &hl);
        }

        float icon_w = (float)theme->icon_size;
        RectF icon(btn->X + (btn->Width - icon_w) * 0.5f,
                   btn->Y + (btn->Height - icon_w) * 0.5f,
                   icon_w, icon_w);

        /* 关闭按钮用得最多，画得比其他两个粗一点、亮一点，
           一眼就能找到。 */
        BOOL is_close = (id == OVERLAY_BTN_CLOSE);

        unsigned int color;
        if (id == OVERLAY_BTN_DOC && !doc_enabled) {
            color = theme->icon_disabled_argb;
        } else if (is_close) {
            color = 0xFFFFFFFF;      /* 关闭按钮常态就是纯白 */
        } else {
            color = hovered ? theme->icon_hover_argb : theme->icon_argb;
        }

        Pen pen(argb_to_color(color), is_close ? 2.2f : 1.6f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);

        switch (id) {
        case OVERLAY_BTN_DOC:   draw_icon_doc(g, &pen, icon);   break;
        case OVERLAY_BTN_COPY:  draw_icon_copy(g, &pen, icon);  break;
        case OVERLAY_BTN_CLOSE: draw_icon_close(g, &pen, icon); break;
        default: break;
        }
    }
}

void overlay_button_rects(const OverlayTheme *theme, int width, int height,
                          RECT *out_doc, RECT *out_copy, RECT *out_close)
{
    if (theme == NULL) return;

    ToolbarLayout lay;
    compute_layout(theme, (float)width, (float)height, &lay);

    RECT *outs[4] = { NULL, out_doc, out_copy, out_close };
    for (int i = 1; i < 4; i++) {
        if (outs[i] == NULL) continue;
        outs[i]->left   = (LONG)floorf(lay.buttons[i].X);
        outs[i]->top    = (LONG)floorf(lay.buttons[i].Y);
        outs[i]->right  = (LONG)ceilf(lay.buttons[i].X + lay.buttons[i].Width);
        outs[i]->bottom = (LONG)ceilf(lay.buttons[i].Y + lay.buttons[i].Height);
    }
}

/* ---------- 导出 ---------- */

BOOL overlay_gdiplus_startup(void)
{
    if (g_gdiplus_ready) return TRUE;

    GdiplusStartupInput input;
    input.GdiplusVersion = 1;
    input.DebugEventCallback = NULL;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = FALSE;

    if (GdiplusStartup(&g_gdiplus_token, &input, NULL) != Ok) {
        g_gdiplus_token = 0;
        return FALSE;
    }
    g_gdiplus_ready = TRUE;
    return TRUE;
}

void overlay_gdiplus_shutdown(void)
{
    if (!g_gdiplus_ready) return;
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    g_gdiplus_ready = FALSE;
}

BOOL overlay_measure(const wchar_t *text, const OverlayTheme *theme,
                     int *out_width, int *out_height)
{
    if (!g_gdiplus_ready || text == NULL || theme == NULL ||
        out_width == NULL || out_height == NULL) {
        return FALSE;
    }

    HDC screen = GetDC(NULL);
    if (screen == NULL) return FALSE;

    BOOL ok = FALSE;
    {
        Graphics g(screen);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        Font *body = create_body_font(theme->font_px);
        Font *emoji = create_emoji_font(theme->font_px);

        if (body != NULL && body->GetLastStatus() == Ok) {
            StringFormat fmt;
            setup_unit_format(&fmt);

            TextLayout lay;
            layout_init(&lay);
            if (layout_text(&g, text, body, emoji, &fmt,
                            (float)theme->max_text_width, &lay)) {
                /* +3：MeasureString 的取整比实际排版略紧，
                   再留一点余量，避免最后一列字被裁。 */
                int text_w = (int)ceilf(lay.width) + theme->text_pad * 2 + 3;
                int text_h = (int)ceilf(lay.height) + theme->text_pad_y * 2 + 2;

                if (text_w < 1) text_w = 1;
                if (text_h < 1) text_h = 1;

                /* 文字框至少和工具栏一样宽，否则胶囊会探出左边界 */
                int toolbar_w = theme->button_size * 3 +
                                (theme->toolbar_h - theme->button_size) + 2;
                if (text_w < toolbar_w) text_w = toolbar_w;

                int width  = text_w + theme->padding * 2;
                int height = text_h + theme->gap + theme->toolbar_h +
                             theme->padding * 2;

                if (width < 1) width = 1;
                if (height < 1) height = 1;

                *out_width = width;
                *out_height = height;
                ok = TRUE;
            }
            layout_free(&lay);
        }

        delete body;
        delete emoji;
    }

    ReleaseDC(NULL, screen);
    return ok;
}

BOOL overlay_render(const wchar_t *text, const OverlayTheme *theme,
                    int width, int height, void *bits, int stride,
                    int hover_button, int pressed_button, int doc_enabled)
{
    if (!g_gdiplus_ready || text == NULL || theme == NULL ||
        bits == NULL || width <= 0 || height <= 0) {
        return FALSE;
    }

    /* 关键：位图必须是预乘 alpha（PARGB），UpdateLayeredWindow 要求如此。
       直接包装调用方给的 DIB 内存，GDI+ 绘制时会正确写入预乘值。 */
    Bitmap bmp(width, height, stride, PixelFormat32bppPARGB, (BYTE *)bits);
    if (bmp.GetLastStatus() != Ok) return FALSE;

    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    /* 先清成全透明，之后一律 SourceOver。
       不能整张用 SourceCopy：那样文字抗锯齿的半透明边缘会直接覆盖掉
       背景像素（等于挖洞），边缘会发黑。Clear 之后 SourceOver 在空位图上
       等价于直接写入，既正确又能保留抗锯齿。 */
    g.SetCompositingMode(CompositingModeSourceOver);
    g.Clear(Color(0, 0, 0, 0));

    ToolbarLayout lay;
    compute_layout(theme, (REAL)width, (REAL)height, &lay);

    /* 1. 投影先画：它在背景之下，否则暗色会盖住文字框边缘，
          看起来就成了"白边外面套一圈黑边"。
       层数按 padding 收缩，保证不会画到窗口外面被裁掉。 */
    if (theme->shadow_argb != 0) {
        int layers = theme->padding - 1;
        if (layers > 6) layers = 6;
        if (layers < 1) layers = 1;

        unsigned int base_a = (theme->shadow_argb >> 24) & 0xFF;
        unsigned int rgb = theme->shadow_argb & 0x00FFFFFF;

        for (int i = layers; i >= 1; i--) {
            RectF sh(lay.text_box.X - (REAL)i,
                     lay.text_box.Y - (REAL)i + 1.5f,
                     lay.text_box.Width + (REAL)(i * 2),
                     lay.text_box.Height + (REAL)(i * 2));
            GraphicsPath sp;
            add_round_rect(&sp, sh, (REAL)theme->radius + (REAL)i);

            unsigned int a = base_a / (unsigned int)(i + 1);
            if (a == 0) continue;
            SolidBrush sb(argb_to_color((a << 24) | rgb));
            g.FillPath(&sb, &sp);
        }
    }

    /* 2. 文字框背景（盖在投影之上） */
    GraphicsPath path;
    add_round_rect(&path, lay.text_box, (REAL)theme->radius);
    SolidBrush bg_brush(argb_to_color(theme->bg_argb));
    g.FillPath(&bg_brush, &path);

    /* 3. 文本区边框 */
    {
        GraphicsPath text_path;
        add_round_rect(&text_path, lay.text_box, (REAL)theme->radius);
        Pen border(argb_to_color(theme->border_argb), 1.0f);
        g.DrawPath(&border, &text_path);
    }

    Font *body = create_body_font(theme->font_px);
    Font *emoji = create_emoji_font(theme->font_px);

    if (body != NULL && body->GetLastStatus() == Ok) {
        StringFormat fmt;
        setup_unit_format(&fmt);

        SolidBrush fg_brush(argb_to_color(theme->fg_argb));

        float text_x = lay.text_box.X + (float)theme->text_pad;
        float text_y = lay.text_box.Y + (float)theme->text_pad_y;
        float text_limit = lay.text_box.Width - (float)theme->text_pad * 2.0f;

        TextLayout text_lay;
        layout_init(&text_lay);
        if (layout_text(&g, text, body, emoji, &fmt, text_limit, &text_lay)) {
            for (int i = 0; i < text_lay.run_count; i++) {
                const TextRun *run = &text_lay.runs[i];
                Font *font = (run->font == FONT_EMOJI && emoji != NULL) ? emoji : body;
                g.DrawString(text + run->start, run->len, font,
                             PointF(text_x + run->x, text_y + run->y),
                             &fmt, &fg_brush);
            }
        }
        layout_free(&text_lay);
    }
    delete body;
    delete emoji;

    /* 3. 胶囊工具栏 */
    draw_toolbar(&g, theme, &lay, hover_button, pressed_button, doc_enabled);

    return TRUE;
}

/* ================= GitHub 标志（矢量绘制） =================
 *
 * 设置页右下角要显示一个 GitHub 图标。为了不引入图片资源、
 * 并且在任何 DPI 下都清晰，这里直接解析官方 mark 的 SVG 路径数据，
 * 用 GDI+ 路径绘制。
 */

namespace {

struct SvgCursor {
    const char *p;
    float x, y;
    float sx, sy;
    float ctrl_x, ctrl_y;
    char  prev;
};

static void svg_skip(SvgCursor *c)
{
    while (*c->p == ' ' || *c->p == ',' || *c->p == '\t' ||
           *c->p == '\n' || *c->p == '\r') {
        c->p++;
    }
}

static bool svg_number(SvgCursor *c, float *out)
{
    svg_skip(c);
    const char *start = c->p;
    if (*c->p == '+' || *c->p == '-') c->p++;

    bool digits = false;
    while (*c->p >= '0' && *c->p <= '9') { c->p++; digits = true; }
    if (*c->p == '.') {
        c->p++;
        while (*c->p >= '0' && *c->p <= '9') { c->p++; digits = true; }
    }
    if (!digits) { c->p = start; return false; }

    if (*c->p == 'e' || *c->p == 'E') {
        const char *save = c->p;
        c->p++;
        if (*c->p == '+' || *c->p == '-') c->p++;
        if (*c->p >= '0' && *c->p <= '9') {
            while (*c->p >= '0' && *c->p <= '9') c->p++;
        } else {
            c->p = save;
        }
    }

    char buffer[64];
    size_t len = (size_t)(c->p - start);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, start, len);
    buffer[len] = 0;
    *out = (float)atof(buffer);
    return true;
}

static void svg_flag(SvgCursor *c, int *out)
{
    svg_skip(c);
    *out = (*c->p == '1') ? 1 : 0;
    if (*c->p == '0' || *c->p == '1') c->p++;
}

static void svg_arc(SvgCursor *c, GraphicsPath *path,
                    float rx, float ry, float rotation,
                    int large_arc, int sweep,
                    float x2, float y2)
{
    const double PI = 3.14159265358979323846;
    float x1 = c->x, y1 = c->y;

    if (rx == 0.0f || ry == 0.0f) {
        path->AddLine(x1, y1, x2, y2);
        c->x = x2; c->y = y2;
        return;
    }

    rx = fabsf(rx);
    ry = fabsf(ry);

    double phi = rotation * PI / 180.0;
    double cos_phi = cos(phi), sin_phi = sin(phi);

    double dx2 = (x1 - x2) / 2.0, dy2 = (y1 - y2) / 2.0;
    double x1p =  cos_phi * dx2 + sin_phi * dy2;
    double y1p = -sin_phi * dx2 + cos_phi * dy2;

    double rx2 = (double)rx * rx, ry2 = (double)ry * ry;
    double x1p2 = x1p * x1p, y1p2 = y1p * y1p;

    double lambda = x1p2 / rx2 + y1p2 / ry2;
    if (lambda > 1.0) {
        double s = sqrt(lambda);
        rx = (float)(rx * s);
        ry = (float)(ry * s);
        rx2 = (double)rx * rx;
        ry2 = (double)ry * ry;
    }

    double num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
    double den = rx2 * y1p2 + ry2 * x1p2;
    double coef = (den == 0.0) ? 0.0 : sqrt(fmax(0.0, num / den));
    if (large_arc == sweep) coef = -coef;

    double cxp =  coef * (rx * (double)y1p / ry);
    double cyp = -coef * (ry * (double)x1p / rx);

    double cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2.0;
    double cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2.0;

    double v1x = (x1p - cxp) / rx,  v1y = (y1p - cyp) / ry;
    double v2x = (-x1p - cxp) / rx, v2y = (-y1p - cyp) / ry;

    double theta1 = atan2(v1y, v1x);
    double theta2 = atan2(v2y, v2x);
    double delta = theta2 - theta1;

    if (!sweep && delta > 0) delta -= 2 * PI;
    if (sweep && delta < 0)  delta += 2 * PI;

    int segments = (int)ceil(fabs(delta) / (PI / 2));
    if (segments < 1) segments = 1;

    for (int i = 0; i < segments; i++) {
        double a1 = theta1 + delta * i / segments;
        double a2 = theta1 + delta * (i + 1) / segments;
        double k = 4.0 / 3.0 * tan((a2 - a1) / 4.0);

        double c1x = cos(a1), c1y = sin(a1);
        double c2x = cos(a2), c2y = sin(a2);

        double p1x = c1x - k * c1y, p1y = c1y + k * c1x;
        double p2x = c2x + k * c2y, p2y = c2y - k * c2x;

        PointF b1((REAL)(cos_phi * rx * p1x - sin_phi * ry * p1y + cx),
                  (REAL)(sin_phi * rx * p1x + cos_phi * ry * p1y + cy));
        PointF b2((REAL)(cos_phi * rx * p2x - sin_phi * ry * p2y + cx),
                  (REAL)(sin_phi * rx * p2x + cos_phi * ry * p2y + cy));
        PointF b3((REAL)(cos_phi * rx * c2x - sin_phi * ry * c2y + cx),
                  (REAL)(sin_phi * rx * c2x + cos_phi * ry * c2y + cy));

        path->AddBezier(PointF((REAL)c->x, (REAL)c->y), b1, b2, b3);
        c->x = (float)b3.X;
        c->y = (float)b3.Y;
    }

    c->x = x2;
    c->y = y2;
}

static bool svg_build(const char *data, GraphicsPath *path)
{
    SvgCursor c;
    memset(&c, 0, sizeof(c));
    c.p = data;

    while (*c.p != 0) {
        svg_skip(&c);
        if (*c.p == 0) break;

        char cmd = *c.p;
        bool relative = (cmd >= 'a' && cmd <= 'z');
        if (strchr("MmLlHhVvCcSsQqTtAaZz", cmd) == NULL) {
            c.p++;
            continue;
        }
        c.p++;

        switch (cmd) {
        case 'M': case 'm': {
            float x, y;
            if (!svg_number(&c, &x) || !svg_number(&c, &y)) return false;
            if (relative) { x += c.x; y += c.y; }
            path->StartFigure();
            c.x = x; c.y = y;
            c.sx = x; c.sy = y;

            float nx, ny;
            while (svg_number(&c, &nx)) {
                if (!svg_number(&c, &ny)) break;
                if (relative) { nx += c.x; ny += c.y; }
                path->AddLine(PointF((REAL)c.x, (REAL)c.y), PointF((REAL)nx, (REAL)ny));
                c.x = nx; c.y = ny;
            }
            break;
        }

        case 'L': case 'l': {
            float x, y;
            while (svg_number(&c, &x)) {
                if (!svg_number(&c, &y)) break;
                if (relative) { x += c.x; y += c.y; }
                path->AddLine(PointF((REAL)c.x, (REAL)c.y), PointF((REAL)x, (REAL)y));
                c.x = x; c.y = y;
            }
            break;
        }

        case 'H': case 'h': {
            float x;
            while (svg_number(&c, &x)) {
                if (relative) x += c.x;
                path->AddLine(PointF((REAL)c.x, (REAL)c.y), PointF((REAL)x, (REAL)c.y));
                c.x = x;
            }
            break;
        }

        case 'V': case 'v': {
            float y;
            while (svg_number(&c, &y)) {
                if (relative) y += c.y;
                path->AddLine(PointF((REAL)c.x, (REAL)c.y), PointF((REAL)c.x, (REAL)y));
                c.y = y;
            }
            break;
        }

        case 'C': case 'c': {
            float x1, y1, x2, y2, x, y;
            while (svg_number(&c, &x1)) {
                if (!svg_number(&c, &y1) || !svg_number(&c, &x2) ||
                    !svg_number(&c, &y2) || !svg_number(&c, &x) ||
                    !svg_number(&c, &y)) break;
                if (relative) {
                    x1 += c.x; y1 += c.y; x2 += c.x; y2 += c.y; x += c.x; y += c.y;
                }
                path->AddBezier(PointF((REAL)c.x, (REAL)c.y),
                                PointF((REAL)x1, (REAL)y1),
                                PointF((REAL)x2, (REAL)y2),
                                PointF((REAL)x, (REAL)y));
                c.ctrl_x = x2; c.ctrl_y = y2;
                c.x = x; c.y = y;
            }
            break;
        }

        case 'S': case 's': {
            float x2, y2, x, y;
            while (svg_number(&c, &x2)) {
                if (!svg_number(&c, &y2) || !svg_number(&c, &x) ||
                    !svg_number(&c, &y)) break;
                if (relative) { x2 += c.x; y2 += c.y; x += c.x; y += c.y; }

                float x1 = c.x, y1 = c.y;
                if (c.prev == 'C' || c.prev == 'c' || c.prev == 'S' || c.prev == 's') {
                    x1 = 2 * c.x - c.ctrl_x;
                    y1 = 2 * c.y - c.ctrl_y;
                }
                path->AddBezier(PointF((REAL)c.x, (REAL)c.y),
                                PointF((REAL)x1, (REAL)y1),
                                PointF((REAL)x2, (REAL)y2),
                                PointF((REAL)x, (REAL)y));
                c.ctrl_x = x2; c.ctrl_y = y2;
                c.x = x; c.y = y;
            }
            break;
        }

        case 'Q': case 'q': {
            float x1, y1, x, y;
            while (svg_number(&c, &x1)) {
                if (!svg_number(&c, &y1) || !svg_number(&c, &x) ||
                    !svg_number(&c, &y)) break;
                if (relative) { x1 += c.x; y1 += c.y; x += c.x; y += c.y; }
                path->AddBezier(PointF((REAL)c.x, (REAL)c.y),
                                PointF((REAL)(c.x + 2.0f / 3.0f * (x1 - c.x)),
                                       (REAL)(c.y + 2.0f / 3.0f * (y1 - c.y))),
                                PointF((REAL)(x + 2.0f / 3.0f * (x1 - x)),
                                       (REAL)(y + 2.0f / 3.0f * (y1 - y))),
                                PointF((REAL)x, (REAL)y));
                c.ctrl_x = x1; c.ctrl_y = y1;
                c.x = x; c.y = y;
            }
            break;
        }

        case 'T': case 't': {
            float x, y;
            while (svg_number(&c, &x)) {
                if (!svg_number(&c, &y)) break;
                if (relative) { x += c.x; y += c.y; }

                float x1 = c.x, y1 = c.y;
                if (c.prev == 'Q' || c.prev == 'q' || c.prev == 'T' || c.prev == 't') {
                    x1 = 2 * c.x - c.ctrl_x;
                    y1 = 2 * c.y - c.ctrl_y;
                }
                path->AddBezier(PointF((REAL)c.x, (REAL)c.y),
                                PointF((REAL)(c.x + 2.0f / 3.0f * (x1 - c.x)),
                                       (REAL)(c.y + 2.0f / 3.0f * (y1 - c.y))),
                                PointF((REAL)(x + 2.0f / 3.0f * (x1 - x)),
                                       (REAL)(y + 2.0f / 3.0f * (y1 - y))),
                                PointF((REAL)x, (REAL)y));
                c.ctrl_x = x1; c.ctrl_y = y1;
                c.x = x; c.y = y;
            }
            break;
        }

        case 'A': case 'a': {
            float rx, ry, rot, x, y;
            int large, sweep;
            while (svg_number(&c, &rx)) {
                if (!svg_number(&c, &ry)) break;
                if (!svg_number(&c, &rot)) break;
                svg_flag(&c, &large);
                svg_flag(&c, &sweep);
                if (!svg_number(&c, &x) || !svg_number(&c, &y)) break;
                if (relative) { x += c.x; y += c.y; }
                svg_arc(&c, path, rx, ry, rot, large, sweep, x, y);
            }
            break;
        }

        case 'Z': case 'z':
            path->CloseFigure();
            c.x = c.sx;
            c.y = c.sy;
            break;

        default:
            break;
        }

        c.prev = cmd;
    }

    return true;
}

/* 官方的 GitHub mark（16x16 viewBox）。 */
static const char *GITHUB_MARK_SVG =
    "M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 "
    "0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13"
    "-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66"
    ".07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15"
    "-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27"
    "s1.36.09 2 .27c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12"
    ".51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 "
    "0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8"
    "c0-4.42-3.58-8-8-8z";

} /* anonymous namespace */

BOOL gdiplus_draw_github_mark(HDC dc, int x, int y, int size,
                              unsigned int argb)
{
    if (dc == NULL || size <= 0) return FALSE;

    Graphics g(dc);
    if (g.GetLastStatus() != Ok) return FALSE;

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    GraphicsPath path;
    if (!svg_build(GITHUB_MARK_SVG, &path)) return FALSE;

    Matrix m;
    m.Translate((REAL)x, (REAL)y);
    path.Transform(&m);

    SolidBrush brush(argb_to_color(argb));
    g.FillPath(&brush, &path);
    return TRUE;
}

/* ================= 悬浮提示气泡 ================= */

BOOL gdiplus_measure_tooltip(const wchar_t *text, int font_px,
                             int *out_width, int *out_height)
{
    if (!g_gdiplus_ready || text == NULL || text[0] == 0 ||
        out_width == NULL || out_height == NULL) {
        return FALSE;
    }

    HDC screen = GetDC(NULL);
    if (screen == NULL) return FALSE;

    BOOL ok = FALSE;
    {
        Graphics g(screen);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        Font *font = create_body_font(font_px);
        if (font != NULL && font->GetLastStatus() == Ok) {
            StringFormat fmt;
            setup_unit_format(&fmt);

            RectF box;
            if (g.MeasureString(text, -1, font, PointF(0.0f, 0.0f),
                                &fmt, &box) == Ok) {
                int pad_x = font_px / 2 + 2;
                int pad_y = font_px / 3 + 1;

                *out_width  = (int)ceilf(box.Width)  + pad_x * 2;
                *out_height = (int)ceilf(box.Height) + pad_y * 2;
                if (*out_width  < 1) *out_width  = 1;
                if (*out_height < 1) *out_height = 1;
                ok = TRUE;
            }
        }
        delete font;
    }

    ReleaseDC(NULL, screen);
    return ok;
}

BOOL gdiplus_draw_tooltip(HDC dc, const wchar_t *text, int font_px,
                          int x, int y, int width, int height,
                          unsigned int bg_argb, unsigned int fg_argb)
{
    if (!g_gdiplus_ready || dc == NULL || text == NULL || text[0] == 0 ||
        width <= 0 || height <= 0) {
        return FALSE;
    }

    Graphics g(dc);
    if (g.GetLastStatus() != Ok) return FALSE;

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    RectF box((REAL)x, (REAL)y, (REAL)width, (REAL)height);

    /* 圆角底 */
    GraphicsPath path;
    float radius = (float)(font_px / 2);
    if (radius > box.Height * 0.5f) radius = box.Height * 0.5f;
    add_round_rect(&path, box, radius);
    SolidBrush bg(argb_to_color(bg_argb));
    g.FillPath(&bg, &path);

    /* 文字居中 */
    Font *font = create_body_font(font_px);
    if (font != NULL && font->GetLastStatus() == Ok) {
        StringFormat fmt;
        fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        fmt.SetTrimming(StringTrimmingNone);
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);

        SolidBrush fg(argb_to_color(fg_argb));
        g.DrawString(text, -1, font, box, &fmt, &fg);
    }
    delete font;

    return TRUE;
}
