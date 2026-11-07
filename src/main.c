/* Application code is ISO C11; SDL's platform backends are external. */
#include "pdf.h"
#include "brush.h"
#include "text.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_SCREEN_FRACTION .07 /* Em size relative to the output height. */
#define TEXT_RGB 0x0f2f8f        /* Dark blue. */

typedef struct {
    const PdfAnimation *info;
    AnimationPlayer player;
    AnimationScaler scaler;
    SDL_Texture *texture;
    int uploaded_frame, texture_w, texture_h;
    SDL_FPoint origin, right, down;
    SDL_FRect control;
} Playback;
typedef struct {
    Pdf *pdf;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture, *ink_texture;
    SDL_Cursor *arrow, *hand;
    Canvas canvas, ink_canvas;
    Matrix view;
    Brush brush;
    int page, w, h;
    bool refresh, dirty, running, down, drawing;
    Uint8 button;
    Point pressed;
    Ink ink;
    Playback *playbacks;
    int playback_count, playback_page;
    bool playbacks_loaded, page_dirty, ink_dirty;
    double fps_override;
    /* Text tool: typed text is previewed on its own small texture and
     * rasterized into the ink layer when finished. */
    TextFont *font;
    bool font_failed, typing, swallow_t;
    char text[4096];
    size_t text_len;
    Point text_origin;
    SDL_Texture *text_texture;
    SDL_FRect text_rect, caret;
    double text_size; /* Em size in output pixels, fixed while typing. */
} App;
static void free_playbacks(App *a) {
    for (int i = 0; i < a->playback_count; i++) {
        SDL_DestroyTexture(a->playbacks[i].texture);
        animation_scaler_free(&a->playbacks[i].scaler);
    }
    free(a->playbacks);
    a->playbacks = NULL;
    a->playback_count = 0;
    a->playbacks_loaded = false;
}
static bool setup_playbacks(App *a) {
    if (!a->playbacks_loaded || a->playback_page != a->page) {
        free_playbacks(a);
        int count = pdf_animation_count(a->pdf, a->page);
        if (count) {
            a->playbacks = calloc((size_t)count, sizeof(*a->playbacks));
            if (!a->playbacks)
                return SDL_SetError("Cannot allocate animation players");
        }
        a->playback_count = count;
        a->playback_page = a->page;
        a->playbacks_loaded = true;
        for (int i = 0; i < count; i++) {
            Playback *p = &a->playbacks[i];
            p->info = pdf_animation(a->pdf, a->page, i);
            p->uploaded_frame = -1;
            animation_player_init(&p->player, p->info->clip->count,
                                  a->fps_override > 0 ? a->fps_override : p->info->fps,
                                  p->info->autoplay, SDL_GetTicksNS());
        }
    }
    for (int i = 0; i < a->playback_count; i++) {
        Playback *p = &a->playbacks[i];
        Box b = p->info->box;
        Point tl = matrix_point(a->view, (Point){b.x0, b.y1});
        Point tr = matrix_point(a->view, (Point){b.x1, b.y1});
        Point bl = matrix_point(a->view, (Point){b.x0, b.y0});
        Point br = matrix_point(a->view, (Point){b.x1, b.y0});
        p->origin = (SDL_FPoint){(float)tl.x, (float)tl.y};
        p->right = (SDL_FPoint){(float)tr.x, (float)tr.y};
        p->down = (SDL_FPoint){(float)bl.x, (float)bl.y};
        double x0 = fmin(fmin(tl.x, tr.x), fmin(bl.x, br.x)),
               x1 = fmax(fmax(tl.x, tr.x), fmax(bl.x, br.x));
        double y0 = fmin(fmin(tl.y, tr.y), fmin(bl.y, br.y)),
               y1 = fmax(fmax(tl.y, tr.y), fmax(bl.y, br.y));
        double w = fmin(26, fmin((x1 - x0) * .3, (y1 - y0) * .3)), h = w * .75;
        p->control = (SDL_FRect){(float)((x0 + x1 - w) * .5),
                                 (float)(y1 - h - fmin(5, (y1 - y0) * .03)), (float)w, (float)h};
        p->uploaded_frame = -1; /* Also refresh bicubic sizing after a resize. */
    }
    return true;
}
static int control_at(const App *a, Point point) {
    if (a->refresh || point.x < a->canvas.clip.x0 || point.x >= a->canvas.clip.x1 ||
        point.y < a->canvas.clip.y0 || point.y >= a->canvas.clip.y1)
        return -1;
    for (int i = a->playback_count - 1; i >= 0; i--) {
        SDL_FRect r = a->playbacks[i].control;
        if (point.x >= r.x && point.x < r.x + r.w && point.y >= r.y && point.y < r.y + r.h)
            return i;
    }
    return -1;
}
static bool ensure_ink_layer(App *a) {
    if (!a->ink_canvas.pixels) {
        if (!canvas_init(&a->ink_canvas, a->w, a->h))
            return SDL_SetError("Cannot allocate ink layer");
        memset(a->ink_canvas.pixels, 0, (size_t)a->w * a->h * 4);
        a->ink_canvas.clip = a->canvas.clip;
    }
    if (!a->ink_texture) {
        a->ink_texture = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING, a->w, a->h);
        if (!a->ink_texture || !SDL_SetTextureBlendMode(a->ink_texture, SDL_BLENDMODE_BLEND))
            return false;
    }
    return true;
}
static bool begin_pen(App *a) {
    if (!ensure_ink_layer(a))
        return false;
    if (!brush_begin(&a->brush, &a->ink_canvas, 7, .70, 0xe52b37))
        return SDL_SetError("Cannot allocate pen buffers");
    return true;
}
static Point position(App *a, float wx, float wy);
static void text_stop(App *a) {
    if (a->typing)
        SDL_StopTextInput(a->window);
    a->typing = a->swallow_t = false;
    a->text_len = 0;
    a->text[0] = 0;
    SDL_DestroyTexture(a->text_texture);
    a->text_texture = NULL;
    SDL_ShowCursor();
    a->dirty = true;
}
/* Lays out the pending text, renders it into a texture covering just its
 * bounds and places the caret. The fill is identical to the final ink. */
