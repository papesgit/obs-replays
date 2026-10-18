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

bool SourceCapture::start(obs_source_t *captureSource, obs_source_t *captureAudioSource, const char **error)
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
	audioSource = obs_source_get_ref(captureAudioSource ? captureAudioSource : captureSource);
	if (audioSource != source)
		obs_source_inc_active(audioSource);
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

	obs_source_add_audio_capture_callback(audioSource, onAudioFrame, this);
	audioConnected = true;
	blog(LOG_INFO, "[obs-replays] Capture started: video '%s', audio '%s' (%u Hz, %u channels).",
	     obs_source_get_name(source), obs_source_get_name(audioSource), audioSampleRate, audioChannels);
	return true;
}

void SourceCapture::stop()
{
	if (audioConnected && audioSource)
		obs_source_remove_audio_capture_callback(audioSource, onAudioFrame, this);
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
	if (audioSource && audioSource != source)
		obs_source_dec_active(audioSource);
	obs_source_release(audioSource);
	audioSource = nullptr;
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
	nextAudioTimestampNs = 0;
	lastAudioSyncOffsetNs = 0;
	audioStreamStarted = false;
	audioReadFraction = 0.0;
	audioRateRatio = 1.0;
	audioPhaseErrorFrames = 0.0;
	lastAudioArrivalNs = 0;
	audioUnderrunDiagnosticPending = false;
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

	capture->enqueueAudio(audioSource, audioData, muted, os_gettime_ns());
}

void SourceCapture::enqueueAudio(obs_source_t *audioSource, const struct audio_data *audioData, bool muted,
				 uint64_t nowNs)
{
	// Reject an oversized callback before allocating planes or evicting valid audio.
	if (audioData->frames > audioSampleRate * 2)
		return;
	AudioBlock block;
	block.frames = audioData->frames;
	block.planes.reserve(static_cast<qsizetype>(audioChannels));
	const size_t planeBytes = static_cast<size_t>(audioData->frames) * sizeof(float);
	for (uint32_t channel = 0; channel < audioChannels; ++channel) {
		QByteArray plane(static_cast<int>(planeBytes), '\0');
		if (!muted && audioData->data[channel])
			std::memcpy(plane.data(), audioData->data[channel], planeBytes);
		block.planes.append(std::move(plane));
	}

	QMutexLocker lock(audioMutex);
	const uint64_t previousArrivalNs = lastAudioArrivalNs;
	lastAudioArrivalNs = nowNs;
	constexpr uint64_t directTimestampRangeNs = 2000000000ULL;
	constexpr int64_t discontinuityThresholdNs = 250000000LL;
	const uint64_t rawTimestampNs = audioData->timestamp;
	const int64_t timestampDeltaNs =
		rawTimestampNs && nextRawAudioTimestampNs
			? static_cast<int64_t>(rawTimestampNs) - static_cast<int64_t>(nextRawAudioTimestampNs)
			: 0;
	bool clockReset = false;
	uint64_t sourceTimestampNs = rawTimestampNs;
	if (rawTimestampNs) {
		const uint64_t distance = rawTimestampNs > nowNs ? rawTimestampNs - nowNs : nowNs - rawTimestampNs;
		const int64_t mapped = static_cast<int64_t>(rawTimestampNs) + audioTimestampOffsetNs;
		const int64_t arrivalError = mapped - static_cast<int64_t>(nowNs);
		if (distance < directTimestampRangeNs) {
			// OBS-clock timestamps remain authoritative across dropouts. A forward
			// gap is missing sound, not a reason to move the source's clock.
			audioTimestampOffsetNs = 0;
		} else if (!audioClockInitialized || timestampDeltaNs < -discontinuityThresholdNs ||
			   ((timestampDeltaNs > discontinuityThresholdNs ||
			     (previousArrivalNs && nowNs - previousArrivalNs > directTimestampRangeNs)) &&
			    (arrivalError > static_cast<int64_t>(directTimestampRangeNs) ||
			     arrivalError < -static_cast<int64_t>(directTimestampRangeNs)))) {
			audioTimestampOffsetNs = static_cast<int64_t>(nowNs) - static_cast<int64_t>(rawTimestampNs);
			clockReset = audioClockInitialized;
		}
		audioClockInitialized = true;
	} else {
		// Without a source clock use arrival time, preserving gaps after a stall.
		// Short callback jitter can still use the expected contiguous timestamp.
		sourceTimestampNs = nowNs;
		const uint64_t expectedDistance = nextAudioTimestampNs > nowNs ? nextAudioTimestampNs - nowNs
									       : nowNs - nextAudioTimestampNs;
		const uint64_t jitterRangeNs = audio_frames_to_ns(audioSampleRate, audioData->frames) * 2;
		if (previousArrivalNs && nowNs - previousArrivalNs < jitterRangeNs && nextAudioTimestampNs &&
		    expectedDistance < jitterRangeNs)
			sourceTimestampNs = nextAudioTimestampNs;
	}

	int64_t mappedTimestampNs =
		static_cast<int64_t>(sourceTimestampNs) + (rawTimestampNs ? audioTimestampOffsetNs : 0);
	// Capture callbacks carry raw device timestamps, before OBS's own timestamp
	// smoothing. Small per-packet variations describe one continuous waveform;
	// only real gaps/overlaps should split it into independently placed runs.
	constexpr int64_t timestampJitterNs = 5000000LL;
	const int64_t continuityErrorNs = mappedTimestampNs - static_cast<int64_t>(nextAudioTimestampNs);
	const int64_t syncOffsetNs = audioSource ? obs_source_get_sync_offset(audioSource) : 0;
	block.discontinuity = !nextAudioTimestampNs || clockReset || continuityErrorNs > timestampJitterNs ||
			      continuityErrorNs < -timestampJitterNs || syncOffsetNs != lastAudioSyncOffsetNs;
	nextAudioTimestampNs = static_cast<uint64_t>(std::max<int64_t>(mappedTimestampNs, 0)) +
			       audio_frames_to_ns(audioSampleRate, audioData->frames);
	lastAudioSyncOffsetNs = syncOffsetNs;
	mappedTimestampNs += syncOffsetNs;
	block.timestampNs = mappedTimestampNs > 0 ? static_cast<uint64_t>(mappedTimestampNs) : nowNs;
	nextRawAudioTimestampNs =
		rawTimestampNs ? rawTimestampNs + audio_frames_to_ns(audioSampleRate, audioData->frames) : 0;
	if (clockReset) {
		audioBlocks->clear();
		queuedAudioFrames = 0;
		audioStreamStarted = false;
		blog(LOG_WARNING,
		     "[obs-replays] Source audio clock reset; timestamp mapping was renewed without changing the replay timeline.");
	}
	if (audioUnderrunDiagnosticPending) {
		const double arrivalIntervalMs =
			previousArrivalNs ? static_cast<double>(nowNs - previousArrivalNs) / 1000000.0 : 0.0;
		blog(LOG_INFO,
		     "[obs-replays] Replay capture audio input recovered (callback interval %.3f ms, source timestamp delta %.3f ms).",
		     arrivalIntervalMs, static_cast<double>(timestampDeltaNs) / 1000000.0);
		audioUnderrunDiagnosticPending = false;
	}

	const uint32_t maxBufferedAudioFrames = audioSampleRate * 2;
	bool trimmedQueue = false;
	while (queuedAudioFrames + block.frames > maxBufferedAudioFrames && !audioBlocks->isEmpty()) {
		const AudioBlock &oldest = audioBlocks->front();
		queuedAudioFrames -= oldest.frames - oldest.offset;
		audioBlocks->pop_front();
		trimmedQueue = true;
	}
	if (trimmedQueue)
		audioStreamStarted = false;
	if (trimmedQueue && nowNs - lastAudioDiagnosticNs >= 5000000000ULL) {
		blog(LOG_WARNING,
		     "[obs-replays] Replay capture audio exceeded its two-second safety buffer; stale audio was discarded.");
		lastAudioDiagnosticNs = nowNs;
	}
	if (queuedAudioFrames + block.frames > maxBufferedAudioFrames) {
		// A single malformed block must not defeat the queue bound.
		return;
	}
	queuedAudioFrames += block.frames;
	audioBlocks->append(std::move(block));
	audioFrames += audioData->frames;
}

