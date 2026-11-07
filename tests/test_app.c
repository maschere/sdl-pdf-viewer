/* Exercise the actual application event handlers using SDL's dummy driver. */
#define SDL_PDF_VIEWER_NO_MAIN
#include "../src/main.c"
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
    /* A tap has the same seven-output-pixel soft tip as a dragged stroke. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 200.5f, 120.5f);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 200.5f, 120.5f);
    size_t dot = ((size_t)120 * a.canvas.w + 200) * 4;
    CHECK(a.ink_canvas.pixels[dot] == 0xe5 && a.ink_canvas.pixels[dot + 1] == 0x2b);
    CHECK(a.brush.coverage[dot / 4 + 1] == 255);
    CHECK(a.brush.coverage[dot / 4 + 3] > 0 && a.brush.coverage[dot / 4 + 3] < 200);
    CHECK(a.brush.coverage[dot / 4 + 4] == 0);
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
    puts("SDL coordinate conversion, smooth ink, clear, link clicks and keyboard navigation "
         "passed.");
    return 0;
}
