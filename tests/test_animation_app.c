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
static void click_control(App *a, int index) {
    SDL_FRect r = a->playbacks[index].control;
    float x = r.x + r.w * .5f, y = r.y + r.h * .5f;
    SDL_RenderCoordinatesToWindow(a->renderer, x, y, &x, &y);
    mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y);
}
static void capture(App *a, const char *directory, const char *name) {
    if (!directory)
        return;
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    SDL_Surface *s = SDL_RenderReadPixels(a->renderer, NULL);
    CHECK(s);
    CHECK(SDL_SaveBMP(s, path));
    SDL_DestroySurface(s);
}
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    const char *output = argc == 3 ? argv[2] : NULL;
    char path[2048], error[256];
    snprintf(path, sizeof(path), "%s/testslides.pdf", argv[1]);
    App a = {0};
    a.pdf = pdf_open(path, error, sizeof(error));
    CHECK(a.pdf);
    a.page = 1;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    a.window = SDL_CreateWindow("SDL PDF Viewer animation tests", 1000, 625, SDL_WINDOW_HIDDEN);
    CHECK(a.window);
    a.renderer = SDL_CreateRenderer(a.window, "software");
    CHECK(a.renderer);
    a.arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    a.hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    CHECK(rebuild(&a) && present(&a));
    CHECK(a.playback_count == 1 && a.playbacks[0].player.frame == 0 &&
          !a.playbacks[0].player.playing);
    SDL_ScaleMode mode;
    CHECK(SDL_GetTextureScaleMode(a.playbacks[0].texture, &mode) && mode == SDL_SCALEMODE_NEAREST);
    capture(&a, output, "gif-paused");
    click_control(&a, 0);
    CHECK(a.playbacks[0].player.playing && !a.ink_canvas.pixels);
    uint64_t now = a.playbacks[0].player.next + 2 * a.playbacks[0].player.interval;
    tick_animations(&a, now);
    CHECK(a.dirty && a.playbacks[0].player.frame == 3);
    CHECK(present(&a));
    capture(&a, output, "gif-playing");
    click_control(&a, 0);
    CHECK(!a.playbacks[0].player.playing && animation_wait(&a, now) == -1);
    int paused = a.playbacks[0].player.frame;
    tick_animations(&a, now + 1000000000);
    CHECK(a.playbacks[0].player.frame == paused);
    /* Ink is a separate transparent overlay and survives animation updates. */
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_DOWN, 100, 200);
    mouse(&a, SDL_EVENT_MOUSE_MOTION, 200, 200);
    mouse(&a, SDL_EVENT_MOUSE_BUTTON_UP, 250, 200);
    CHECK(present(&a));
    size_t ink = ((size_t)200 * a.w + 150) * 4;
    CHECK(a.ink_canvas.pixels[ink + 3] == 255);
    click_control(&a, 0);
    tick_animations(&a, a.playbacks[0].player.next);
    CHECK(present(&a));
    CHECK(a.ink_canvas.pixels[ink + 3] == 255 && a.ink_canvas.pixels[ink] == 0xe5);
    SDL_Event e = {0};
    a.refresh = true; /* Discarding ink keeps playback. */
    CHECK(rebuild(&a));
    CHECK(a.playbacks[0].player.playing);
    CHECK(a.ink_canvas.pixels[ink + 3] == 0);
    paused = a.playbacks[0].player.frame;
    CHECK(SDL_SetWindowSize(a.window, 1200, 750));
    CHECK(rebuild(&a) && present(&a));
    CHECK(a.w == 1200 && a.playbacks[0].player.frame == paused && a.playbacks[0].player.playing);
    e.type = SDL_EVENT_RENDER_DEVICE_RESET;
    CHECK(event(&a, &e));
    CHECK(rebuild(&a) && present(&a));
    CHECK(a.playbacks[0].player.frame == paused && a.playbacks[0].player.playing);
    a.fps_override = 15;
    change_page(&a, 2);
    CHECK(rebuild(&a) && present(&a));
    CHECK(!a.playbacks[0].player.playing && a.playbacks[0].player.frame == 0 &&
          a.playbacks[0].player.interval == 66666667);
    capture(&a, output, "apng-paused");
    click_control(&a, 0);
    tick_animations(&a, a.playbacks[0].player.next + 9 * a.playbacks[0].player.interval);
    CHECK(present(&a));
    capture(&a, output, "apng-playing");
    free_playbacks(&a);
    pdf_close(a.pdf);
    snprintf(path, sizeof(path), "%s/animation-metadata.pdf", argv[1]);
    a.pdf = pdf_open(path, error, sizeof(error));
    CHECK(a.pdf);
    a.page = 0;
    a.fps_override = 0;
    CHECK(rebuild(&a) && present(&a));
    CHECK(a.playback_count == 2 && !a.playbacks[0].player.playing && a.playbacks[1].player.playing);
    CHECK(a.playbacks[1].scaler.pixels && a.playbacks[0].player.interval == 83333333);
    capture(&a, output, "metadata-rotated");
    free_playbacks(&a);
    brush_free(&a.brush);
    canvas_free(&a.canvas);
    canvas_free(&a.ink_canvas);
    SDL_DestroyTexture(a.texture);
    SDL_DestroyTexture(a.ink_texture);
    SDL_DestroyCursor(a.hand);
    SDL_DestroyCursor(a.arrow);
    SDL_DestroyRenderer(a.renderer);
    SDL_DestroyWindow(a.window);
    SDL_Quit();
    pdf_close(a.pdf);
    puts("In-place GIF/APNG, nearest default, bicubic, paused/play controls, FPS override, "
         "independent autoplay, ink overlay, resize and renderer reset passed.");
    return 0;
}
