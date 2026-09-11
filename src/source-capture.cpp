#include "source-capture.h"

#include <obs-module.h>

#include <media-io/audio-io.h>
#include <media-io/video-io.h>

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>
#include <utility>

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
	// A replay capture owns an active reference while it is recording. This
	// keeps source-provided audio alive even when the source is not visible in
	// a Program/Preview scene.
	obs_source_inc_active(source);
	sourceActive = true;
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

	obs_audio_info audioInfo = {};
	if (!obs_get_audio_info(&audioInfo)) {
		stop();
		*error = "Could not read OBS audio settings for replay capture.";
		return false;
	}
	audioSampleRate = audioInfo.samples_per_sec;
	audioChannels = static_cast<uint32_t>(get_audio_channels(audioInfo.speakers));
	audioMutex = new QMutex();
	audioBlocks = new QList<AudioBlock>();
	struct audio_output_info outputInfo = {};
	outputInfo.name = "obs-replays-source-audio";
	outputInfo.samples_per_sec = audioSampleRate;
	outputInfo.format = AUDIO_FORMAT_FLOAT_PLANAR;
	outputInfo.speakers = audioInfo.speakers;
	outputInfo.input_callback = provideAudio;
	outputInfo.input_param = this;
	if (audio_output_open(&audio, &outputInfo) != AUDIO_OUTPUT_SUCCESS) {
		stop();
		*error = "Could not create the replay source audio output.";
		return false;
	}

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
	if (audio)
		audio_output_close(audio);
	if (sourceActive && source)
		obs_source_dec_active(source);
	delete audioBlocks;
	delete audioMutex;
	obs_source_release(source);
	source = nullptr;
	view = nullptr;
	video = nullptr;
	audio = nullptr;
	audioBlocks = nullptr;
	audioMutex = nullptr;
	audioSampleRate = 0;
	audioChannels = 0;
	queuedAudioFrames = 0;
	videoConnected = false;
	audioConnected = false;
	sourceActive = false;
}

bool SourceCapture::isActive() const
{
	return source && videoConnected;
}

video_t *SourceCapture::videoOutput() const
{
	return video;
}

audio_t *SourceCapture::audioOutput() const
{
	return audio;
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
	if (!capture || !audioData || !capture->audioMutex || !capture->audioBlocks ||
	    audioData->frames == 0)
		return;

	AudioBlock block;
	block.timestampNs = audioData->timestamp;
	block.frames = audioData->frames;
	block.planes.reserve(static_cast<qsizetype>(capture->audioChannels));
	const size_t planeBytes = static_cast<size_t>(audioData->frames) * sizeof(float);
	for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
		QByteArray plane(static_cast<int>(planeBytes), '\0');
		if (!muted && audioData->data[channel])
			std::memcpy(plane.data(), audioData->data[channel], planeBytes);
		block.planes.append(std::move(plane));
	}

	QMutexLocker lock(capture->audioMutex);
	constexpr uint32_t maxBufferedAudioFrames = 96000;
	while (capture->queuedAudioFrames + block.frames > maxBufferedAudioFrames &&
	       !capture->audioBlocks->isEmpty()) {
		const AudioBlock &oldest = capture->audioBlocks->front();
		capture->queuedAudioFrames -= oldest.frames - oldest.offset;
		capture->audioBlocks->pop_front();
	}
	capture->queuedAudioFrames += block.frames;
	capture->audioBlocks->append(std::move(block));
	capture->audioFrames += audioData->frames;
}

bool SourceCapture::provideAudio(void *param, uint64_t startTs, uint64_t, uint64_t *newTs,
				 uint32_t activeMixers, struct audio_output_data *mixes)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !capture->audioMutex || !capture->audioBlocks || !newTs || !mixes)
		return false;

	constexpr uint32_t outputFrames = AUDIO_OUTPUT_FRAMES;
	QMutexLocker lock(capture->audioMutex);
	*newTs = startTs;
	if (activeMixers == 0)
		return true;
	if (capture->audioBlocks->isEmpty())
		return true;

	uint32_t written = 0;
	while (written < outputFrames && !capture->audioBlocks->isEmpty()) {
		AudioBlock &block = capture->audioBlocks->front();
		const uint32_t available = block.frames - block.offset;
		const uint32_t count = std::min(available, outputFrames - written);
		for (uint32_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
			if ((activeMixers & (1u << mix)) == 0)
				continue;
			for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
				float *destination = mixes[mix].data[channel];
				if (!destination)
					continue;
				const QByteArray &plane = block.planes.at(static_cast<qsizetype>(channel));
				const auto *source = reinterpret_cast<const float *>(plane.constData()) + block.offset;
				std::memcpy(destination + written, source, static_cast<size_t>(count) * sizeof(float));
			}
		}
		written += count;
		block.offset += count;
		capture->queuedAudioFrames -= count;
		if (block.offset == block.frames)
			capture->audioBlocks->pop_front();
	}
	return true;
}

} // namespace obs_replays
