#include "source-capture.h"

#include <obs-module.h>

#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
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
	audioScratchPlanes.reserve(static_cast<qsizetype>(audioChannels));
	constexpr uint32_t scratchFrames = AUDIO_OUTPUT_FRAMES + AUDIO_OUTPUT_FRAMES / 100 + 4;
	for (uint32_t channel = 0; channel < audioChannels; ++channel)
		audioScratchPlanes.append(QByteArray(static_cast<int>(scratchFrames * sizeof(float)), '\0'));
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
	audioScratchPlanes.clear();
	audioSampleRate = 0;
	audioChannels = 0;
	queuedAudioFrames = 0;
	audioClockInitialized = false;
	audioTimestampOffsetNs = 0;
	nextRawAudioTimestampNs = 0;
	audioOutputStarted = false;
	audioPrimingStartedNs = 0;
	audioOutputTimestampNs = 0;
	lastAudioArrivalNs = 0;
	audioUnderrunDiagnosticPending = false;
	audioTargetQueueFrames = 0;
	audioReadFraction = 0.0;
	audioQueueErrorFrames = 0.0;
	audioRateRatio = 1.0;
	lastAudioDiagnosticNs = 0;
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

void SourceCapture::onAudioFrame(void *param, obs_source_t *audioSource, const struct audio_data *audioData, bool muted)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !audioData || !capture->audioMutex || !capture->audioBlocks || audioData->frames == 0)
		return;

	AudioBlock block;
	block.frames = audioData->frames;
	block.planes.reserve(static_cast<qsizetype>(capture->audioChannels));
	const size_t planeBytes = static_cast<size_t>(audioData->frames) * sizeof(float);
	for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
		QByteArray plane(static_cast<int>(planeBytes), '\0');
		if (!muted && audioData->data[channel])
			std::memcpy(plane.data(), audioData->data[channel], planeBytes);
		block.planes.append(std::move(plane));
	}

	const uint64_t nowNs = os_gettime_ns();
	QMutexLocker lock(capture->audioMutex);
	const uint64_t previousArrivalNs = capture->lastAudioArrivalNs;
	capture->lastAudioArrivalNs = nowNs;
	constexpr uint64_t directTimestampRangeNs = 2000000000ULL;
	constexpr uint64_t discontinuityThresholdNs = 250000000ULL;
	const uint64_t rawTimestampNs = audioData->timestamp;
	bool discontinuity = false;
	int64_t timestampDeltaNs = 0;
	if (!capture->audioClockInitialized) {
		const uint64_t distance = rawTimestampNs > nowNs ? rawTimestampNs - nowNs : nowNs - rawTimestampNs;
		capture->audioTimestampOffsetNs =
			rawTimestampNs && distance < directTimestampRangeNs
				? 0
				: static_cast<int64_t>(nowNs) - static_cast<int64_t>(rawTimestampNs);
		capture->audioClockInitialized = true;
	} else if (rawTimestampNs && capture->nextRawAudioTimestampNs) {
		timestampDeltaNs =
			static_cast<int64_t>(rawTimestampNs) - static_cast<int64_t>(capture->nextRawAudioTimestampNs);
		const uint64_t difference = rawTimestampNs > capture->nextRawAudioTimestampNs
						    ? rawTimestampNs - capture->nextRawAudioTimestampNs
						    : capture->nextRawAudioTimestampNs - rawTimestampNs;
		discontinuity = difference > discontinuityThresholdNs;
		if (discontinuity) {
			capture->audioTimestampOffsetNs =
				static_cast<int64_t>(nowNs) - static_cast<int64_t>(rawTimestampNs);
			capture->audioBlocks->clear();
			capture->queuedAudioFrames = 0;
			capture->audioOutputStarted = false;
			capture->audioPrimingStartedNs = 0;
			capture->audioOutputTimestampNs = 0;
			capture->audioTargetQueueFrames = 0;
			capture->audioReadFraction = 0.0;
			capture->audioQueueErrorFrames = 0.0;
			capture->audioRateRatio = 1.0;
		}
	}

	const uint64_t sourceTimestampNs = rawTimestampNs ? rawTimestampNs : capture->nextRawAudioTimestampNs;
	int64_t mappedTimestampNs = static_cast<int64_t>(sourceTimestampNs) + capture->audioTimestampOffsetNs;
	if (audioSource)
		mappedTimestampNs += obs_source_get_sync_offset(audioSource);
	block.timestampNs = mappedTimestampNs > 0 ? static_cast<uint64_t>(mappedTimestampNs) : nowNs;
	capture->nextRawAudioTimestampNs =
		sourceTimestampNs + audio_frames_to_ns(capture->audioSampleRate, audioData->frames);

	if (discontinuity) {
		blog(LOG_WARNING,
		     "[obs-replays] Source audio timestamp jumped; replay capture audio was resynchronized.");
	}
	if (capture->audioUnderrunDiagnosticPending) {
		const double arrivalIntervalMs =
			previousArrivalNs ? static_cast<double>(nowNs - previousArrivalNs) / 1000000.0 : 0.0;
		blog(LOG_INFO,
		     "[obs-replays] Replay capture audio input recovered (callback interval %.3f ms, source timestamp delta %.3f ms).",
		     arrivalIntervalMs, static_cast<double>(timestampDeltaNs) / 1000000.0);
		capture->audioUnderrunDiagnosticPending = false;
	}

	const uint32_t maxBufferedAudioFrames = capture->audioSampleRate * 2;
	bool trimmedQueue = false;
	while (capture->queuedAudioFrames + block.frames > maxBufferedAudioFrames && !capture->audioBlocks->isEmpty()) {
		const AudioBlock &oldest = capture->audioBlocks->front();
		capture->queuedAudioFrames -= oldest.frames - oldest.offset;
		capture->audioBlocks->pop_front();
		trimmedQueue = true;
	}
	if (trimmedQueue) {
		capture->audioOutputStarted = false;
		capture->audioPrimingStartedNs = 0;
		capture->audioOutputTimestampNs = 0;
		capture->audioTargetQueueFrames = 0;
		capture->audioReadFraction = 0.0;
		capture->audioQueueErrorFrames = 0.0;
		capture->audioRateRatio = 1.0;
	}
	if (trimmedQueue && nowNs - capture->lastAudioDiagnosticNs >= 5000000000ULL) {
		blog(LOG_WARNING,
		     "[obs-replays] Replay capture audio exceeded its two-second safety buffer; stale audio was discarded.");
		capture->lastAudioDiagnosticNs = nowNs;
	}
	if (capture->queuedAudioFrames + block.frames > maxBufferedAudioFrames) {
		// A single malformed block must not defeat the queue bound.
		return;
	}
	capture->queuedAudioFrames += block.frames;
	capture->audioBlocks->append(std::move(block));
	capture->audioFrames += audioData->frames;
}

