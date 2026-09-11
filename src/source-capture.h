#pragma once

#include <atomic>
#include <cstdint>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_view;
typedef struct obs_view obs_view_t;
struct video_output;
typedef struct video_output video_t;
struct video_data;
struct audio_data;

namespace obs_replays {

class SourceCapture {
public:
	~SourceCapture();

	bool start(obs_source_t *source, const char **error);
	void stop();

	bool isActive() const;
	video_t *videoOutput() const;
	uint64_t timelineUs() const;
	uint64_t capturedVideoFrames() const;
	uint64_t capturedAudioFrames() const;

private:
	static void onVideoFrame(void *param, struct video_data *frame);
	static void onAudioFrame(void *param, obs_source_t *source,
			 const struct audio_data *audioData, bool muted);

	obs_source_t *source = nullptr;
	obs_view_t *view = nullptr;
	video_t *video = nullptr;
	std::atomic<uint64_t> firstVideoTimestampNs = 0;
	std::atomic<uint64_t> lastVideoTimestampNs = 0;
	std::atomic<uint64_t> videoFrames = 0;
	std::atomic<uint64_t> audioFrames = 0;
	bool videoConnected = false;
	bool audioConnected = false;
};

} // namespace obs_replays