static bool text_preview(App *a) {
    SDL_DestroyTexture(a->text_texture);
    a->text_texture = NULL;
    a->dirty = true;
    Path path = {0};
    Point caret;
    if (!text_path(a->font, a->text, a->text_size, a->text_origin, &path, &caret)) {
        path_free(&path);
        return SDL_SetError("Cannot lay out text");
    }
    TextMetrics m = text_metrics(a->font, a->text_size);
    double above = m.cap_height + round(a->text_size * .12), below = round(a->text_size * .22);
    a->caret = (SDL_FRect){(float)round(caret.x), (float)(caret.y - above),
                           (float)fmax(1, round(a->text_size / 24)), (float)(above + below)};
    Box clip = a->canvas.clip;
    double xmin = clip.x1, xmax = clip.x0, ymin = clip.y1, ymax = clip.y0;
    for (size_t i = 0; i < path.count; i++) {
        Edge e = path.edges[i];
        xmin = fmin(xmin, fmin(e.a.x, e.b.x));
        xmax = fmax(xmax, fmax(e.a.x, e.b.x));
        ymin = fmin(ymin, fmin(e.a.y, e.b.y));
        ymax = fmax(ymax, fmax(e.a.y, e.b.y));
    }
    int x0 = (int)floor(fmax(xmin, clip.x0)), x1 = (int)ceil(fmin(xmax, clip.x1));
    int y0 = (int)floor(fmax(ymin, clip.y0)), y1 = (int)ceil(fmin(ymax, clip.y1));
    if (x1 <= x0 || y1 <= y0) {
        path_free(&path);
        return true;
    }
    Canvas c;
    if (!canvas_init(&c, x1 - x0, y1 - y0)) {
        path_free(&path);
        return SDL_SetError("Cannot allocate text preview");
    }
    memset(c.pixels, 0, (size_t)c.w * c.h * 4);
    c.clip = (Box){clip.x0 - x0, clip.y0 - y0, clip.x1 - x0, clip.y1 - y0};
    for (size_t i = 0; i < path.count; i++) {
        Edge *e = &path.edges[i];
        e->a.x -= x0;
        e->b.x -= x0;
        e->a.y -= y0;
        e->b.y -= y0;
    }
    bool ok = canvas_fill(&c, &path, TEXT_RGB, false);
    path_free(&path);
    if (!ok)
        SDL_SetError("Cannot render text");
    else {
        a->text_texture = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STATIC, c.w, c.h);
        ok = a->text_texture && SDL_SetTextureBlendMode(a->text_texture, SDL_BLENDMODE_BLEND) &&
             SDL_SetTextureScaleMode(a->text_texture, SDL_SCALEMODE_NEAREST) &&
             SDL_UpdateTexture(a->text_texture, NULL, c.pixels, c.w * 4);
        a->text_rect = (SDL_FRect){(float)x0, (float)y0, (float)c.w, (float)c.h};
    }
    canvas_free(&c);
    return ok;
}
static bool text_start(App *a) {
    if (!a->font && !a->font_failed) {
        a->font = text_font_open_default();
        if (!a->font) {
            a->font_failed = true;
            SDL_Log("Text tool unavailable: no TrueType font found "
                    "(use --font file.ttf or SDL_PDF_VIEWER_FONT)");
        }
    }
    if (!a->font)
        return true;
    float wx = 0, wy = 0;
    SDL_GetMouseState(&wx, &wy);
    Point p = position(a, wx, wy);
    /* Presentation-sized text: a fixed share of the output (screen) height,
     * so it looks the same at any resolution or display scale. */
    a->text_size = fmax(8, round(a->h * TEXT_SCREEN_FRACTION));
    /* Center the first line's capitals vertically on the pointer. */
    TextMetrics m = text_metrics(a->font, a->text_size);
    a->text_origin = (Point){round(p.x), round(p.y + m.cap_height * .5)};
    a->text_len = 0;
    a->text[0] = 0;
    a->typing = true;
    a->swallow_t = true; /* The T key's own character may still follow. */
    float density = SDL_GetWindowPixelDensity(a->window);
    float line = (float)a->text_size / (density > 0 ? density : 1); /* window units */
    SDL_Rect area = {(int)wx, (int)(wy - line / 2), 1, (int)line};
    SDL_SetTextInputArea(a->window, &area, 0);
    SDL_StartTextInput(a->window);
    SDL_HideCursor();
    return text_preview(a);
}
static bool text_commit(App *a) {
    bool ok = true;
    if (a->text_len) {
        if (!ensure_ink_layer(a))
            return false;
        Path path = {0};
        ok = text_path(a->font, a->text, a->text_size, a->text_origin, &path, NULL) &&
             canvas_fill(&a->ink_canvas, &path, TEXT_RGB, false);
        path_free(&path);
        if (!ok)
            SDL_SetError("Cannot draw text");
        a->ink_dirty = true;
    }
    text_stop(a);
    return ok;
}
static bool text_insert(App *a, const char *s) {
    if (a->text_len + strlen(s) >= sizeof(a->text))
        return true;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 0x20 && ch != 0x7f) || ch == '\n')
            a->text[a->text_len++] = (char)ch;
    }
    a->text[a->text_len] = 0;
    return text_preview(a);
}
static bool text_key(App *a, const SDL_KeyboardEvent *k) {
    a->swallow_t = false;
    switch (k->key) {
    case SDLK_ESCAPE:
        if (!k->repeat)
            text_stop(a);
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (k->mod & SDL_KMOD_SHIFT)
            return text_insert(a, "\n");
        return k->repeat || text_commit(a);
    case SDLK_DOWN: /* Finishes the text; it does not change the page. */
        return k->repeat || text_commit(a);
    case SDLK_BACKSPACE:
        if (!a->text_len)
            return true;
        do /* Remove one whole UTF-8 sequence. */
            a->text_len--;
        while (a->text_len && ((unsigned char)a->text[a->text_len] & 0xc0) == 0x80);
        a->text[a->text_len] = 0;
        return text_preview(a);
    default:
        return true; /* Other keys only type text, never navigate. */
    }
}
static void tick_animations(App *a, uint64_t now) {
    for (int i = 0; i < a->playback_count; i++)
        if (animation_player_tick(&a->playbacks[i].player, now))
            a->dirty = true;
}
static int animation_wait(const App *a, uint64_t now) {
    int wait = -1;
    for (int i = 0; i < a->playback_count; i++) {
        int ms = animation_player_wait(&a->playbacks[i].player, now);
        if (ms >= 0 && (wait < 0 || ms < wait))
            wait = ms;
    }
    return wait;
}
static bool playback_texture(App *a, Playback *p) {
    const Animation *clip = p->info->clip;
    if (p->uploaded_frame == p->player.frame && p->texture)
        return true;
    int w = clip->w, h = clip->h;
    const uint8_t *pixels = clip->frames[p->player.frame];
    if (p->info->interpolation == ANIMATION_BICUBIC) {
        double width = ceil(hypot(p->right.x - p->origin.x, p->right.y - p->origin.y));
        double height = ceil(hypot(p->down.x - p->origin.x, p->down.y - p->origin.y));
        if (!isfinite(width) || !isfinite(height) || width > 16384 || height > 16384)
            return SDL_SetError("Animation display dimensions exceed limits");
        w = (int)width;
        h = (int)height;
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
        if (!animation_scale_bicubic(&p->scaler, pixels, clip->w, clip->h, w, h))
            return SDL_SetError("Cannot bicubic-scale animation");
        pixels = p->scaler.pixels;
    }
    if (!p->texture || p->texture_w != w || p->texture_h != h) {
        SDL_DestroyTexture(p->texture);
        p->texture = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
        p->texture_w = w;
        p->texture_h = h;
        if (!p->texture || !SDL_SetTextureBlendMode(p->texture, SDL_BLENDMODE_BLEND) ||
            !SDL_SetTextureScaleMode(p->texture, SDL_SCALEMODE_NEAREST))
            return false;
    }
    if (!SDL_UpdateTexture(p->texture, NULL, pixels, w * 4))
        return false;
    p->uploaded_frame = p->player.frame;
    return true;
}
static bool draw_control(App *a, const Playback *p) {
    SDL_FRect r = p->control;
    if (!SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND) ||
        !SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 65) || !SDL_RenderFillRect(a->renderer, &r))
        return false;
    if (p->player.playing) {
        SDL_FRect bars[2] = {{r.x + r.w * .34f, r.y + r.h * .25f, r.w * .10f, r.h * .5f},
                             {r.x + r.w * .56f, r.y + r.h * .25f, r.w * .10f, r.h * .5f}};
        return SDL_SetRenderDrawColor(a->renderer, 255, 255, 255, 205) &&
               SDL_RenderFillRects(a->renderer, bars, 2);
    }
    SDL_FColor color = {1, 1, 1, .8f};
    SDL_Vertex vertices[3] = {{{r.x + r.w * .38f, r.y + r.h * .23f}, color, {0, 0}},
                              {{r.x + r.w * .38f, r.y + r.h * .77f}, color, {0, 0}},
                              {{r.x + r.w * .70f, r.y + r.h * .50f}, color, {0, 0}}};
    return SDL_RenderGeometry(a->renderer, NULL, vertices, 3, NULL, 0);
}
static bool rebuild(App *a) {
    int w, h;
    if (!SDL_GetRenderOutputSize(a->renderer, &w, &h))
        return false;
    if (w < 1 || h < 1)
        return true;
    if (w != a->w || h != a->h || !a->canvas.pixels) {
        canvas_free(&a->canvas);
        SDL_DestroyTexture(a->texture);
        a->texture = NULL;
        brush_free(&a->brush);
        canvas_free(&a->ink_canvas);
        SDL_DestroyTexture(a->ink_texture);
        a->ink_texture = NULL;
        if (!canvas_init(&a->canvas, w, h)) {
            SDL_SetError("Display exceeds raster memory limit");
            return false;
        }
        a->w = w;
        a->h = h;
        a->texture = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!a->texture) {
            SDL_SetError("Cannot allocate display buffers");
            return false;
        }
        if (!SDL_SetTextureBlendMode(a->texture, SDL_BLENDMODE_NONE))
            return false;
    }
    a->down = a->drawing = false;
    SDL_CaptureMouse(false);
    text_stop(a); /* Pending text belongs to the discarded page geometry. */
    if (!pdf_render_background(a->pdf, a->page, &a->canvas, &a->view)) {
        SDL_SetError("Page %d: %s", a->page + 1, pdf_error(a->pdf));
        return false;
    }
    if (a->ink_canvas.pixels) {
        memset(a->ink_canvas.pixels, 0, (size_t)a->w * a->h * 4);
        a->ink_canvas.clip = a->canvas.clip;
    }
    if (!setup_playbacks(a))
        return false;
    a->page_dirty = a->ink_dirty = true;
    a->refresh = false;
    a->dirty = true;
    return true;
}
static bool present(App *a) {
    if (a->page_dirty && !SDL_UpdateTexture(a->texture, NULL, a->canvas.pixels, a->w * 4))
        return false;
    if (a->ink_dirty && a->ink_texture &&
        !SDL_UpdateTexture(a->ink_texture, NULL, a->ink_canvas.pixels, a->w * 4))
        return false;
    if (!SDL_SetRenderClipRect(a->renderer, NULL) || !SDL_RenderClear(a->renderer) ||
        !SDL_RenderTexture(a->renderer, a->texture, NULL, NULL))
        return false;
    Box b = a->canvas.clip;
    SDL_Rect clip = {(int)floor(b.x0), (int)floor(b.y0), (int)ceil(b.x1) - (int)floor(b.x0),
                     (int)ceil(b.y1) - (int)floor(b.y0)};
    if (!SDL_SetRenderClipRect(a->renderer, &clip))
        return false;
    for (int i = 0; i < a->playback_count; i++) {
        Playback *p = &a->playbacks[i];
        if (!playback_texture(a, p) || !SDL_RenderTextureAffine(a->renderer, p->texture, NULL,
                                                                &p->origin, &p->right, &p->down))
            return false;
    }
    if (a->ink_texture && !SDL_RenderTexture(a->renderer, a->ink_texture, NULL, NULL))
        return false;
    if (a->typing) {
        if (a->text_texture &&
            !SDL_RenderTexture(a->renderer, a->text_texture, NULL, &a->text_rect))
            return false;
        if (!SDL_SetRenderDrawBlendMode(a->renderer, SDL_BLENDMODE_BLEND) ||
            !SDL_SetRenderDrawColor(a->renderer, (TEXT_RGB >> 16) & 255, (TEXT_RGB >> 8) & 255,
                                    TEXT_RGB & 255, 255) ||
            !SDL_RenderFillRect(a->renderer, &a->caret))
            return false;
    }
    for (int i = 0; i < a->playback_count; i++)
        if (!draw_control(a, &a->playbacks[i]))
            return false;
    if (!SDL_SetRenderClipRect(a->renderer, NULL) || !SDL_RenderPresent(a->renderer))
        return false;
    a->page_dirty = a->ink_dirty = a->dirty = false;
    return true;
}
static Point position(App *a, float wx, float wy) {
    float x = wx, y = wy;
    SDL_RenderCoordinatesFromWindow(a->renderer, wx, wy, &x, &y);
    return (Point){x, y};
}
static bool link_at(App *a, Point p, PdfLink *link) {
    Matrix inverse;
    if (!matrix_inverse(a->view, &inverse))
        return false;
    return pdf_link_at(a->pdf, a->page, matrix_point(inverse, p), link);
}
static bool curve_draw(App *a, Curve c) {
    Path path = {0};
    path_move(&path, c.start);
    bool ok = path_curve(&path, c.c1, c.c2, c.end);
    if (ok) {
        if (!path.count)
            brush_line(&a->brush, &a->ink_canvas, c.start, c.start);
        for (size_t i = 0; i < path.count; i++)
            brush_line(&a->brush, &a->ink_canvas, path.edges[i].a, path.edges[i].b);
    }
    path_free(&path);
    a->dirty = a->ink_dirty = true;
    return ok;
}
static void change_page(App *a, int page) {
    if (page < 0 || page >= pdf_page_count(a->pdf) || page == a->page)
        return;
    a->page = page;
    a->refresh = true;
    a->down = a->drawing = false;
    SDL_CaptureMouse(false);
    SDL_SetCursor(a->arrow);
}
static bool event(App *a, SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_QUIT:
        a->running = false;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        a->refresh = true;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        a->dirty = true;
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
    case SDL_EVENT_RENDER_TARGETS_RESET:
        SDL_DestroyTexture(a->texture);
        a->texture = NULL;
        SDL_DestroyTexture(a->ink_texture);
        a->ink_texture = NULL;
        SDL_DestroyTexture(a->text_texture);
        a->text_texture = NULL;
        for (int i = 0; i < a->playback_count; i++) {
            SDL_DestroyTexture(a->playbacks[i].texture);
            a->playbacks[i].texture = NULL;
        }
        a->w = 0;
        a->refresh = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (a->drawing && !curve_draw(a, ink_finish(&a->ink)))
            return false;
        a->down = a->drawing = false;
        SDL_CaptureMouse(false);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (a->typing)
            return text_key(a, &e->key);
        if (e->key.repeat)
            break;
        switch (e->key.key) {
        case SDLK_ESCAPE:
        case SDLK_Q:
            a->running = false;
            break;
        case SDLK_RIGHT:
        case SDLK_DOWN:
        case SDLK_PAGEDOWN:
        case SDLK_SPACE:
        case SDLK_N:
            change_page(a, a->page + 1);
            break;
        case SDLK_LEFT:
        case SDLK_UP:
        case SDLK_PAGEUP:
        case SDLK_BACKSPACE:
        case SDLK_P:
            change_page(a, a->page - 1);
            break;
        case SDLK_HOME:
            change_page(a, 0);
            break;
        case SDLK_END:
            change_page(a, pdf_page_count(a->pdf) - 1);
            break;
        case SDLK_T:
            if (!a->refresh && !text_start(a))
                return false;
            break;
        case SDLK_C:
        case SDLK_DELETE:
            a->refresh = true;
            break;
        case SDLK_F:
            if (!SDL_SetWindowFullscreen(a->window,
                                         !(SDL_GetWindowFlags(a->window) & SDL_WINDOW_FULLSCREEN)))
                return false;
            a->refresh = true;
            break;
        default:
            break;
        }
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!a->typing)
            break;
        if (a->swallow_t) {
            a->swallow_t = false;
            if (!strcmp(e->text.text, "t") || !strcmp(e->text.text, "T"))
                break;
        }
        SDL_HideCursor();
        return text_insert(a, e->text.text);
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        /* A click finishes pending text and is consumed: it neither leaves a
         * dot nor follows a link. */
        if (a->typing)
            return text_commit(a);
        if (a->refresh || a->down ||
            (e->button.button != SDL_BUTTON_LEFT && e->button.button != SDL_BUTTON_RIGHT))
            break;
        a->down = true;
        a->button = e->button.button;
        a->pressed = position(a, e->button.x, e->button.y);
        a->drawing = false;
        SDL_CaptureMouse(true);
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        if (a->typing)
            SDL_ShowCursor();
        if (a->refresh)
            break;
        Point p = position(a, e->motion.x, e->motion.y);
        if (a->down) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(a->window, &ww, &wh);
            (void)wh;
            double threshold = ww > 0 ? 4.0 * a->canvas.w / ww : 4;
            if (!a->drawing && is_drag(a->pressed, p, threshold)) {
                if (!begin_pen(a))
                    return false;
                a->drawing = true;
                ink_begin(&a->ink, a->pressed);
                SDL_SetCursor(a->arrow);
            }
            if (a->drawing && !curve_draw(a, ink_add(&a->ink, p)))
                return false;
        } else {
            PdfLink link;
            SDL_SetCursor(control_at(a, p) >= 0 || link_at(a, p, &link) ? a->hand : a->arrow);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!a->down || e->button.button != a->button)
            break;
        Point p = position(a, e->button.x, e->button.y);
        a->down = false;
        SDL_CaptureMouse(false);
        if (a->drawing) {
            if (!curve_draw(a, ink_add(&a->ink, p)) || !curve_draw(a, ink_finish(&a->ink)))
                return false;
            a->drawing = false;
        } else {
            PdfLink link;
            int control = control_at(a, a->pressed);
            if (a->button == SDL_BUTTON_LEFT && control >= 0) {
                if (control_at(a, p) == control) {
                    animation_player_toggle(&a->playbacks[control].player, SDL_GetTicksNS());
                    a->dirty = true;
                }
            } else if (a->button == SDL_BUTTON_LEFT && link_at(a, a->pressed, &link)) {
                if (link.url) {
                    if (!SDL_OpenURL(link.url))
                        SDL_Log("Cannot open URL: %s", SDL_GetError());
                } else
                    change_page(a, link.page);
            } /* A click without a drag draws nothing. */
        }
        break;
    }
    default:
        break;
    }
    return true;
}
static int write_ppm(Pdf *pdf, const char *path, int page) {
    Canvas c = {0};
    Matrix view;
    int result = 1;
    if (!canvas_init(&c, 1600, 1000)) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    if (!pdf_render(pdf, page, &c, &view)) {
        fprintf(stderr, "%s\n", pdf_error(pdf));
        goto done;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        goto done;
    }
    bool ok = fprintf(f, "P6\n%d %d\n255\n", c.w, c.h) > 0;
    for (size_t i = 0; i < (size_t)c.w * c.h && ok; i++)
        ok = fwrite(c.pixels + 4 * i, 1, 3, f) == 3;
    if (fclose(f))
        ok = false;
    result = ok ? 0 : 1;
done:
    canvas_free(&c);
    return result;
}
#ifndef SDL_PDF_VIEWER_NO_MAIN
int main(int argc, char **argv) {
    bool windowed = false, check = false;
    const char *path = NULL, *output = NULL, *font_path = NULL;
    int render_page = 0;
    double fps_override = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed"))
            windowed = true;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            char *end;
            fps_override = strtod(argv[++i], &end);
            if (end == argv[i] || *end || !isfinite(fps_override) || fps_override < .1 ||
                fps_override > 240) {
                fprintf(stderr, "Invalid --fps (expected 0.1 to 240)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--font") && i + 1 < argc)
            font_path = argv[++i];
        else if (!strcmp(argv[i], "--check"))
            check = true;
        else if (!strcmp(argv[i], "--render") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            char *end;
            long n = strtol(argv[++i], &end, 10);
            if (*end || n < 1 || n > 10000) {
                fprintf(stderr, "Invalid --page\n");
                return 2;
            }
            render_page = (int)n - 1;
        } else if (!path && strncmp(argv[i], "--", 2))
            path = argv[i];
        else {
            fprintf(stderr,
                    "Usage: %s [--windowed|--check|--render output.ppm] [--page N] [--fps N] "
                    "[--font file.ttf] file.pdf\n",
                    argv[0]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr,
                "Usage: %s [--windowed|--check|--render output.ppm] [--page N] [--fps N] "
                "[--font file.ttf] file.pdf\n",
                argv[0]);
        return 2;
    }
    char error[256];
    Pdf *pdf = pdf_open(path, error, sizeof(error));
    if (!pdf) {
        fprintf(stderr, "PDF: %s\n", error);
        return 1;
    }
    /* Reject unsupported content before entering fullscreen. Decodes all used images. */
    Canvas probe = {0};
    Matrix view;
    if (!canvas_init(&probe, 128, 128)) {
        pdf_close(pdf);
        return 1;
    }
    for (int i = 0; i < pdf_page_count(pdf); i++)
        if (!pdf_render(pdf, i, &probe, &view)) {
            fprintf(stderr, "Page %d: %s\n", i + 1, pdf_error(pdf));
            canvas_free(&probe);
            pdf_close(pdf);
            return 1;
        }
    canvas_free(&probe);
    if (check) {
        printf("Validated %d pages in the supported PDF subset.\n", pdf_page_count(pdf));
        pdf_close(pdf);
        return 0;
    }
    if (output) {
        int r = write_ppm(pdf, output, render_page);
        pdf_close(pdf);
        return r;
    }
    App a = {0};
    a.pdf = pdf;
    if (font_path && !(a.font = text_font_load(font_path))) {
        fprintf(stderr, "Font: cannot load TrueType font %s\n", font_path);
        pdf_close(pdf);
        return 1;
    }
    a.fps_override = fps_override;
    a.running = true;
    a.refresh = true;
    a.page = render_page;
    int result = 1;
    if (render_page >= pdf_page_count(pdf)) {
        fprintf(stderr, "Page out of range\n");
        pdf_close(pdf);
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
        goto done;
    a.window = SDL_CreateWindow("SDL PDF Viewer", 1280, 800,
                                SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_BORDERLESS);
    if (!a.window)
        goto done;
    if (!windowed && !SDL_SetWindowFullscreen(a.window, true))
        goto done;
    a.renderer = SDL_CreateRenderer(a.window, NULL);
    if (!a.renderer)
        goto done;
    SDL_SetRenderVSync(a.renderer, 1);
    a.arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    a.hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    if (!a.arrow || !a.hand)
        goto done;
    SDL_SetCursor(a.arrow);
    while (a.running) {
        if (a.refresh && !rebuild(&a))
            goto done;
        tick_animations(&a, SDL_GetTicksNS());
        if (a.dirty && !present(&a))
            goto done;
        SDL_Event e;
        int wait = animation_wait(&a, SDL_GetTicksNS());
        if (wait < 0) {
            if (!SDL_WaitEvent(&e))
                goto done;
        } else {
            SDL_ClearError();
            if (!SDL_WaitEventTimeout(&e, wait)) {
                if (*SDL_GetError())
                    goto done;
                continue;
            }
        }
        do {
            if (!event(&a, &e))
                goto done;
        } while (SDL_PollEvent(&e));
    }
    result = 0;
done:
    if (result) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        if (a.window)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SDL PDF Viewer", SDL_GetError(),
                                     a.window);
    }
    free_playbacks(&a);
    SDL_DestroyTexture(a.text_texture);
    text_font_free(a.font);
    brush_free(&a.brush);
    canvas_free(&a.ink_canvas);
    SDL_DestroyTexture(a.ink_texture);
    canvas_free(&a.canvas);
    SDL_DestroyCursor(a.hand);
    SDL_DestroyCursor(a.arrow);
    SDL_DestroyTexture(a.texture);
    SDL_DestroyRenderer(a.renderer);
    SDL_DestroyWindow(a.window);
    SDL_Quit();
    pdf_close(pdf);
    return result;
}

#endif
