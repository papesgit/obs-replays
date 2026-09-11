#include "replay-channel-source.h"

#include <obs-module.h>
extern "C" {
#include <media-playback/media-playback.h>
}

namespace obs_replays {
namespace {
constexpr const char *channel_a_source_id = "obs_replays_channel_a";
constexpr const char *channel_b_source_id = "obs_replays_channel_b";
const char *get_channel_a_name(void *) { return "OBS Replays Channel A"; }
const char *get_channel_b_name(void *) { return "OBS Replays Channel B"; }
} // namespace

ReplayChannelSource::ReplayChannelSource(obs_source_t *source, ReplayChannel channel)
	: source(source), replayChannel(channel)
{
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
		}
		activePlayer = 0;
	}
	for (media_playback_t *decoder : decoders) {
		if (decoder) {
			media_playback_stop(decoder);
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

bool ReplayChannelSource::takeCued(QString *error)
{
	media_playback_t *decoder = nullptr;
	{
		std::lock_guard lock(mutex);
		const int nextPlayer = activePlayer == 0 ? 1 : 0;
		if (players[nextPlayer].state != PlayerState::Cued || !players[nextPlayer].decoder) {
			*error = "The next replay event is not cued yet.";
			return false;
		}
		players[activePlayer].state = PlayerState::Idle;
		activePlayer = nextPlayer;
		players[activePlayer].state = PlayerState::Playing;
		decoder = players[activePlayer].decoder;
	}
	obs_source_show_preloaded_video(source);
	media_playback_play_pause(decoder, false);
	blog(LOG_INFO, "[obs-replays] Taking preloaded Channel %c player %d to air.",
	     replayChannel == ReplayChannel::A ? 'A' : 'B', activePlayer + 1);
	obs_source_media_started(source);
	return true;
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
	{ std::lock_guard lock(mutex); decoder = players[playerIndex].decoder; players[playerIndex] = {}; }
	if (decoder) { media_playback_stop(decoder); media_playback_destroy(decoder); }
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
	std::lock_guard lock(mutex);
	if (playerIndex == activePlayer && players[playerIndex].state == PlayerState::Playing)
		obs_source_output_audio(source, audio);
}

} // namespace obs_replays
