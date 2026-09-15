#include "replay-channel-source.h"

#include <obs-module.h>
#include <util/platform.h>
extern "C" {
#include <media-playback/media-playback.h>
void media_playback_set_speed(media_playback_t *playback, int speed);
void media_playback_release_speed_state(media_playback_t *playback);
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace obs_replays {
namespace {
constexpr const char *channel_a_source_id = "obs_replays_channel_a";
constexpr const char *channel_b_source_id = "obs_replays_channel_b";
const char *get_channel_a_name(void *) { return "OBS Replays Channel A"; }
const char *get_channel_b_name(void *) { return "OBS Replays Channel B"; }

uint32_t video_plane_rows(enum video_format format, uint32_t height, size_t plane)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return plane == 0 ? height : (plane < 3 ? (height + 1) / 2 : 0);
	case VIDEO_FORMAT_NV12:
		return plane == 0 ? height : (plane == 1 ? (height + 1) / 2 : 0);
	default:
		return 0;
	}
}
} // namespace

ReplayChannelSource::ReplayChannelSource(obs_source_t *source, ReplayChannel channel)
	: source(source), replayChannel(channel)
{
	// The media decoder already supplies a shared, rate-mapped clock for video
	// and audio.  OBS's usual asynchronous audio smoother interprets a live
	// rate adjustment as timestamp jitter and can accumulate audio behind the
	// video, so this source owns that synchronisation directly.
	obs_source_set_async_unbuffered(source, true);
	obs_source_set_async_decoupled(source, true);
	for (int index = 0; index < 2; ++index)
		playerCallbacks[index] = {this, index};
}

ReplayChannelSource::~ReplayChannelSource() { reset(); }

void ReplayChannelSource::registerSourceTypes()
{
	obs_source_info channelA = {};
	channelA.id = channel_a_source_id;
	channelA.type = OBS_SOURCE_TYPE_INPUT;
	channelA.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC |
				OBS_SOURCE_CONTROLLABLE_MEDIA;
	channelA.get_name = get_channel_a_name;
	channelA.create = createA;
	channelA.destroy = destroy;
	channelA.activate = activate;
	channelA.deactivate = deactivate;
	channelA.icon_type = OBS_ICON_TYPE_MEDIA;
	obs_register_source(&channelA);
	obs_source_info channelB = channelA;
	channelB.id = channel_b_source_id;
	channelB.get_name = get_channel_b_name;
	channelB.create = createB;
	obs_register_source(&channelB);
}

ReplayChannelSource *ReplayChannelSource::fromSource(obs_source_t *source)
{
	return source ? static_cast<ReplayChannelSource *>(obs_obj_get_data(source)) : nullptr;
}

ReplayChannel ReplayChannelSource::channel() const { return replayChannel; }

void ReplayChannelSource::reset()
{
	std::array<media_playback_t *, 2> decoders = {};
	{
		std::lock_guard lock(mutex);
		for (int index = 0; index < 2; ++index) {
			decoders[index] = players[index].decoder;
			players[index] = {};
			cachedVideo[index] = {};
			pendingAudio[index].clear();
		}
		activePlayer = 0;
		deferredPlaybackSpeedPercent = playbackSpeedPercent;
		hasDeferredPlaybackSpeed = false;
		fadingOutPlayer = -1;
		fadeStartNs = 0;
		fadeDurationNs = 0;
	}
	for (media_playback_t *decoder : decoders) {
		if (decoder) {
			media_playback_stop(decoder);
			media_playback_release_speed_state(decoder);
			media_playback_destroy(decoder);
		}
	}
	if (source)
		obs_source_output_video(source, nullptr);
}

bool ReplayChannelSource::load(const QString &path, qint64 positionMilliseconds, QString *error)
{
	return loadSlot(0, path, positionMilliseconds, PlayerState::LoadingActive, error);
}

bool ReplayChannelSource::cueNext(const QString &path, qint64 positionMilliseconds, QString *error)
{
	int nextPlayer = 0;
	{ std::lock_guard lock(mutex); nextPlayer = activePlayer == 0 ? 1 : 0; }
	return loadSlot(nextPlayer, path, positionMilliseconds, PlayerState::LoadingCued, error);
}

