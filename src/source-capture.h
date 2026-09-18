#pragma once

#include <atomic>
#include <cstdint>

#include <QByteArray>
#include <QList>

class QMutex;
struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_view;
typedef struct obs_view obs_view_t;
struct video_output;
typedef struct video_output video_t;
struct video_data;
struct audio_data;
struct audio_output_data;
struct audio_output;
typedef struct audio_output audio_t;

namespace obs_replays {

class SourceCapture {
public:
	~SourceCapture();

	bool start(obs_source_t *source, const char **error);
	void stop();

	bool isActive() const;
	video_t *videoOutput() const;
	audio_t *audioOutput() const;
	uint64_t timelineUs() const;
	uint64_t capturedVideoFrames() const;
	uint64_t capturedAudioFrames() const;

private:
	struct AudioBlock {
		uint64_t timestampNs = 0;
		uint32_t frames = 0;
		uint32_t offset = 0;
		QList<QByteArray> planes;
	};

	static void onVideoFrame(void *param, struct video_data *frame);
	static void onAudioFrame(void *param, obs_source_t *source,
			 const struct audio_data *audioData, bool muted);
	static bool provideAudio(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
			 uint32_t activeMixers, struct audio_output_data *mixes);

	obs_source_t *source = nullptr;
	obs_view_t *view = nullptr;
	video_t *video = nullptr;
	audio_t *audio = nullptr;
	QMutex *audioMutex = nullptr;
	QList<AudioBlock> *audioBlocks = nullptr;
	QList<QByteArray> audioScratchPlanes;
	uint32_t audioSampleRate = 0;
	uint32_t audioChannels = 0;
	uint32_t queuedAudioFrames = 0;
	bool audioClockInitialized = false;
	int64_t audioTimestampOffsetNs = 0;
	uint64_t nextRawAudioTimestampNs = 0;
	bool audioOutputStarted = false;
	uint64_t audioPrimingStartedNs = 0;
	uint64_t audioOutputTimestampNs = 0;
	uint64_t lastAudioArrivalNs = 0;
	bool audioUnderrunDiagnosticPending = false;
	uint32_t audioTargetQueueFrames = 0;
	double audioReadFraction = 0.0;
	double audioQueueErrorFrames = 0.0;
	double audioRateRatio = 1.0;
	uint64_t lastAudioDiagnosticNs = 0;
	std::atomic<uint64_t> firstVideoTimestampNs = 0;
	std::atomic<uint64_t> lastVideoTimestampNs = 0;
	std::atomic<uint64_t> videoFrames = 0;
	std::atomic<uint64_t> audioFrames = 0;
	bool videoConnected = false;
	bool audioConnected = false;
	bool sourceActive = false;
};

} // namespace obs_replays
