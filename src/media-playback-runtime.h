#pragma once

#include <stdint.h>

struct mp_decode;
struct mp_media;
struct media_playback;
typedef struct media_playback media_playback_t;

void obs_replays_scale_decode_timestamp(struct mp_decode *decoder, int64_t *timestamp, int64_t *duration);
void obs_replays_set_media_speed(struct mp_media *media, int speed);
void obs_replays_release_media_speed_state(struct mp_media *media);
void obs_replays_destroy_media_playback(media_playback_t *playback);