bool ReplayChannelSource::takeCued(int fadeDurationMilliseconds, QString *error)
{
	media_playback_t *decoder = nullptr;
	const bool fade = fadeDurationMilliseconds > 0;
	{
		std::lock_guard lock(mutex);
		const int nextPlayer = activePlayer == 0 ? 1 : 0;
		if (players[nextPlayer].state != PlayerState::Cued || !players[nextPlayer].decoder) {
			*error = "The next replay event is not cued yet.";
			return false;
		}
		const int outgoingPlayer = activePlayer;
		if (fade) {
			fadingOutPlayer = outgoingPlayer;
			fadeStartNs = os_gettime_ns();
			fadeDurationNs = static_cast<uint64_t>(fadeDurationMilliseconds) * 1000000ULL;
			deferredPlaybackSpeedPercent = playbackSpeedPercent;
			hasDeferredPlaybackSpeed = false;
			// A player may still hold a frame from the preceding transition.
			// Do not blend that stale image into this boundary; wait for its
			// next decoded on-air frame instead.
			cachedVideo[outgoingPlayer] = {};
			pendingAudio[outgoingPlayer].clear();
			pendingAudio[nextPlayer].clear();
		} else {
			players[outgoingPlayer].state = PlayerState::Idle;
		}
		activePlayer = nextPlayer;
		players[activePlayer].state = PlayerState::Playing;
		decoder = players[activePlayer].decoder;
	}
	if (!fade)
		obs_source_show_preloaded_video(source);
	media_playback_play_pause(decoder, false);
	blog(LOG_INFO, "[obs-replays] Taking preloaded Channel %c player %d to air%s.",
	     replayChannel == ReplayChannel::A ? 'A' : 'B', activePlayer + 1,
	     fade ? " with an event fade" : "");
	obs_source_media_started(source);
	return true;
}

void ReplayChannelSource::setPlaybackSpeed(int percent)
{
	percent = std::clamp(percent, 10, 100);
	std::array<media_playback_t *, 2> decoders = {};
	bool applySpeed = false;
	{
		std::lock_guard lock(mutex);
		// Both lanes must retain one stable rate during an audio crossfade.
		// Remember the latest requested value and apply it as the fade ends.
		if (fadingOutPlayer >= 0) {
			deferredPlaybackSpeedPercent = percent;
			hasDeferredPlaybackSpeed = true;
			return;
		}
		if (playbackSpeedPercent != percent) {
			pendingAudio[0].clear();
			pendingAudio[1].clear();
			playbackSpeedPercent = percent;
			applySpeed = true;
		}
		for (int index = 0; index < 2; ++index)
			decoders[index] = players[index].decoder;
	}
	if (!applySpeed)
		return;
	for (media_playback_t *decoder : decoders) {
		if (decoder)
			media_playback_set_speed(decoder, percent);
	}
}

bool ReplayChannelSource::loadSlot(int playerIndex, const QString &path, qint64 positionMilliseconds,
				   PlayerState state, QString *error)
{
	if (path.isEmpty()) { *error = "Replay event media path is empty."; return false; }
	releaseSlot(playerIndex);
	const QByteArray utf8Path = path.toUtf8();
	{
		std::lock_guard lock(mutex);
		players[playerIndex].path = utf8Path;
		players[playerIndex].positionMilliseconds = positionMilliseconds;
		players[playerIndex].state = state;
	}
	struct mp_media_info info = {};
	info.opaque = &playerCallbacks[playerIndex];
	info.v_cb = videoFrame;
	info.v_preload_cb = seekFrame;
	info.v_seek_cb = seekFrame;
	info.a_cb = audioFrame;
	info.stop_cb = playbackStopped;
	info.path = utf8Path.constData();
	{
		std::lock_guard lock(mutex);
		info.speed = playbackSpeedPercent;
	}
	info.is_local_file = true;
	info.request_preload = true;
	media_playback_t *decoder = media_playback_create(&info);
	if (!decoder) {
		std::lock_guard lock(mutex);
		players[playerIndex].state = PlayerState::Idle;
		*error = "OBS could not open the replay event media.";
		return false;
	}
	{ std::lock_guard lock(mutex); players[playerIndex].decoder = decoder; }
	blog(LOG_INFO, "[obs-replays] Opening Channel %c player %d at %lld ms: %s",
	     replayChannel == ReplayChannel::A ? 'A' : 'B', playerIndex + 1,
	     static_cast<long long>(positionMilliseconds), utf8Path.constData());
	media_playback_play(decoder, false, false);
	return true;
}