bool SourceCapture::provideAudio(void *param, uint64_t startTs, uint64_t, uint64_t *newTs, uint32_t activeMixers,
				 struct audio_output_data *mixes)
{
	auto *capture = static_cast<SourceCapture *>(param);
	if (!capture || !capture->audioMutex || !capture->audioBlocks || !newTs || !mixes)
		return false;

	// A fixed look-behind absorbs callback jitter without changing A/V timing.
	// Source timestamps anchor each continuous run and control its read rate.
	// Derive every window from OBS's clock instead of accumulating rounded block
	// durations or restarting the output clock when the input disappears.
	constexpr uint64_t bufferingNs = 100000000ULL;
	*newTs = startTs > bufferingNs ? startTs - bufferingNs : 0;
	if (!activeMixers)
		return true;

	QMutexLocker lock(capture->audioMutex);
	uint32_t outputOffset = 0;
	uint32_t copied = 0;
	uint32_t discarded = 0;
	while (!capture->audioBlocks->isEmpty() && outputOffset < AUDIO_OUTPUT_FRAMES) {
		AudioBlock &head = capture->audioBlocks->front();
		const int64_t deltaNs = static_cast<int64_t>(head.timestampNs) - static_cast<int64_t>(*newTs);
		const double headPosition = static_cast<double>(deltaNs) * capture->audioSampleRate / 1000000000.0 +
					    head.offset + capture->audioReadFraction;
		const double phaseError = headPosition - outputOffset;
		const bool lostAlignment = std::abs(phaseError) > capture->audioSampleRate * 0.020;
		if (!capture->audioStreamStarted || head.discontinuity || lostAlignment) {
			capture->audioStreamStarted = false;
			capture->audioReadFraction = 0.0;
			capture->audioPhaseErrorFrames = 0.0;
			capture->audioRateRatio = 1.0;
			// Align run boundaries exactly, preserving real gaps and discarding
			// late audio. Within a run the samples stay contiguous.
			int64_t position = static_cast<int64_t>(std::llround(static_cast<double>(deltaNs) *
									     capture->audioSampleRate / 1000000000.0)) +
					   head.offset;
			if (position < outputOffset) {
				const uint32_t skip = static_cast<uint32_t>(
					std::min<int64_t>(outputOffset - position, head.frames - head.offset));
				head.offset += skip;
				capture->queuedAudioFrames -= skip;
				discarded += skip;
				position += skip;
			}
			if (head.offset == head.frames) {
				capture->audioBlocks->pop_front();
				continue;
			}
			if (position >= AUDIO_OUTPUT_FRAMES)
				break;
			outputOffset = static_cast<uint32_t>(position);
			head.discontinuity = false;
			capture->audioStreamStarted = true;
		} else {
			// Follow timestamp phase, not FIFO depth. Low-pass device timestamp
			// jitter and compensate slow clock drift by interpolation instead of
			// inserting silence or cutting samples at packet boundaries.
			capture->audioPhaseErrorFrames = capture->audioPhaseErrorFrames * 0.95 + phaseError * 0.05;
			capture->audioRateRatio = std::abs(capture->audioPhaseErrorFrames) < 1.0
							  ? 1.0
							  : std::clamp(1.0 - capture->audioPhaseErrorFrames /
										       (capture->audioSampleRate * 2.0),
								       0.995, 1.005);
		}

		const uint32_t remaining = AUDIO_OUTPUT_FRAMES - outputOffset;
		const uint32_t wanted = static_cast<uint32_t>(std::floor(capture->audioReadFraction +
									 static_cast<double>(remaining - 1) *
										 capture->audioRateRatio)) +
					2;
		uint32_t gathered = 0;
		for (const AudioBlock &block : std::as_const(*capture->audioBlocks)) {
			if (gathered && block.discontinuity)
				break;
			const uint32_t count = std::min(block.frames - block.offset, wanted - gathered);
			for (uint32_t channel = 0; channel < capture->audioChannels; ++channel)
				std::memcpy(capture->audioScratchPlanes[channel].data() + gathered * sizeof(float),
					    block.planes.at(channel).constData() + block.offset * sizeof(float),
					    count * sizeof(float));
			gathered += count;
			if (gathered == wanted)
				break;
		}
		uint32_t produced = 0;
		for (; produced < remaining; ++produced) {
			const double readPosition = capture->audioReadFraction + produced * capture->audioRateRatio;
			const uint32_t first = static_cast<uint32_t>(std::floor(readPosition));
			if (first >= gathered)
				break;
			const uint32_t second = std::min(first + 1, gathered - 1);
			const float fraction = static_cast<float>(readPosition - first);
			for (uint32_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
				if (!(activeMixers & (1u << mix)))
					continue;
				for (uint32_t channel = 0; channel < capture->audioChannels; ++channel) {
					if (!mixes[mix].data[channel])
						continue;
					const auto *samples = reinterpret_cast<const float *>(
						capture->audioScratchPlanes.at(channel).constData());
					mixes[mix].data[channel][outputOffset + produced] =
						samples[first] + (samples[second] - samples[first]) * fraction;
				}
			}
		}
		const double advanced = capture->audioReadFraction + produced * capture->audioRateRatio;
		uint32_t consumed = std::min(static_cast<uint32_t>(std::floor(advanced)), gathered);
		capture->audioReadFraction = advanced - consumed;
		while (consumed) {
			AudioBlock &block = capture->audioBlocks->front();
			const uint32_t count = std::min(consumed, block.frames - block.offset);
			block.offset += count;
			capture->queuedAudioFrames -= count;
			consumed -= count;
			if (block.offset == block.frames)
				capture->audioBlocks->pop_front();
		}
		outputOffset += produced;
		copied += produced;
		if (capture->audioBlocks->isEmpty() || capture->audioBlocks->front().discontinuity) {
			capture->audioStreamStarted = false;
			capture->audioReadFraction = 0.0;
		}
	}
	if (capture->audioBlocks->isEmpty())
		capture->audioStreamStarted = false;

	const uint64_t nowNs = os_gettime_ns();
	if (capture->audioFrames.load() && (copied < AUDIO_OUTPUT_FRAMES || discarded > AUDIO_OUTPUT_FRAMES) &&
	    nowNs - capture->lastAudioDiagnosticNs >= 5000000000ULL) {
		blog(LOG_WARNING,
		     "[obs-replays] Replay audio timestamp alignment: %u silent frames, %u late/overlapping frames discarded, %u queued frames; timeline preserved.",
		     AUDIO_OUTPUT_FRAMES - copied, discarded, capture->queuedAudioFrames);
		capture->lastAudioDiagnosticNs = nowNs;
		capture->audioUnderrunDiagnosticPending = copied < AUDIO_OUTPUT_FRAMES;
	}
	// OBS clears the mix buffers before this callback. Missing intervals remain
	// silence, including a source that never produces audio; output never stalls.
	return true;
}

} // namespace obs_replays
