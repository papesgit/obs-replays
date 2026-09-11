#include "segment-writer.h"

#include <obs-module.h>
#include <callback/calldata.h>
#include <callback/proc.h>

#include <QByteArray>
#include <QFileInfo>
#include <QMutexLocker>

namespace obs_replays {

SegmentWriter::~SegmentWriter()
{
	release();
}

bool SegmentWriter::start(video_t *video, audio_t *audio, const QString &path,
			  int videoBitrateMbps, int audioBitrateKbps, int segmentDurationSeconds,
			  QString *error)
{
	if (output || !video || !audio || path.isEmpty()) {
		*error = "Cannot start the replay segment writer.";
		return false;
	}

	obs_data_t *encoderSettings = obs_data_create();
	obs_data_set_string(encoderSettings, "rate_control", "CBR");
	obs_data_set_int(encoderSettings, "bitrate", videoBitrateMbps * 1000);
	obs_data_set_int(encoderSettings, "keyint_sec", 2);
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

	output = obs_output_create("ffmpeg_muxer", "obs-replays-mkv", nullptr, nullptr);
	if (!output) {
		release();
		*error = "OBS could not create the FFmpeg MKV muxer output.";
		return false;
	}

	const QFileInfo initialFile(path);
	const QByteArray outputPath = initialFile.absoluteFilePath().toUtf8();
	const QByteArray outputDirectory = initialFile.absolutePath().toUtf8();
	const QByteArray outputExtension = initialFile.suffix().isEmpty()
					     ? QByteArray("mkv")
					     : initialFile.suffix().toUtf8();
	obs_data_t *outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", outputPath.constData());
	obs_data_set_string(outputSettings, "directory", outputDirectory.constData());
	obs_data_set_string(outputSettings, "format", "replay-%CCYY-%MM-%DD-%hh-%mm-%ss");
	obs_data_set_string(outputSettings, "extension", outputExtension.constData());
	obs_data_set_bool(outputSettings, "allow_spaces", false);
	obs_data_set_string(outputSettings, "muxer_settings", "");
	obs_data_set_bool(outputSettings, "split_file", true);
	obs_data_set_int(outputSettings, "max_time_sec", segmentDurationSeconds);
	obs_data_set_bool(outputSettings, "allow_overwrite", false);
	obs_output_update(output, outputSettings);
	obs_data_release(outputSettings);
	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);
	signal_handler_t *signalHandler = obs_output_get_signal_handler(output);
	signal_handler_connect(signalHandler, "file_changed", onFileChanged, this);
	signal_handler_connect(signalHandler, "stop", onStopped, this);

	stopped = false;
	if (!obs_output_start(output)) {
		const char *lastError = obs_output_get_last_error(output);
		*error = lastError && *lastError ? QString::fromUtf8(lastError)
							 : QStringLiteral("OBS could not start the replay MKV output.");
		release();
		return false;
	}
	{
		QMutexLocker lock(&mutex);
		latestPath = initialFile.absoluteFilePath();
	}
	return true;
}

void SegmentWriter::stop()
{
	if (output && obs_output_active(output))
		obs_output_stop(output);
}

void SegmentWriter::release()
{
	if (output) {
		signal_handler_t *signalHandler = obs_output_get_signal_handler(output);
		signal_handler_disconnect(signalHandler, "file_changed", onFileChanged, this);
		signal_handler_disconnect(signalHandler, "stop", onStopped, this);
		obs_output_release(output);
	}
	obs_encoder_release(videoEncoder);
	obs_encoder_release(audioEncoder);
	output = nullptr;
	videoEncoder = nullptr;
	audioEncoder = nullptr;
	stopped = false;
	QMutexLocker lock(&mutex);
	latestPath.clear();
}

bool SegmentWriter::isActive() const
{
	return output && obs_output_active(output);
}

bool SegmentWriter::hasStopped() const
{
	return stopped.load();
}

bool SegmentWriter::requestSplit(QString *error)
{
	if (!output || !obs_output_active(output)) {
		*error = "Replay recording is not active.";
		return false;
	}

	calldata_t parameters;
	calldata_init(&parameters);
	const bool called = proc_handler_call(obs_output_get_proc_handler(output), "split_file",
				      &parameters);
	bool split_enabled = false;
	calldata_get_bool(&parameters, "split_file_enabled", &split_enabled);
	calldata_free(&parameters);
	if (!called || !split_enabled) {
		*error = "OBS could not request a new replay segment.";
		return false;
	}
	return true;
}

QString SegmentWriter::latestSegmentPath() const
{
	QMutexLocker lock(&mutex);
	return latestPath;
}

void SegmentWriter::onFileChanged(void *param, calldata_t *data)
{
	auto *writer = static_cast<SegmentWriter *>(param);
	const char *path = nullptr;
	if (!writer || !calldata_get_string(data, "next_file", &path) || !path)
		return;
	QMutexLocker lock(&writer->mutex);
	writer->latestPath = QString::fromUtf8(path);
}

void SegmentWriter::onStopped(void *param, calldata_t *)
{
	auto *writer = static_cast<SegmentWriter *>(param);
	if (writer)
		writer->stopped = true;
}

} // namespace obs_replays