void ReplayChannelSource::releaseSlot(int playerIndex)
{
	media_playback_t *decoder = nullptr;
	{
		std::lock_guard lock(mutex);
		decoder = players[playerIndex].decoder;
		players[playerIndex] = {};
		cachedVideo[playerIndex] = {};
		pendingAudio[playerIndex].clear();
	}
	if (decoder) {
		media_playback_stop(decoder);
		media_playback_release_speed_state(decoder);
		media_playback_destroy(decoder);
	}
}

bool ReplayChannelSource::cacheVideoFrame(CachedVideoFrame &destination,
					  const obs_source_frame *sourceFrame)
{
	if (!sourceFrame || (sourceFrame->format != VIDEO_FORMAT_I420 &&
			     sourceFrame->format != VIDEO_FORMAT_NV12))
		return false;

	CachedVideoFrame copy = {};
	copy.frame = *sourceFrame;
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
		const uint32_t rows = video_plane_rows(sourceFrame->format, sourceFrame->height, plane);
		if (!rows)
			continue;
		if (!sourceFrame->data[plane] || !sourceFrame->linesize[plane])
			return false;
		const size_t bytes = static_cast<size_t>(sourceFrame->linesize[plane]) * rows;
		copy.data[plane].assign(sourceFrame->data[plane], sourceFrame->data[plane] + bytes);
		copy.frame.data[plane] = copy.data[plane].data();
	}
	copy.valid = true;
	destination = std::move(copy);
	return true;
}

bool ReplayChannelSource::blendVideoFrames(const CachedVideoFrame &outgoing,
					   const CachedVideoFrame &incoming, float incomingOpacity,
					   CachedVideoFrame &destination)
{
	if (!outgoing.valid || !incoming.valid || outgoing.frame.format != incoming.frame.format ||
	    outgoing.frame.width != incoming.frame.width || outgoing.frame.height != incoming.frame.height ||
	    (outgoing.frame.format != VIDEO_FORMAT_I420 && outgoing.frame.format != VIDEO_FORMAT_NV12))
		return false;

	CachedVideoFrame blend = outgoing;
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
		const uint32_t rows = video_plane_rows(blend.frame.format, blend.frame.height, plane);
		if (!rows)
			continue;
		if (blend.frame.linesize[plane] != incoming.frame.linesize[plane])
			return false;
		const size_t bytes = static_cast<size_t>(blend.frame.linesize[plane]) * rows;
		for (size_t index = 0; index < bytes; ++index) {
			const float mixed = outgoing.data[plane][index] * (1.0f - incomingOpacity) +
					    incoming.data[plane][index] * incomingOpacity;
			blend.data[plane][index] = static_cast<uint8_t>(std::clamp(mixed, 0.0f, 255.0f));
		}
		blend.frame.data[plane] = blend.data[plane].data();
	}
	blend.valid = true;
	destination = std::move(blend);
	return true;
}

ReplayChannelSource::CachedAudioFrame ReplayChannelSource::cacheAudioFrame(
	const obs_source_audio *sourceAudio)
{
	CachedAudioFrame copy = {};
	if (!sourceAudio || sourceAudio->format == AUDIO_FORMAT_UNKNOWN || !sourceAudio->frames)
		return copy;
	copy.audio = *sourceAudio;
	const size_t planes = get_audio_planes(sourceAudio->format, sourceAudio->speakers);
	const size_t bytesPerPlane = get_audio_size(sourceAudio->format, sourceAudio->speakers,
						      sourceAudio->frames);
	for (size_t plane = 0; plane < planes; ++plane) {
		if (!sourceAudio->data[plane])
			return {};
		copy.data[plane].assign(sourceAudio->data[plane],
					       sourceAudio->data[plane] + bytesPerPlane);
		copy.audio.data[plane] = copy.data[plane].data();
	}
	return copy;
}