bool SourceCapture::provideAudio(void *param, uint64_t startTs, uint64_t, uint64_t *newTs, uint32_t activeMixers,
				 struct audio_output_data *mixes)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !capture->audioMutex || !capture->audioBlocks || !newTs || !mixes)
		return false;

	constexpr uint32_t outputFrames = AUDIO_OUTPUT_FRAMES;
	const uint64_t nowNs = os_gettime_ns();
	QMutexLocker lock(capture->audioMutex);
	*newTs = startTs;
	if (activeMixers == 0)
		return true;
	constexpr uint32_t targetQueueFrames = AUDIO_OUTPUT_FRAMES * 4;
	constexpr uint64_t maximumPrimingNs = 250000000ULL;
	if (!capture->audioOutputStarted) {
		if (!capture->audioPrimingStartedNs)
			capture->audioPrimingStartedNs = nowNs;

		const bool adequatelyPrimed = capture->queuedAudioFrames >= targetQueueFrames + outputFrames;
		const bool primingTimedOut = nowNs - capture->audioPrimingStartedNs >= maximumPrimingNs;
		if (!adequatelyPrimed && !primingTimedOut)
			return false;

		if (!capture->audioBlocks->isEmpty()) {
			const AudioBlock &first = capture->audioBlocks->front();
			capture->audioOutputTimestampNs =
				first.timestampNs + audio_frames_to_ns(capture->audioSampleRate, first.offset);
			const uint32_t availableReserve = capture->queuedAudioFrames > outputFrames
								  ? capture->queuedAudioFrames - outputFrames
								  : 0;
			capture->audioTargetQueueFrames = std::min(targetQueueFrames, availableReserve);
			capture->audioOutputStarted = true;
		} else {
			// Do not prevent a silent source from starting or stopping an OBS
			// output forever. Keep submitting silence after the bounded wait.
			return true;
		}
	}

	*newTs = capture->audioOutputTimestampNs;
	auto reportUnderrun = [&](uint32_t produced, uint32_t requested, uint32_t queuedBeforeRead) {
		if (nowNs - capture->lastAudioDiagnosticNs < 5000000000ULL)
			return;
		const double callbackAgeMs =
			capture->lastAudioArrivalNs
				? static_cast<double>(nowNs - capture->lastAudioArrivalNs) / 1000000.0
				: 0.0;
		blog(LOG_WARNING,
		     "[obs-replays] Replay capture audio input queue underrun (%u of %u frames available; %u queued before read, %.4f%% rate correction, %.3f ms since source callback).",
		     produced, requested, queuedBeforeRead, (capture->audioRateRatio - 1.0) * 100.0, callbackAgeMs);
		capture->lastAudioDiagnosticNs = nowNs;
		capture->audioUnderrunDiagnosticPending = true;
	};
	// Keep this private output on a monotonic clock and always submit its
	// pre-cleared buffers once priming has completed.
	if (capture->audioBlocks->isEmpty()) {
		if (capture->audioOutputStarted)
			reportUnderrun(0, outputFrames, 0);
		capture->audioOutputTimestampNs += audio_frames_to_ns(capture->audioSampleRate, outputFrames);
		return true;
	}

	uint32_t outputOffset = 0;
	const uint32_t framesToProduce = outputFrames - outputOffset;
	if (framesToProduce == 0)
		return true;

	// Keep a small, stable FIFO behind the OBS audio clock. The correction is
	// spread continuously across samples (at most 0.5%) instead of being made
	// as audible whole-sample gaps or overlaps.
	const uint32_t queuedBeforeRead = capture->queuedAudioFrames;
	const double projectedQueue =
		static_cast<double>(capture->queuedAudioFrames) - static_cast<double>(framesToProduce);
	const double queueError = projectedQueue - static_cast<double>(capture->audioTargetQueueFrames);
	capture->audioQueueErrorFrames = capture->audioQueueErrorFrames * 0.995 + queueError * 0.005;
	const double desiredRatio =
		std::clamp(1.0 + capture->audioQueueErrorFrames / (static_cast<double>(capture->audioSampleRate) * 2.0),
			   0.995, 1.005);
	capture->audioRateRatio = capture->audioRateRatio * 0.99 + desiredRatio * 0.01;

	const double finalReadPosition =
		capture->audioReadFraction + static_cast<double>(framesToProduce - 1) * capture->audioRateRatio;
	const uint32_t wantedInputFrames = static_cast<uint32_t>(std::floor(finalReadPosition)) + 2;
	const uint32_t gatheredFrames = std::min(wantedInputFrames, capture->queuedAudioFrames);
	if (gatheredFrames == 0)
		return true;

	uint32_t gathered = 0;
	for (const AudioBlock &block : std::as_const(*capture->audioBlocks)) {
		const uint32_t available = block.frames - block.offset;
		const uint32_t count = std::min(available, gatheredFrames - gathered);
		for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
			const QByteArray &sourcePlane = block.planes.at(static_cast<qsizetype>(channel));
			const auto *sourceSamples =
				reinterpret_cast<const float *>(sourcePlane.constData()) + block.offset;
			auto *inputSamples = reinterpret_cast<float *>(
				capture->audioScratchPlanes[static_cast<qsizetype>(channel)].data());
			std::memcpy(inputSamples + gathered, sourceSamples, static_cast<size_t>(count) * sizeof(float));
		}
		gathered += count;
		if (gathered == gatheredFrames)
			break;
	}

	uint32_t produced = 0;
	for (; produced < framesToProduce; ++produced) {
		const double position =
			capture->audioReadFraction + static_cast<double>(produced) * capture->audioRateRatio;
		const uint32_t first = static_cast<uint32_t>(std::floor(position));
		if (first >= gatheredFrames)
			break;
		const uint32_t second = std::min(first + 1, gatheredFrames - 1);
		const float fraction = static_cast<float>(position - static_cast<double>(first));
		for (uint32_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
			if ((activeMixers & (1u << mix)) == 0)
				continue;
			for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
				float *destination = mixes[mix].data[channel];
				if (!destination)
					continue;
				const auto *input = reinterpret_cast<const float *>(
					capture->audioScratchPlanes.at(static_cast<qsizetype>(channel)).constData());
				destination[outputOffset + produced] =
					input[first] + (input[second] - input[first]) * fraction;
			}
		}
	}

	const double advancedPosition =
		capture->audioReadFraction + static_cast<double>(produced) * capture->audioRateRatio;
	uint32_t consumedFrames =
		std::min(static_cast<uint32_t>(std::floor(advancedPosition)), capture->queuedAudioFrames);
	capture->audioReadFraction = advancedPosition - static_cast<double>(consumedFrames);
	while (consumedFrames && !capture->audioBlocks->isEmpty()) {
		AudioBlock &block = capture->audioBlocks->front();
		const uint32_t available = block.frames - block.offset;
		const uint32_t count = std::min(available, consumedFrames);
		block.offset += count;
		capture->queuedAudioFrames -= count;
		consumedFrames -= count;
		if (block.offset == block.frames)
			capture->audioBlocks->pop_front();
	}
	if (capture->audioBlocks->isEmpty())
		capture->audioReadFraction = 0.0;
	if (produced < framesToProduce) {
		reportUnderrun(produced, framesToProduce, queuedBeforeRead);
	} else if (std::abs(capture->audioRateRatio - 1.0) >= 0.0045 &&
		   nowNs - capture->lastAudioDiagnosticNs >= 30000000000ULL) {
		blog(LOG_WARNING,
		     "[obs-replays] Replay capture audio clock correction is near its limit (%.3f%%, %u queued frames).",
		     (capture->audioRateRatio - 1.0) * 100.0, capture->queuedAudioFrames);
		capture->lastAudioDiagnosticNs = nowNs;
	}
	capture->audioOutputTimestampNs += audio_frames_to_ns(capture->audioSampleRate, outputFrames);
	return true;
}

} // namespace obs_replays
