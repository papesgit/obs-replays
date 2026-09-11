#pragma once

#include <atomic>

#include <QMutex>
#include <QString>

struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct obs_output;
typedef struct obs_output obs_output_t;
struct video_output;
typedef struct video_output video_t;
struct audio_output;
typedef struct audio_output audio_t;
struct calldata;
typedef struct calldata calldata_t;

namespace obs_replays {

class SegmentWriter {
public:
	~SegmentWriter();

	bool start(video_t *video, audio_t *audio, const QString &path, int videoBitrateMbps,
		   int audioBitrateKbps, int segmentDurationSeconds, QString *error);
	void stop();
	void forceStop();
	void release();

	bool isActive() const;
	bool hasStopped() const;
	bool requestSplit(QString *error);
	QString latestSegmentPath() const;

private:
	static void onFileChanged(void *param, calldata_t *data);
	static void onStopped(void *param, calldata_t *data);

	obs_encoder_t *videoEncoder = nullptr;
	obs_encoder_t *audioEncoder = nullptr;
	obs_output_t *output = nullptr;
	mutable QMutex mutex;
	QString latestPath;
	std::atomic<bool> stopped = false;
};

} // namespace obs_replays