namespace {
template<typename Sample>
void resample_audio_plane(const uint8_t *inputBytes, uint8_t *outputBytes, uint32_t inputFrames,
			  uint32_t outputFrames, size_t channels, bool planar)
{
	const auto *input = reinterpret_cast<const Sample *>(inputBytes);
	auto *output = reinterpret_cast<Sample *>(outputBytes);
	const double step = static_cast<double>(inputFrames) / outputFrames;
	const size_t samplesPerFrame = planar ? 1 : channels;
	for (uint32_t outputFrame = 0; outputFrame < outputFrames; ++outputFrame) {
		const double sourcePosition = outputFrame * step;
		const uint32_t firstFrame = std::min<uint32_t>(
			static_cast<uint32_t>(sourcePosition), inputFrames - 1);
		const uint32_t secondFrame = std::min<uint32_t>(firstFrame + 1, inputFrames - 1);
		const double fraction = sourcePosition - firstFrame;
		for (size_t channel = 0; channel < samplesPerFrame; ++channel) {
			const double first = input[firstFrame * samplesPerFrame + channel];
			const double second = input[secondFrame * samplesPerFrame + channel];
			output[outputFrame * samplesPerFrame + channel] =
				static_cast<Sample>(first + (second - first) * fraction);
		}
	}
}
} // namespace

ReplayChannelSource::CachedAudioFrame ReplayChannelSource::resampleAudioFrame(
	const obs_source_audio *sourceAudio, int speedPercent)
{
	CachedAudioFrame output = {};
	if (!sourceAudio || !sourceAudio->frames || !sourceAudio->samples_per_sec)
		return output;

	obs_audio_info outputInfo = {};
	const uint32_t outputRate = obs_get_audio_info(&outputInfo) && outputInfo.samples_per_sec
		? outputInfo.samples_per_sec
		: sourceAudio->samples_per_sec;
	const double sourceStep = static_cast<double>(sourceAudio->samples_per_sec) *
		std::clamp(speedPercent, 10, 100) / (static_cast<double>(outputRate) * 100.0);
	const uint32_t outputFrames = std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(
		sourceAudio->frames / sourceStep)));
	if (sourceAudio->format != AUDIO_FORMAT_FLOAT && sourceAudio->format != AUDIO_FORMAT_FLOAT_PLANAR &&
	    sourceAudio->format != AUDIO_FORMAT_16BIT && sourceAudio->format != AUDIO_FORMAT_16BIT_PLANAR)
		return cacheAudioFrame(sourceAudio);

	output.audio = *sourceAudio;
	output.audio.samples_per_sec = outputRate;
	output.audio.frames = outputFrames;
	const bool planar = is_audio_planar(sourceAudio->format);
	const size_t planes = get_audio_planes(sourceAudio->format, sourceAudio->speakers);
	const size_t channels = get_audio_channels(sourceAudio->speakers);
	for (size_t plane = 0; plane < planes; ++plane) {
		if (!sourceAudio->data[plane])
			return {};
		output.data[plane].resize(get_audio_size(sourceAudio->format, sourceAudio->speakers,
							   outputFrames));
		if (sourceAudio->format == AUDIO_FORMAT_FLOAT ||
		    sourceAudio->format == AUDIO_FORMAT_FLOAT_PLANAR)
			resample_audio_plane<float>(sourceAudio->data[plane], output.data[plane].data(),
						    sourceAudio->frames, outputFrames, channels, planar);
		else
			resample_audio_plane<int16_t>(sourceAudio->data[plane], output.data[plane].data(),
						      sourceAudio->frames, outputFrames, channels, planar);
		output.audio.data[plane] = output.data[plane].data();
	}
	return output;
}

