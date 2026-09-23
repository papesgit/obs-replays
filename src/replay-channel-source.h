#pragma once

#include <obs.h>

#include <QString>
#include <QByteArray>

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct media_playback;
typedef struct media_playback media_playback_t;

namespace obs_replays {

enum class ReplayChannel { A, B };

class ReplayChannelSource {
public:
	static void registerSourceTypes();
	static ReplayChannelSource *fromSource(obs_source_t *source);

	ReplayChannel channel() const;
	void reset();
	bool hasPendingStartOrCue();
	static void shutdownDecoderCleanup();
	bool load(const QString &path, qint64 positionMilliseconds, QString *error);
	bool cueNext(const QString &path, qint64 positionMilliseconds, QString *error);
	bool takeCued(int fadeDurationMilliseconds, QString *error);
	void setPlaybackSpeed(int percent);

private:
	enum class PlayerState {
		Idle,
		// The first decoder frame primes media-playback's internal state.
		LoadingActive,
		LoadingCued,
		SeekingActive,
		SeekingCued,
		Cued,
		Playing,
	};
	struct PlayerCallback;
	struct Slot {
		media_playback_t *decoder = nullptr;
		std::shared_ptr<PlayerCallback> callback;
		QByteArray path;
		qint64 positionMilliseconds = 0;
		PlayerState state = PlayerState::Idle;
		bool mediaStarted = false;
	};
	struct PlayerCallback {
		std::mutex mutex;
		ReplayChannelSource *channel = nullptr;
		int playerIndex = 0;
	};
	struct CachedVideoFrame {
		obs_source_frame frame = {};
		std::array<std::vector<uint8_t>, MAX_AV_PLANES> data;
		bool valid = false;
	};
	struct CachedAudioFrame {
		obs_source_audio audio = {};
		std::array<std::vector<uint8_t>, MAX_AV_PLANES> data;
	};

	ReplayChannelSource(obs_source_t *source, ReplayChannel channel);
	~ReplayChannelSource();

	static void *createA(obs_data_t *settings, obs_source_t *source);
	static void *createB(obs_data_t *settings, obs_source_t *source);
	static void destroy(void *data);
	static void activate(void *data);
	static void deactivate(void *data);
	static void videoFrame(void *data, obs_source_frame *frame);
	static void seekFrame(void *data, obs_source_frame *frame);
	static void audioFrame(void *data, obs_source_audio *audio);
	static void playbackStopped(void *data);

	bool loadSlot(int playerIndex, const QString &path, qint64 positionMilliseconds, PlayerState state,
		      QString *error);
	void receiveVideo(int playerIndex, obs_source_frame *frame, bool seekFrame);
	void receiveAudio(int playerIndex, obs_source_audio *audio);
	void releaseSlot(int playerIndex);
	static bool cacheVideoFrame(CachedVideoFrame &destination, const obs_source_frame *source);
	static bool blendVideoFrames(const CachedVideoFrame &outgoing, const CachedVideoFrame &incoming,
				     float incomingOpacity, CachedVideoFrame &destination);
	static CachedAudioFrame cacheAudioFrame(const obs_source_audio *source);
	static CachedAudioFrame resampleAudioFrame(const obs_source_audio *source, int speedPercent);
	static bool blendAudioFrames(const CachedAudioFrame &outgoing, const CachedAudioFrame &incoming,
				     float incomingGain, CachedAudioFrame &destination);
	float transitionProgressLocked(uint64_t nowNs) const;
	void outputTransitionVideo(int playerIndex, obs_source_frame *frame);
	void outputTransitionAudio(int playerIndex, CachedAudioFrame audio);

	obs_source_t *source = nullptr;
	ReplayChannel replayChannel;
	// "slots" is a Qt keyword macro in OBS's Qt-enabled build, so use a
	// non-Qt identifier here.
	std::array<Slot, 2> players;
	std::array<CachedVideoFrame, 2> cachedVideo;
	std::array<std::deque<CachedAudioFrame>, 2> pendingAudio;
	std::mutex mutex;
	int activePlayer = 0;
	int playbackSpeedPercent = 100;
	int deferredPlaybackSpeedPercent = 100;
	bool hasDeferredPlaybackSpeed = false;
	int fadingOutPlayer = -1;
	uint64_t fadeStartNs = 0;
	uint64_t fadeDurationNs = 0;
	bool active = false;
};

} // namespace obs_replays
