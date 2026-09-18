#include "segment-writer.h"

#include <obs-module.h>
#include <QByteArray>
#include <QFileInfo>

namespace obs_replays {

SegmentWriter::~SegmentWriter()
{
	release();
}

bool SegmentWriter::start(video_t *video, audio_t *audio, const QString &path,
			  int videoBitrateMbps, int audioBitrateKbps, QString *error)
{
	if (output || !video || !audio || path.isEmpty()) {
		*error = "Cannot start the replay recording writer.";
		return false;
	}

	obs_data_t *encoderSettings = obs_data_create();
	obs_data_set_string(encoderSettings, "rate_control", "CBR");
	obs_data_set_int(encoderSettings, "bitrate", videoBitrateMbps * 1000);
	// A one-second GOP gives the fragmented MP4 muxer frequent independently
	// decodable fragment boundaries while retaining efficient normal playback.
	obs_data_set_int(encoderSettings, "keyint_sec", 1);
	obs_data_set_string(encoderSettings, "preset", "veryfast");
	videoEncoder = obs_video_encoder_create("obs_x264", "obs-replays-video", encoderSettings, nullptr);
	obs_data_release(encoderSettings);
	if (!videoEncoder) {
		*error = "OBS could not create the x264 replay video encoder.";
		return false;
	}
	obs_encoder_set_video(videoEncoder, video);

	obs_data_t *audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", audioBitrateKbps);
	audioEncoder = obs_audio_encoder_create("ffmpeg_aac", "obs-replays-audio", audioSettings, 0,
						nullptr);
	obs_data_release(audioSettings);
	if (!audioEncoder) {
		release();
		*error = "OBS could not create the AAC replay audio encoder.";
		return false;
	}
	obs_encoder_set_audio(audioEncoder, audio);

	output = obs_output_create("ffmpeg_muxer", "obs-replays-fmp4", nullptr, nullptr);
	if (!output) {
		release();
		*error = "OBS could not create the FFmpeg fragmented MP4 muxer output.";
		return false;
	}

	const QFileInfo initialFile(path);
	const QByteArray outputPath = initialFile.absoluteFilePath().toUtf8();
	obs_data_t *outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", outputPath.constData());
	obs_data_set_string(outputSettings, "muxer_settings",
			    "movflags=empty_moov+frag_keyframe+default_base_moof+skip_trailer "
			    "frag_duration=500000 flush_packets=1");
	obs_data_set_bool(outputSettings, "split_file", false);
	obs_data_set_bool(outputSettings, "allow_overwrite", true);
	obs_output_update(output, outputSettings);
	obs_data_release(outputSettings);
	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);
	signal_handler_t *signalHandler = obs_output_get_signal_handler(output);
	signal_handler_connect(signalHandler, "stop", onStopped, this);

	stopped = false;
	if (!obs_output_start(output)) {
		const char *lastError = obs_output_get_last_error(output);
		*error = lastError && *lastError ? QString::fromUtf8(lastError)
							 : QStringLiteral("OBS could not start the replay MP4 output.");
		release();
		return false;
	}
	return true;
}

void SegmentWriter::stop()
{
	if (output && obs_output_active(output))
		obs_output_stop(output);
}

void SegmentWriter::forceStop()
{
	if (output) {
		obs_output_force_stop(output);
		// ffmpeg_muxer normally finishes a forced stop on its next packet. If
		// interleaving never began there is no next packet, so explicitly detach
		// the encoders; output destruction will terminate the helper process.
		if (obs_output_active(output))
			obs_output_end_data_capture(output);
	}
}

void SegmentWriter::release()
{
	if (output) {
		signal_handler_t *signalHandler = obs_output_get_signal_handler(output);
		signal_handler_disconnect(signalHandler, "stop", onStopped, this);
		obs_output_release(output);
	}
	obs_encoder_release(videoEncoder);
	obs_encoder_release(audioEncoder);
	output = nullptr;
	videoEncoder = nullptr;
	audioEncoder = nullptr;
	stopped = false;
}

bool SegmentWriter::isActive() const
{
	return output && obs_output_active(output);
}

bool SegmentWriter::hasStopped() const
{
	return stopped.load();
}

void SegmentWriter::onStopped(void *param, calldata_t *)
{
	auto *writer = static_cast<SegmentWriter *>(param);
	if (writer)
		writer->stopped = true;
}

} // namespace obs_replays
