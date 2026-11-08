/* Exercise the actual application event handlers using SDL's dummy driver. */
#define SDL_PDF_VIEWER_NO_MAIN
#include "../src/main.c"
#include "ttf_fixture.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Failed line %d: %s (%s)\n", __LINE__, #x, SDL_GetError());            \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void mouse(App *a, Uint32 type, float x, float y) {
    SDL_Event e = {0};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
    }
    CHECK(event(a, &e));
}
static void type(App *a, const char *text) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    CHECK(event(a, &e));
}
static void press(App *a, SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = key;
    e.key.mod = mod;
    CHECK(event(a, &e));
}
/* Discards the ink the way navigation or a resize does. */
static void clear_ink(App *a) {
    a->refresh = true;
    CHECK(rebuild(a));
}
static size_t at(const App *a, int x, int y) { return ((size_t)y * a->canvas.w + x) * 4; }
/* Draws a horizontal stroke through pixel centers and returns its measured
 * width: the summed brush coverage across one column, in pixels. The tip's
 * pixel box filter telescopes, so this equals the profile's integral,
 * diameter * (1 + hardness) / 2, regardless of the stroke's position. */
static double stroke_width(App *a, int x, int y) {
    mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x - 10.5f, y + .5f);
    mouse(a, SDL_EVENT_MOUSE_MOTION, x + .5f, y + .5f);
    mouse(a, SDL_EVENT_MOUSE_MOTION, x + 10.5f, y + .5f);
    mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x + 10.5f, y + .5f);
    CHECK(a->brush.coverage[y * a->canvas.w + x] == 255);
    double sum = 0;
    for (int r = y - 40; r <= y + 40; r++)
        sum += a->brush.coverage[r * a->canvas.w + x];
    return sum / 255;
}
/* Checks the outline of the box with corners (60, 80) and (200, 160). */
static void check_box(const App *a) {
    const int edges[][2] = {{130, 80}, {130, 160}, {60, 120}, {200, 120}, {60, 80}, {200, 160}};
    for (size_t i = 0; i < SDL_arraysize(edges); i++) {
        const uint8_t *edge = a->ink_canvas.pixels + at(a, edges[i][0], edges[i][1]);
        CHECK(edge[0] == 0xd4 && edge[1] == 0xa8 && edge[2] == 0 && edge[3] == 255);
    }
    CHECK(a->ink_canvas.pixels[at(a, 130, 100) + 3] == 0); /* Only an outline. */
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    char file[2048], error[256];
    snprintf(file, sizeof(file), "%s/demo.pdf", argv[1]);
    App a = {0};
    a.pdf = pdf_open(file, error, sizeof(error));
    CHECK(a.pdf);
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    a.window = SDL_CreateWindow("SDL PDF Viewer tests", 400, 300, SDL_WINDOW_HIDDEN);
    CHECK(a.window);
    a.renderer = SDL_CreateRenderer(a.window, "software");
    CHECK(a.renderer);
    /* Some headless SDL drivers have no cursor implementation. */
    a.arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    a.hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    CHECK(rebuild(&a));
    CHECK(present(&a));
    Point p = position(&a, 100, 180);
    CHECK(fabs(p.x - 100) < .01 && fabs(p.y - 180) < .01);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 100, 180.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 160, 180.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 200, 180.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200, 180.5f);
    CHECK(!a.down && !a.drawing && a.dirty);
    size_t pixel = ((size_t)180 * a.canvas.w + 150) * 4;
    CHECK(a.ink_canvas.pixels[pixel] == 0xe5 && a.ink_canvas.pixels[pixel + 1] == 0x2b);
    CHECK(a.ink_canvas.pixels[pixel + 3] == 255);
    CHECK(present(&a));
    SDL_Event key = {0};
    key.type = SDL_EVENT_KEY_DOWN;
    /* C and Delete are no longer shortcuts: the ink stays. */
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    key.key.key = SDLK_DELETE;
    CHECK(event(&a, &key));
    CHECK(!a.refresh && a.ink_canvas.pixels[pixel + 3] == 255);
    clear_ink(&a);
    CHECK(a.ink_canvas.pixels[pixel + 3] == 0);
    /* Shift+drag draws an ember arrow from the press point to the pointer;
     * the preview follows the pointer and earlier ink is kept. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 50.5f, 60.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 80.5f, 60.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 80.5f, 60.5f);
    size_t red = at(&a, 65, 60);
    CHECK(a.ink_canvas.pixels[red] == 0xe5 && a.ink_canvas.pixels[red + 3] == 255);
    SDL_SetModState(SDL_KMOD_LSHIFT);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 40.5f, 100.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 60.5f, 140.5f);
    CHECK(a.drawing && a.arrow_stroke);
    CHECK(a.ink_canvas.pixels[at(&a, 50, 120) + 3] == 255);
    SDL_SetModState(SDL_KMOD_NONE); /* Releasing Shift mid-drag keeps the arrow. */
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 300.5f, 60.5f);
    CHECK(a.arrow_stroke && a.ink_canvas.pixels[at(&a, 50, 120) + 3] == 0);
    CHECK(a.ink_canvas.pixels[red] == 0xe5 && a.ink_canvas.pixels[red + 3] == 255);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 240.5f, 100.5f);
    CHECK(!a.down && !a.drawing);
    const uint8_t *shaft = a.ink_canvas.pixels + at(&a, 140, 100);
    CHECK(shaft[0] == 0xff && shaft[1] == 0x5a && shaft[2] == 0x1f && shaft[3] == 255);
    CHECK(a.ink_canvas.pixels[at(&a, 300, 60) + 3] == 0);
    /* The head's wings, six pen widths long, point back from the tip at about
     * 28 degrees each; sample the pixel containing a point 80% along each. */
    double head = a.pen_size * ARROW_HEAD * .8;
    for (int side = -1; side <= 1; side += 2) {
        double wx = 240.5 - head * cos(ARROW_ANGLE), wy = 100.5 + side * head * sin(ARROW_ANGLE);
        CHECK(a.ink_canvas.pixels[at(&a, (int)wx, (int)wy) + 3] > 0);
    }    CHECK(a.ink_canvas.pixels[at(&a, 250, 100) + 3] == 0); /* Nothing past the tip. */
    /* A Shift-click without a drag still draws nothing. */
    SDL_SetModState(SDL_KMOD_LSHIFT);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 200.5f, 200.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200.5f, 200.5f);
    SDL_SetModState(SDL_KMOD_NONE);
    CHECK(a.ink_canvas.pixels[at(&a, 200, 200) + 3] == 0);
    clear_ink(&a);
    CHECK(shaft[3] == 0 && a.ink_canvas.pixels[red + 3] == 0);
    /* Ctrl+drag draws a dark yellow box from the press point to the pointer;
     * the preview follows the pointer and earlier ink is kept. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 110.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 150.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 150.5f, 120.5f);
    size_t under = at(&a, 130, 120);
    CHECK(a.ink_canvas.pixels[under] == 0xe5 && a.ink_canvas.pixels[under + 3] == 255);
    SDL_SetModState(SDL_KMOD_LCTRL);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 60.5f, 80.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 130.5f, 140.5f);
    CHECK(a.drawing && a.rect_stroke && !a.arrow_stroke);
    CHECK(a.ink_canvas.pixels[at(&a, 130, 100) + 3] == 255);
    CHECK(a.ink_canvas.pixels[under] == 0xd4); /* The preview's edge crosses the ink. */
    SDL_SetModState(SDL_KMOD_NONE); /* Releasing Ctrl mid-drag keeps the box. */
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 300.5f, 220.5f);
    CHECK(a.rect_stroke && a.ink_canvas.pixels[at(&a, 130, 100) + 3] == 0);
    CHECK(a.ink_canvas.pixels[under] == 0xe5 && a.ink_canvas.pixels[under + 3] == 255);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200.5f, 160.5f);
    CHECK(!a.down && !a.drawing);
    check_box(&a);
    CHECK(a.ink_canvas.pixels[at(&a, 300, 220) + 3] == 0);
    clear_ink(&a);
    /* Command works the same way, and a box can be dragged in any direction. */
    SDL_SetModState(SDL_KMOD_LGUI);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 200.5f, 160.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 60.5f, 80.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 60.5f, 80.5f);
    SDL_SetModState(SDL_KMOD_NONE);
    check_box(&a);
    clear_ink(&a);
    /* A Ctrl-click without a drag still draws nothing. */
    SDL_SetModState(SDL_KMOD_LCTRL);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 200.5f, 200.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200.5f, 200.5f);
    SDL_SetModState(SDL_KMOD_NONE);
    CHECK(a.ink_canvas.pixels[at(&a, 200, 200) + 3] == 0);
    /* Ctrl wins over Shift, and losing focus mid-box leaves the box without
     * flushing the previous freehand stroke's tail in the box's color. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 220.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 250.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 250.5f, 120.5f);
    clear_ink(&a);
    SDL_SetModState(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 60.5f, 80.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 200.5f, 160.5f);
    SDL_SetModState(SDL_KMOD_NONE);
    CHECK(a.drawing && a.rect_stroke && !a.arrow_stroke);
    SDL_Event blur = {0};
    blur.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    CHECK(event(&a, &blur));
    CHECK(!a.down && !a.drawing);
    check_box(&a);
    CHECK(a.ink_canvas.pixels[at(&a, 250, 120) + 3] == 0);
    clear_ink(&a);
    /* A click without a drag (left or right) draws nothing. */
    size_t dot = ((size_t)120 * a.canvas.w + 200) * 4;
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 200.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200.5f, 120.5f);
    CHECK(!a.down && !a.drawing && a.ink_canvas.pixels[dot + 3] == 0);
    SDL_Event right = {0};
    right.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    right.button.button = SDL_BUTTON_RIGHT;
    right.button.x = 200.5f;
    right.button.y = 120.5f;
    CHECK(event(&a, &right));
    right.type = SDL_EVENT_MOUSE_BUTTON_UP;
    CHECK(event(&a, &right));
    CHECK(!a.down && a.ink_canvas.pixels[dot + 3] == 0);
    /* The soft tip's diameter is 0.5% of the output height, like text a share
     * of the screen rather than a fixed pixel count: 1.5 pixels here. */
    double width = stroke_width(&a, 200, 120);
    CHECK(a.h == 300 && a.pen_size == 1.5);
    CHECK(fabs(width - a.pen_size * 1.7 / 2) < .02);
    CHECK(a.ink_canvas.pixels[dot] == 0xe5 && a.ink_canvas.pixels[dot + 1] == 0x2b);
    CHECK(a.brush.coverage[(120 + 2) * a.canvas.w + 200] == 0);
    clear_ink(&a);
    /* Doubling the window height doubles the stroke width. */
    CHECK(SDL_SetWindowSize(a.window, 800, 600));
    CHECK(rebuild(&a));
    CHECK(a.h == 600);
    double wide = stroke_width(&a, 400, 240);
    CHECK(a.pen_size == 3 && fabs(wide - 2 * width) < .04);
    CHECK(a.brush.coverage[(240 + 2) * a.canvas.w + 400] == 0);
    CHECK(a.brush.coverage[(240 + 1) * a.canvas.w + 400] > 0);
    CHECK(SDL_SetWindowSize(a.window, 400, 300));
    CHECK(rebuild(&a));
    CHECK(a.h == 300);
    /* T starts dark blue text at the pointer; typing never navigates. */
    static uint8_t font[2048];
    size_t font_size = ttf_fixture(font, sizeof(font));
    a.font = text_font_from_memory(font, font_size);
    CHECK(a.font);
    /* T is ignored while a pen stroke is in progress. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 100, 250);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 140, 250);
    press(&a, SDLK_T, 0);
    CHECK(a.drawing && !a.typing);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 140, 250);
    clear_ink(&a);
    SDL_WarpMouseInWindow(a.window, 120, 150);
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    press(&a, SDLK_T, 0);
    CHECK(a.typing && a.text_len == 0);
    CHECK(a.text_size == round(a.h * .07)); /* 7% of the output height. */
    Point o = a.text_origin;
    CHECK(o.x == round(mx) && o.y == round(my + text_metrics(a.font, a.text_size).cap_height / 2));
    type(&a, "t"); /* The T key's own character is not inserted. */
    type(&a, "H");
    press(&a, SDLK_N, 0);
    press(&a, SDLK_C, 0);
    press(&a, SDLK_SPACE, 0);
    CHECK(a.page == 0 && !a.refresh && a.typing);
    type(&a, "t");
    CHECK(!strcmp(a.text, "Ht"));
    press(&a, SDLK_BACKSPACE, 0);
    type(&a, "\xc3\xa9"); /* Backspace removes a whole UTF-8 sequence. */
    CHECK(a.text_len == 3);
    press(&a, SDLK_BACKSPACE, 0);
    CHECK(!strcmp(a.text, "H"));
    press(&a, SDLK_RETURN, SDL_KMOD_LSHIFT);
    type(&a, "H");
    CHECK(!strcmp(a.text, "H\nH") && a.text_texture && present(&a));
    /* The fixture's H square spans 0.1..0.6 em right of the origin and its
     * 0.7 em cap height above the baseline; sample near its center. */
    size_t cx = (size_t)(o.x + round(a.text_size * .35)), cy = (size_t)(o.y - round(a.text_size * .35));
    size_t first = (cy * a.canvas.w + cx) * 4;
    size_t second = first + (size_t)text_metrics(a.font, a.text_size).line_height * a.canvas.w * 4;
    a.running = true;
    press(&a, SDLK_ESCAPE, 0); /* Cancels the text instead of quitting. */
    CHECK(!a.typing && a.running && !a.text_texture && a.text_len == 0);
    CHECK(a.ink_canvas.pixels[first + 3] == 0 && a.ink_canvas.pixels[second + 3] == 0);
    press(&a, SDLK_T, 0);
    type(&a, "H");
    press(&a, SDLK_RETURN, SDL_KMOD_RSHIFT);
    type(&a, "H");
    press(&a, SDLK_RETURN, 0);
    CHECK(!a.typing && a.ink_dirty);
    for (int i = 0; i < 2; i++) {
        const uint8_t *px = a.ink_canvas.pixels + (i ? second : first);
        CHECK(px[0] == 0x0f && px[1] == 0x2f && px[2] == 0x8f && px[3] == 255);
    }
    CHECK(present(&a));
    /* Committed text is ink: discarded together with pen strokes. */
    clear_ink(&a);
    CHECK(a.ink_canvas.pixels[first + 3] == 0 && a.ink_canvas.pixels[second + 3] == 0);
    /* Down finishes the text instead of changing the page. */
    press(&a, SDLK_T, 0);
    type(&a, "H");
    press(&a, SDLK_DOWN, 0);
    CHECK(!a.typing && a.page == 0 && !a.refresh && a.ink_canvas.pixels[first + 3] == 255);
    clear_ink(&a);
    /* A click finishes the text and is consumed: no link, no dot. */
    press(&a, SDLK_T, 0);
    type(&a, "H");
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 300, 90);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 300, 90);
    CHECK(!a.typing && !a.down && a.page == 0 && !a.refresh);
    CHECK(a.ink_canvas.pixels[first + 3] == 255);
    CHECK(a.ink_canvas.pixels[((size_t)90 * a.canvas.w + 300) * 4 + 3] == 0);
    clear_ink(&a);
    /* A tap on the PDF link goes to the rotated second page. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 300, 90);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 300, 90);
    CHECK(a.page == 1 && a.refresh);
    CHECK(rebuild(&a));
    key.key.key = SDLK_LEFT;
    CHECK(event(&a, &key));
    CHECK(a.page == 0);
    CHECK(rebuild(&a));
    key.key.key = SDLK_LEFT;
    CHECK(event(&a, &key));
    CHECK(a.page == 0 && !a.refresh);
    key.key.key = SDLK_END;
    CHECK(event(&a, &key));
    CHECK(a.page == 1);
    CHECK(rebuild(&a));
    key.key.key = SDLK_HOME;
    CHECK(event(&a, &key));
    CHECK(a.page == 0);
    CHECK(rebuild(&a));
    /* Pages from the background cache match pages rendered on demand. */
    Canvas expected = {0};
    Matrix expected_view;
    CHECK(canvas_init(&expected, a.w, a.h));
    for (int i = 0; i < pdf_page_count(a.pdf); i++)
        CHECK(pdf_render(a.pdf, i, &expected, &expected_view));
    cache_start(&a);
    CHECK(a.worker_count > 0);
    for (int i = 0; i < 40; i++) {
        key.key.key = i % 3 == 2 ? SDLK_LEFT : SDLK_RIGHT;
        CHECK(event(&a, &key));
        if (a.refresh)
            CHECK(rebuild(&a));
        CHECK(pdf_render_background(a.pdf, a.page, &expected, &expected_view));
        CHECK(!memcmp(a.canvas.pixels, expected.pixels, (size_t)a.w * a.h * 4));
        CHECK(expected_view.e == a.view.e && expected_view.f == a.view.f);
        CHECK(present(&a));
        SDL_Event pending;
        while (SDL_PollEvent(&pending))
            CHECK(event(&a, &pending));
    }
    /* A resize drops everything that was rendered for the old size. */
    CHECK(SDL_SetWindowSize(a.window, 500, 350));
    CHECK(rebuild(&a) && a.canvas.w == 500);
    canvas_free(&expected);
    CHECK(canvas_init(&expected, a.w, a.h));
    CHECK(pdf_render_background(a.pdf, a.page, &expected, &expected_view));
    CHECK(!memcmp(a.canvas.pixels, expected.pixels, (size_t)a.w * a.h * 4));
    CHECK(present(&a));
    canvas_free(&expected);
    cache_stop(&a);
    key.key.key = SDLK_ESCAPE;
    a.running = true;
    CHECK(event(&a, &key));
    CHECK(!a.running);
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
    pdf_close(a.pdf);
    puts("SDL coordinate conversion, smooth ink, arrows, boxes, text, link clicks and keyboard "
         "navigation passed.");
    return 0;
}
