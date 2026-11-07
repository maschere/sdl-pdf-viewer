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
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 100, 180);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 160, 180);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 200, 180);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200, 180);
    CHECK(!a.down && !a.drawing && a.dirty);
    size_t pixel = ((size_t)180 * a.canvas.w + 150) * 4;
    CHECK(a.ink_canvas.pixels[pixel] == 0xe5 && a.ink_canvas.pixels[pixel + 1] == 0x2b);
    CHECK(a.ink_canvas.pixels[pixel + 3] == 255);
    CHECK(present(&a));
    SDL_Event key = {0};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(a.refresh);
    CHECK(rebuild(&a));
    CHECK(a.ink_canvas.pixels[pixel + 3] == 0);
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
    /* A dragged stroke has a seven-output-pixel soft tip. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 190.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 200.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 210.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 210.5f, 120.5f);
    size_t w = (size_t)a.canvas.w;
    CHECK(a.ink_canvas.pixels[dot] == 0xe5 && a.ink_canvas.pixels[dot + 1] == 0x2b);
    CHECK(a.brush.coverage[dot / 4 + w] == 255);
    CHECK(a.brush.coverage[dot / 4 + 3 * w] > 0 && a.brush.coverage[dot / 4 + 3 * w] < 200);
    CHECK(a.brush.coverage[dot / 4 + 4 * w] == 0);
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(rebuild(&a));
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
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(rebuild(&a));
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
    /* Committed text is ink: C clears it together with pen strokes. */
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(rebuild(&a));
    CHECK(a.ink_canvas.pixels[first + 3] == 0 && a.ink_canvas.pixels[second + 3] == 0);
    /* Down finishes the text instead of changing the page. */
    press(&a, SDLK_T, 0);
    type(&a, "H");
    press(&a, SDLK_DOWN, 0);
    CHECK(!a.typing && a.page == 0 && !a.refresh && a.ink_canvas.pixels[first + 3] == 255);
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(rebuild(&a));
    /* A click finishes the text and is consumed: no link, no dot. */
    press(&a, SDLK_T, 0);
    type(&a, "H");
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 300, 90);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 300, 90);
    CHECK(!a.typing && !a.down && a.page == 0 && !a.refresh);
    CHECK(a.ink_canvas.pixels[first + 3] == 255);
    CHECK(a.ink_canvas.pixels[((size_t)90 * a.canvas.w + 300) * 4 + 3] == 0);
    key.key.key = SDLK_C;
    CHECK(event(&a, &key));
    CHECK(rebuild(&a));
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
    puts("SDL coordinate conversion, smooth ink, text, clear, link clicks and keyboard "
         "navigation passed.");
    return 0;
}