bool ReplayChannelSource::blendAudioFrames(const CachedAudioFrame &outgoing,
					   const CachedAudioFrame &incoming, float incomingGain,
					   CachedAudioFrame &destination)
{
	if (!outgoing.audio.frames || outgoing.audio.frames != incoming.audio.frames ||
	    outgoing.audio.format != incoming.audio.format ||
	    outgoing.audio.speakers != incoming.audio.speakers ||
	    outgoing.audio.samples_per_sec != incoming.audio.samples_per_sec)
		return false;

	CachedAudioFrame blend = outgoing;
	blend.audio.timestamp = std::max(outgoing.audio.timestamp, incoming.audio.timestamp);
	const float outgoingGain = std::sqrt(std::max(0.0f, 1.0f - incomingGain * incomingGain));
	const size_t planes = get_audio_planes(blend.audio.format, blend.audio.speakers);
	const size_t samples = is_audio_planar(blend.audio.format)
				   ? blend.audio.frames
				   : static_cast<size_t>(blend.audio.frames) * get_audio_channels(blend.audio.speakers);
	for (size_t plane = 0; plane < planes; ++plane) {
		if (blend.audio.format == AUDIO_FORMAT_FLOAT || blend.audio.format == AUDIO_FORMAT_FLOAT_PLANAR) {
			auto *out = reinterpret_cast<float *>(blend.data[plane].data());
			const auto *in = reinterpret_cast<const float *>(incoming.data[plane].data());
			for (size_t sample = 0; sample < samples; ++sample)
				out[sample] = out[sample] * outgoingGain + in[sample] * incomingGain;
		} else if (blend.audio.format == AUDIO_FORMAT_16BIT ||
			   blend.audio.format == AUDIO_FORMAT_16BIT_PLANAR) {
			auto *out = reinterpret_cast<int16_t *>(blend.data[plane].data());
			const auto *in = reinterpret_cast<const int16_t *>(incoming.data[plane].data());
			for (size_t sample = 0; sample < samples; ++sample) {
				const float value = out[sample] * outgoingGain + in[sample] * incomingGain;
				out[sample] = static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
			}
		} else {
			return false;
		}
		blend.audio.data[plane] = blend.data[plane].data();
	}
	destination = std::move(blend);
	return true;
}

float ReplayChannelSource::transitionProgressLocked(uint64_t nowNs) const
{
	if (fadingOutPlayer < 0 || !fadeDurationNs)
		return 1.0f;
	return std::clamp(static_cast<float>(nowNs - fadeStartNs) / fadeDurationNs, 0.0f, 1.0f);
}

void *ReplayChannelSource::createA(obs_data_t *, obs_source_t *source) { return new ReplayChannelSource(source, ReplayChannel::A); }
void *ReplayChannelSource::createB(obs_data_t *, obs_source_t *source) { return new ReplayChannelSource(source, ReplayChannel::B); }
void ReplayChannelSource::destroy(void *data) { delete static_cast<ReplayChannelSource *>(data); }
void ReplayChannelSource::activate(void *data) { if (auto *channel = static_cast<ReplayChannelSource *>(data)) channel->active = true; }
void ReplayChannelSource::deactivate(void *data) { if (auto *channel = static_cast<ReplayChannelSource *>(data)) channel->active = false; }

void ReplayChannelSource::videoFrame(void *data, obs_source_frame *frame)
{
	auto *callback = static_cast<PlayerCallback *>(data);
	if (callback && callback->channel) callback->channel->receiveVideo(callback->playerIndex, frame, false);
}
void ReplayChannelSource::seekFrame(void *data, obs_source_frame *frame)
{
	auto *callback = static_cast<PlayerCallback *>(data);
	if (callback && callback->channel) callback->channel->receiveVideo(callback->playerIndex, frame, true);
}
void ReplayChannelSource::audioFrame(void *data, obs_source_audio *audio)
{
	auto *callback = static_cast<PlayerCallback *>(data);
	if (callback && callback->channel) callback->channel->receiveAudio(callback->playerIndex, audio);
}
void ReplayChannelSource::playbackStopped(void *data)
{
	auto *callback = static_cast<PlayerCallback *>(data);
	if (callback && callback->channel)
		blog(LOG_WARNING, "[obs-replays] Channel %c player %d stopped before completing playout.",
		     callback->channel->channel() == ReplayChannel::A ? 'A' : 'B',
		     callback->playerIndex + 1);
}

