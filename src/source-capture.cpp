#include "source-capture.h"

#include <obs-module.h>

#include <media-io/video-io.h>

namespace obs_replays {

SourceCapture::~SourceCapture()
{
	stop();
}

bool SourceCapture::start(obs_source_t *captureSource, const char **error)
{
	if (source) {
		*error = "The source capture is already active.";
		return false;
	}
	if (!captureSource) {
		*error = "No OBS source is selected for replay capture.";
		return false;
	}

	source = obs_source_get_ref(captureSource);
	firstVideoTimestampNs = 0;
	lastVideoTimestampNs = 0;
	videoFrames = 0;
	audioFrames = 0;
	view = obs_view_create();
	if (!view) {
		stop();
		*error = "Could not create the replay capture view.";
		return false;
	}

	obs_view_set_source(view, 0, source);
	video = obs_view_add(view);
	if (!video || !video_output_connect(video, nullptr, onVideoFrame, this)) {
		stop();
		*error = "Could not connect the selected source to replay video capture.";
		return false;
	}
	videoConnected = true;

	obs_source_add_audio_capture_callback(source, onAudioFrame, this);
	audioConnected = true;
	return true;
}

void SourceCapture::stop()
{
	if (audioConnected && source)
		obs_source_remove_audio_capture_callback(source, onAudioFrame, this);
	if (videoConnected && video)
		video_output_disconnect(video, onVideoFrame, this);
	if (view) {
		obs_view_set_source(view, 0, nullptr);
		obs_view_remove(view);
		obs_view_destroy(view);
	}
	obs_source_release(source);
	source = nullptr;
	view = nullptr;
	video = nullptr;
	videoConnected = false;
	audioConnected = false;
}

bool SourceCapture::isActive() const
{
	return source && videoConnected;
}

video_t *SourceCapture::videoOutput() const
{
	return video;
}

uint64_t SourceCapture::timelineUs() const
{
	const uint64_t firstTimestamp = firstVideoTimestampNs.load();
	const uint64_t lastTimestamp = lastVideoTimestampNs.load();
	if (!firstTimestamp || lastTimestamp < firstTimestamp)
		return 0;
	return (lastTimestamp - firstTimestamp) / 1000;
}

uint64_t SourceCapture::capturedVideoFrames() const
{
	return videoFrames.load();
}

uint64_t SourceCapture::capturedAudioFrames() const
{
	return audioFrames.load();
}

void SourceCapture::onVideoFrame(void *param, struct video_data *frame)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !frame)
		return;
	uint64_t noTimestamp = 0;
	capture->firstVideoTimestampNs.compare_exchange_strong(noTimestamp, frame->timestamp);
	capture->lastVideoTimestampNs = frame->timestamp;
	++capture->videoFrames;
}

void SourceCapture::onAudioFrame(void *param, obs_source_t *, const struct audio_data *audioData, bool muted)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !audioData || muted)
		return;
	capture->audioFrames += audioData->frames;
}

} // namespace obs_replays
