#pragma once

#include <QString>
#include <QByteArray>

#include <array>
#include <mutex>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_data;
typedef struct obs_data obs_data_t;
struct obs_source_frame;
struct obs_source_audio;
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
	bool load(const QString &path, qint64 positionMilliseconds, QString *error);
	bool cueNext(const QString &path, qint64 positionMilliseconds, QString *error);
	bool takeCued(QString *error);

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
	struct Slot {
		media_playback_t *decoder = nullptr;
		QByteArray path;
		qint64 positionMilliseconds = 0;
		PlayerState state = PlayerState::Idle;
	};
	struct PlayerCallback {
		ReplayChannelSource *channel = nullptr;
		int playerIndex = 0;
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

	bool loadSlot(int playerIndex, const QString &path, qint64 positionMilliseconds,
		      PlayerState state, QString *error);
	void receiveVideo(int playerIndex, obs_source_frame *frame, bool seekFrame);
	void receiveAudio(int playerIndex, obs_source_audio *audio);
	void releaseSlot(int playerIndex);

	obs_source_t *source = nullptr;
	ReplayChannel replayChannel;
	// "slots" is a Qt keyword macro in OBS's Qt-enabled build, so use a
	// non-Qt identifier here.
	std::array<Slot, 2> players;
	std::array<PlayerCallback, 2> playerCallbacks;
	std::mutex mutex;
	int activePlayer = 0;
	bool active = false;
};

} // namespace obs_replays