void ReplayChannelSource::outputTransitionVideo(int playerIndex, obs_source_frame *frame)
{
	CachedVideoFrame blended = {};
	media_playback_t *decoderToPause = nullptr;
	media_playback_t *decoderToRetune = nullptr;
	int deferredSpeed = 100;
	bool outputCurrent = false;
	bool outputBlended = false;
	{
		std::lock_guard lock(mutex);
		if (fadingOutPlayer < 0)
			return;
		cacheVideoFrame(cachedVideo[playerIndex], frame);
		const float progress = transitionProgressLocked(os_gettime_ns());
		if (cachedVideo[fadingOutPlayer].valid && cachedVideo[activePlayer].valid &&
		    blendVideoFrames(cachedVideo[fadingOutPlayer], cachedVideo[activePlayer], progress, blended)) {
			blended.frame.timestamp = frame->timestamp;
			outputBlended = true;
		}
		if (!outputBlended && playerIndex == fadingOutPlayer)
			outputCurrent = true;
		if (progress >= 1.0f) {
			decoderToPause = players[fadingOutPlayer].decoder;
			players[fadingOutPlayer].state = PlayerState::Idle;
			pendingAudio[fadingOutPlayer].clear();
			fadingOutPlayer = -1;
			fadeStartNs = 0;
			fadeDurationNs = 0;
			if (hasDeferredPlaybackSpeed) {
				deferredSpeed = deferredPlaybackSpeedPercent;
				hasDeferredPlaybackSpeed = false;
				if (playbackSpeedPercent != deferredSpeed) {
					playbackSpeedPercent = deferredSpeed;
					pendingAudio[activePlayer].clear();
					decoderToRetune = players[activePlayer].decoder;
				}
			}
			outputCurrent = playerIndex == activePlayer;
		}
	}
	if (decoderToPause)
		media_playback_play_pause(decoderToPause, true);
	if (decoderToRetune)
		media_playback_set_speed(decoderToRetune, deferredSpeed);
	if (outputBlended) {
		obs_source_set_video_frame(source, &blended.frame);
		obs_source_output_video(source, &blended.frame);
	} else if (outputCurrent) {
		obs_source_set_video_frame(source, frame);
		obs_source_output_video(source, frame);
	}
}

void ReplayChannelSource::outputTransitionAudio(int playerIndex, CachedAudioFrame audio)
{
	CachedAudioFrame mixed = {};
	bool outputMixed = false;
	{
		std::lock_guard lock(mutex);
		if (fadingOutPlayer < 0)
			return;
		if (!audio.audio.frames)
			return;
		auto &queue = pendingAudio[playerIndex];
		queue.emplace_back(std::move(audio));
		if (queue.size() > 4)
			queue.pop_front();
		auto &outgoing = pendingAudio[fadingOutPlayer];
		auto &incoming = pendingAudio[activePlayer];
		if (outgoing.empty() || incoming.empty())
			return;
		const float linearProgress = transitionProgressLocked(os_gettime_ns());
		constexpr float halfPi = 1.57079632679f;
		const float incomingGain = std::sin(linearProgress * halfPi);
		if (blendAudioFrames(outgoing.front(), incoming.front(), incomingGain, mixed)) {
			outgoing.pop_front();
			incoming.pop_front();
			outputMixed = true;
		} else {
			mixed = std::move(incoming.front());
			incoming.pop_front();
			outgoing.pop_front();
			outputMixed = true;
		}
	}
	if (outputMixed)
		obs_source_output_audio(source, &mixed.audio);
}

