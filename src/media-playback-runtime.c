/* Runtime controls for the media-playback helper compiled into this plugin. */

#include "media-playback-runtime.h"

#include <media-playback/media-playback.h>
#include <media-playback/cache.h>
#include <media-playback/decode.h>
#include <media-playback/media.h>

#include <util/bmem.h>

struct media_playback {
	bool is_cached;
	union {
		mp_media_t media;
		mp_cache_t cache;
	};
};

struct speed_state {
	struct mp_media *media;
	int speed;
	int64_t source_anchor;
	int64_t output_anchor;
	int64_t last_source;
	bool has_frame;
	struct speed_state *next;
};

static pthread_mutex_t speed_states_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct speed_state *speed_states;

static struct speed_state *get_speed_state_locked(struct mp_media *media)
{
	for (struct speed_state *state = speed_states; state; state = state->next) {
		if (state->media == media)
			return state;
	}

	struct speed_state *state = bzalloc(sizeof(*state));
	state->media = media;
	state->speed = media->speed;
	state->next = speed_states;
	speed_states = state;
	return state;
}

void obs_replays_scale_decode_timestamp(struct mp_decode *decoder, int64_t *timestamp,
					int64_t *duration)
{
	if (!decoder || !timestamp || !duration)
		return;

	pthread_mutex_lock(&speed_states_mutex);
	struct speed_state *state = get_speed_state_locked(decoder->m);
	const int64_t source_timestamp = *timestamp;
	const int64_t source_duration = *duration;
	const int speed = state->speed > 0 ? state->speed : 100;
	const int64_t output_timestamp = state->output_anchor +
		((source_timestamp - state->source_anchor) * 100) / speed;
	const int64_t output_duration = (source_duration * 100) / speed;
	// Both decoder tracks use this one affine clock.  They naturally have
	// different frame cadences, so separate per-track anchors would make a
	// live speed change rebase audio and video at different media positions.
	// A seek may legitimately jump backwards; otherwise retain the furthest
	// decoded timestamp as the shared playback pivot.
	if (!state->has_frame || source_timestamp < state->last_source - 1000000000LL)
		state->last_source = source_timestamp;
	else if (source_timestamp > state->last_source)
		state->last_source = source_timestamp;
	state->has_frame = true;
	pthread_mutex_unlock(&speed_states_mutex);

	*timestamp = output_timestamp;
	*duration = output_duration;
}

void obs_replays_set_media_speed(struct mp_media *media, int speed)
{
	if (!media)
		return;
	if (speed < 10)
		speed = 10;
	else if (speed > 100)
		speed = 100;

	pthread_mutex_lock(&speed_states_mutex);
	for (struct speed_state *state = speed_states; state; state = state->next) {
		if (state->media != media)
			continue;
		if (state->has_frame) {
			const int old_speed = state->speed > 0 ? state->speed : 100;
			const int64_t pivot_output = state->output_anchor +
				((state->last_source - state->source_anchor) * 100) / old_speed;
			state->source_anchor = state->last_source;
			state->output_anchor = pivot_output;
		}
		state->speed = speed;
	}
	pthread_mutex_unlock(&speed_states_mutex);

	pthread_mutex_lock(&media->mutex);
	media->speed = speed;
	pthread_mutex_unlock(&media->mutex);
}

void obs_replays_release_media_speed_state(struct mp_media *media)
{
	pthread_mutex_lock(&speed_states_mutex);
	struct speed_state **link = &speed_states;
	while (*link) {
		struct speed_state *state = *link;
		if (state->media == media) {
			*link = state->next;
			bfree(state);
		} else {
			link = &state->next;
		}
	}
	pthread_mutex_unlock(&speed_states_mutex);
}

void media_playback_set_speed(media_playback_t *playback, int speed)
{
	if (!playback)
		return;
	if (playback->is_cached) {
		pthread_mutex_lock(&playback->cache.mutex);
		playback->cache.speed = speed;
		playback->cache.m.speed = speed;
		pthread_mutex_unlock(&playback->cache.mutex);
	} else {
		obs_replays_set_media_speed(&playback->media, speed);
	}
}

void media_playback_release_speed_state(media_playback_t *playback)
{
	if (playback && !playback->is_cached)
		obs_replays_release_media_speed_state(&playback->media);
}
