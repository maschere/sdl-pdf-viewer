#ifndef ANIMATION_H
#define ANIMATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ANIMATION_NEAREST, ANIMATION_BICUBIC } AnimationInterpolation;
typedef struct {
    int w, h, count;
    uint8_t **frames; /* Fully composited, straight-alpha, top-down RGBA. */
    size_t bytes;
} Animation;
/* No files, external programs or platform codecs are opened by the decoder. */
Animation *animation_decode(const uint8_t *data, size_t size, size_t budget, char *error,
                            size_t error_size);
void animation_free(Animation *a);

typedef struct {
    int frame, count;
    bool playing;
    uint64_t interval, next;
} AnimationPlayer;
void animation_player_init(AnimationPlayer *p, int count, double fps, bool autoplay, uint64_t now);
bool animation_player_tick(AnimationPlayer *p, uint64_t now);
void animation_player_toggle(AnimationPlayer *p, uint64_t now);
int animation_player_wait(const AnimationPlayer *p, uint64_t now);

#endif