void ReplayChannelSource::receiveVideo(int playerIndex, obs_source_frame *frame, bool isSeekFrame)
{
	media_playback_t *decoderToPause = nullptr;
	media_playback_t *decoderToResume = nullptr;
	media_playback_t *decoderToSeek = nullptr;
	qint64 seekPositionMilliseconds = 0;
	bool outputActive = false, preloadCue = false, signalStarted = false, setSeekFrame = false;
	{
		std::lock_guard lock(mutex);
		if (playerIndex < 0 || playerIndex > 1 || !players[playerIndex].decoder) return;
		if (!isSeekFrame && players[playerIndex].state == PlayerState::LoadingCued) {
			players[playerIndex].state = PlayerState::SeekingCued;
			decoderToSeek = players[playerIndex].decoder;
			seekPositionMilliseconds = players[playerIndex].positionMilliseconds;
		} else if (!isSeekFrame && players[playerIndex].state == PlayerState::LoadingActive) {
			players[playerIndex].state = PlayerState::SeekingActive;
			decoderToSeek = players[playerIndex].decoder;
			seekPositionMilliseconds = players[playerIndex].positionMilliseconds;
		} else if (isSeekFrame && players[playerIndex].state == PlayerState::SeekingCued) {
			players[playerIndex].state = PlayerState::Cued;
			decoderToPause = players[playerIndex].decoder;
			preloadCue = true;
		} else if (isSeekFrame && players[playerIndex].state == PlayerState::SeekingActive) {
			players[playerIndex].state = PlayerState::Playing;
			decoderToResume = players[playerIndex].decoder;
			outputActive = signalStarted = true;
			setSeekFrame = true;
		} else if (playerIndex == activePlayer && players[playerIndex].state == PlayerState::Playing) {
			outputActive = true;
		}
	}
	if (decoderToSeek) {
		// The media thread initializes lazily and resets its pause state once.
		// Prime it with one discarded frame, then pause and seek to obtain the
		// reliable v_seek_cb frame for the requested event position.
		media_playback_play_pause(decoderToSeek, true);
		media_playback_seek(decoderToSeek, seekPositionMilliseconds);
		blog(LOG_INFO, "[obs-replays] Channel %c player %d primed; seeking to %lld ms.",
		     replayChannel == ReplayChannel::A ? 'A' : 'B', playerIndex + 1,
		     static_cast<long long>(seekPositionMilliseconds));
	}
	if (preloadCue) {
		CachedVideoFrame cueFrame = {};
		if (cacheVideoFrame(cueFrame, frame)) {
			std::lock_guard lock(mutex);
			cachedVideo[playerIndex] = std::move(cueFrame);
		}
		obs_source_preload_video(source, frame);
		media_playback_play_pause(decoderToPause, true);
		blog(LOG_INFO, "[obs-replays] Channel %c player %d is cued.",
		     replayChannel == ReplayChannel::A ? 'A' : 'B', playerIndex + 1);
	}
	if (setSeekFrame) {
		// This is the exact seek hand-off used by OBS's own ffmpeg_source.
		// It establishes the source's frame state before the decoder resumes
		// normal asynchronous frame output.
		obs_source_set_video_frame(source, frame);
		blog(LOG_INFO, "[obs-replays] Channel %c player %d seek frame is ready.",
		     replayChannel == ReplayChannel::A ? 'A' : 'B', playerIndex + 1);
		media_playback_play_pause(decoderToResume, false);
	}
	bool transitionOutput = false;
	if (!isSeekFrame && !setSeekFrame) {
		std::lock_guard lock(mutex);
		transitionOutput = fadingOutPlayer >= 0 &&
			(playerIndex == activePlayer || playerIndex == fadingOutPlayer) &&
			players[playerIndex].state == PlayerState::Playing;
	}
	if (transitionOutput) {
		outputTransitionVideo(playerIndex, frame);
		return;
	}
	if (outputActive && !setSeekFrame) {
		// Keep OBS's retained async texture current as well as submitting the
		// timed frame. If the underlying segment reaches EOF while an outro
		// stinger is still rendering, that retained texture holds the final
		// replay frame instead of timing out to black.
		obs_source_set_video_frame(source, frame);
		obs_source_output_video(source, frame);
	}
	if (signalStarted) obs_source_media_started(source);
}

void ReplayChannelSource::receiveAudio(int playerIndex, obs_source_audio *audio)
{
	bool fading = false;
	bool outputActive = false;
	int speed = 100;
	{
		std::lock_guard lock(mutex);
		fading = fadingOutPlayer >= 0;
		outputActive = playerIndex == activePlayer && players[playerIndex].state == PlayerState::Playing;
		speed = playbackSpeedPercent;
	}
	CachedAudioFrame resampled = resampleAudioFrame(audio, speed);
	if (!resampled.audio.frames)
		return;
	if (fading) {
		outputTransitionAudio(playerIndex, std::move(resampled));
		return;
	}
	if (outputActive)
		obs_source_output_audio(source, &resampled.audio);
}

} // namespace obs_replays
