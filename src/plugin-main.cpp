/*
OBS Replays

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "replay-session.h"
#include "replay-channel-source.h"
#include "segment-writer.h"
#include "source-capture.h"

#include <QByteArray>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QVector>

#include <memory>
#include <cstdint>
#include <algorithm>

namespace {

constexpr const char *dock_id = "obs-replays.dock";
constexpr const char *dock_title = "Replays";
constexpr const char *settings_filename = "settings.json";
constexpr const char *scene_collections_key = "scene_collections";
constexpr const char *playback_source_name = "OBS Replays Channel A";
constexpr const char *playback_source_id = "obs_replays_channel_a";

QWidget *replay_dock = nullptr;
QComboBox *source_selector = nullptr;
QLineEdit *replay_folder_selector = nullptr;
QSpinBox *video_bitrate_selector = nullptr;
QSpinBox *audio_bitrate_selector = nullptr;
QComboBox *replay_scene_selector = nullptr;
QComboBox *intro_transition_selector = nullptr;
QComboBox *outro_transition_selector = nullptr;
QSpinBox *pre_roll_seconds_selector = nullptr;
QSpinBox *post_roll_seconds_selector = nullptr;
QLabel *settings_status = nullptr;
QLabel *playout_status = nullptr;
QLabel *recording_status = nullptr;
QLabel *storage_status = nullptr;
QPushButton *start_recording_button = nullptr;
QPushButton *stop_recording_button = nullptr;
QPushButton *mark_event_button = nullptr;
QPushButton *play_events_button = nullptr;
QListWidget *events_list = nullptr;
QTimer *recording_timer = nullptr;
std::unique_ptr<obs_replays::ReplaySession> replay_session;
std::unique_ptr<obs_replays::SourceCapture> source_capture;
std::unique_ptr<obs_replays::SegmentWriter> segment_writer;
QString active_segment_path;

struct PendingReplayEvent {
	obs_replays::TimelineUs inUs = 0;
	obs_replays::TimelineUs outUs = 0;
	QString label;
};

QVector<PendingReplayEvent> pending_events;
obs_source_t *previous_program_scene = nullptr;
obs_source_t *previous_transition = nullptr;
obs_source_t *active_replay_scene = nullptr;
obs_source_t *active_replay_playback_source = nullptr;
int previous_transition_duration = 0;
bool replay_scene_change_in_progress = false;
QTimer *event_playout_timer = nullptr;

struct ReplayPlayoutItem {
	QString segmentPath;
	qint64 inMilliseconds = 0;
	qint64 durationMilliseconds = 0;
	QString label;
};

QVector<ReplayPlayoutItem> replay_playout_queue;
int replay_playout_index = -1;
qint64 pending_seek_milliseconds = -1;
bool pending_live_playout = false;
bool awaiting_event_start = false;
uint64_t playback_generation = 0;
qint64 intro_transition_point_milliseconds = 0;
qint64 first_event_intro_lead_milliseconds = 0;
bool outro_cleanup_pending = false;
obs_source_t *outro_transition_wait_source = nullptr;
uint64_t outro_transition_wait_generation = 0;

void media_started_callback(void *, calldata_t *);
void play_selected_replay_events();
void outro_transition_stop_callback(void *, calldata_t *);

void update_playout_button()
{
	if (!play_events_button)
		return;
	if (outro_cleanup_pending) {
		play_events_button->setText("Ending replay…");
		play_events_button->setEnabled(false);
	} else if (active_replay_scene) {
		play_events_button->setText("Stop event playback");
		play_events_button->setEnabled(true);
	} else {
		play_events_button->setText("Play selected events");
		play_events_button->setEnabled(true);
	}
}

qint64 stinger_transition_point_milliseconds(obs_source_t *transition)
{
	if (!transition || QString::fromUtf8(obs_source_get_id(transition)) != "obs_stinger_transition")
		return 0;

	obs_data_t *settings = obs_source_get_settings(transition);
	if (!settings)
		return 0;
	const qint64 point = std::max<qint64>(0, obs_data_get_int(settings, "transition_point"));
	const bool point_is_frame = obs_data_get_int(settings, "tp_type") == 1;
	obs_data_release(settings);
	if (!point_is_frame)
		return point;

	obs_video_info video_info = {};
	if (!obs_get_video_info(&video_info) || video_info.fps_num == 0)
		return 0;
	return (point * 1000LL * video_info.fps_den) / video_info.fps_num;
}

bool add_source_to_selector(void *, obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	if (name && *name)
		source_selector->addItem(QString::fromUtf8(name));
	return true;
}

void refresh_source_selector()
{
	if (!source_selector)
		return;

	const QString selected_source = source_selector->currentText();
	source_selector->clear();
	obs_enum_sources(add_source_to_selector, nullptr);
	const int selected_index = source_selector->findText(selected_source);
	if (selected_index >= 0)
		source_selector->setCurrentIndex(selected_index);
}

void populate_frontend_source_selector(QComboBox *selector,
				      void (*populate)(obs_frontend_source_list *))
{
	const QSignalBlocker blocker(selector);
	const QString selected = selector->currentText();
	selector->clear();

	obs_frontend_source_list sources = {};
	populate(&sources);
	for (size_t i = 0; i < sources.sources.num; ++i) {
		const char *name = obs_source_get_name(sources.sources.array[i]);
		if (name && *name)
			selector->addItem(QString::fromUtf8(name));
	}
	obs_frontend_source_list_free(&sources);

	const int selected_index = selector->findText(selected);
	if (selected_index >= 0)
		selector->setCurrentIndex(selected_index);
}

void refresh_playout_selectors()
{
	if (!replay_scene_selector)
		return;

	populate_frontend_source_selector(replay_scene_selector, obs_frontend_get_scenes);
	populate_frontend_source_selector(intro_transition_selector,
					 obs_frontend_get_transitions);
	populate_frontend_source_selector(outro_transition_selector,
					 obs_frontend_get_transitions);
}

QString format_duration(qint64 seconds)
{
	const qint64 days = seconds / 86400;
	seconds %= 86400;
	const qint64 hours = seconds / 3600;
	seconds %= 3600;
	const qint64 minutes = seconds / 60;
	if (days > 0)
		return QString("%1 d %2 h").arg(days).arg(hours);
	if (hours > 0)
		return QString("%1 h %2 min").arg(hours).arg(minutes);
	return QString("%1 min").arg(minutes);
}

QString format_timeline(obs_replays::TimelineUs timestamp_us)
{
	const qint64 total_milliseconds = timestamp_us / 1000;
	const qint64 hours = total_milliseconds / (60 * 60 * 1000);
	const qint64 minutes = (total_milliseconds / (60 * 1000)) % 60;
	const qint64 seconds = (total_milliseconds / 1000) % 60;
	const qint64 milliseconds = total_milliseconds % 1000;
	return QString("%1:%2:%3.%4")
		.arg(hours, 2, 10, QLatin1Char('0'))
		.arg(minutes, 2, 10, QLatin1Char('0'))
		.arg(seconds, 2, 10, QLatin1Char('0'))
		.arg(milliseconds, 3, 10, QLatin1Char('0'));
}

void update_storage_status()
{
	if (!storage_status || !replay_folder_selector ||
	    replay_folder_selector->text().isEmpty()) {
		if (storage_status)
			storage_status->setText("Choose a replay folder to estimate recording time.");
		return;
	}

	const QStorageInfo storage(replay_folder_selector->text());
	if (!storage.isValid() || !storage.isReady()) {
		storage_status->setText("The selected replay folder is not currently available.");
		return;
	}

	constexpr qint64 safety_reserve = 5LL * 1024 * 1024 * 1024;
	const qint64 free_bytes = storage.bytesAvailable();
	const qint64 recordable_bytes = qMax<qint64>(0, free_bytes - safety_reserve);
	const double bits_per_second = video_bitrate_selector->value() * 1000.0 * 1000.0 +
				       audio_bitrate_selector->value() * 1000.0;
	const qint64 recordable_seconds =
		bits_per_second > 0.0 ? (qint64)((recordable_bytes * 8.0) / bits_per_second) : 0;
	const double free_gib = free_bytes / (1024.0 * 1024.0 * 1024.0);
	const QString message =
		QString("Available: %1 GiB. Estimated recording time: %2 (5 GiB safety reserve, %3 Mb/s video + %4 kb/s audio).")
			.arg(QString::number(free_gib, 'f', 1), format_duration(recordable_seconds))
			.arg(video_bitrate_selector->value())
			.arg(audio_bitrate_selector->value());
	storage_status->setText(message);
}

obs_data_t *get_scene_collection_settings(obs_data_t *settings, bool create)
{
	char *collection_name = obs_frontend_get_current_scene_collection();
	if (!collection_name)
		return nullptr;

	obs_data_t *collections = obs_data_get_obj(settings, scene_collections_key);
	if (!collections && create) {
		collections = obs_data_create();
		obs_data_set_obj(settings, scene_collections_key, collections);
	}

	obs_data_t *collection_settings = nullptr;
	if (collections) {
		collection_settings = obs_data_get_obj(collections, collection_name);
		if (!collection_settings && create) {
			collection_settings = obs_data_create();
			obs_data_set_obj(collections, collection_name, collection_settings);
		}
		obs_data_release(collections);
	}
	bfree(collection_name);
	return collection_settings;
}

void load_settings()
{
	char *path = obs_module_config_path(settings_filename);
	if (!path)
		return;

	obs_data_t *settings = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!settings)
		return;

	obs_data_t *collection_settings = get_scene_collection_settings(settings, false);
	obs_data_release(settings);
	if (!collection_settings) {
		settings_status->setText("No saved settings for this scene collection.");
		return;
	}

	const QSignalBlocker source_blocker(source_selector);
	const QSignalBlocker replay_folder_blocker(replay_folder_selector);
	const QSignalBlocker video_bitrate_blocker(video_bitrate_selector);
	const QSignalBlocker audio_bitrate_blocker(audio_bitrate_selector);
	const QSignalBlocker scene_blocker(replay_scene_selector);
	const QSignalBlocker intro_blocker(intro_transition_selector);
	const QSignalBlocker outro_blocker(outro_transition_selector);

	auto select_saved_value = [](QComboBox *selector, const char *value) {
		const int index = selector->findText(QString::fromUtf8(value));
		if (index >= 0)
			selector->setCurrentIndex(index);
	};

	select_saved_value(source_selector,
			   obs_data_get_string(collection_settings, "source"));
	replay_folder_selector->setText(
		QString::fromUtf8(obs_data_get_string(collection_settings, "replay_folder")));
	const int video_bitrate = (int)obs_data_get_int(collection_settings, "video_bitrate_mbps");
	const int audio_bitrate = (int)obs_data_get_int(collection_settings, "audio_bitrate_kbps");
	if (video_bitrate > 0)
		video_bitrate_selector->setValue(video_bitrate);
	if (audio_bitrate > 0)
		audio_bitrate_selector->setValue(audio_bitrate);
	select_saved_value(replay_scene_selector,
			   obs_data_get_string(collection_settings, "replay_scene"));
	select_saved_value(intro_transition_selector,
			   obs_data_get_string(collection_settings, "intro_transition"));
	select_saved_value(outro_transition_selector,
			   obs_data_get_string(collection_settings, "outro_transition"));

	const int pre_roll_seconds =
		(int)obs_data_get_int(collection_settings, "pre_roll_seconds");
	const int post_roll_seconds =
		(int)obs_data_get_int(collection_settings, "post_roll_seconds");
	if (pre_roll_seconds >= 0)
		pre_roll_seconds_selector->setValue(pre_roll_seconds);
	if (post_roll_seconds >= 0)
		post_roll_seconds_selector->setValue(post_roll_seconds);

	obs_data_release(collection_settings);
	settings_status->setText("Settings loaded for the current scene collection.");
	update_storage_status();
}

void save_settings()
{
	char *path = obs_module_config_path(settings_filename);
	if (!path) {
		settings_status->setText("Could not find the OBS plugin configuration folder.");
		return;
	}

	obs_data_t *settings = obs_data_create_from_json_file_safe(path, "bak");
	if (!settings)
		settings = obs_data_create();
	obs_data_t *collection_settings = get_scene_collection_settings(settings, true);
	if (!collection_settings) {
		obs_data_release(settings);
		bfree(path);
		settings_status->setText("Could not identify the current scene collection.");
		return;
	}

	auto set_selector = [collection_settings](const char *key, QComboBox *selector) {
		const QByteArray value = selector->currentText().toUtf8();
		obs_data_set_string(collection_settings, key, value.constData());
	};
	set_selector("source", source_selector);
	const QByteArray replay_folder = replay_folder_selector->text().toUtf8();
	obs_data_set_string(collection_settings, "replay_folder", replay_folder.constData());
	obs_data_set_int(collection_settings, "video_bitrate_mbps", video_bitrate_selector->value());
	obs_data_set_int(collection_settings, "audio_bitrate_kbps", audio_bitrate_selector->value());
	set_selector("replay_scene", replay_scene_selector);
	set_selector("intro_transition", intro_transition_selector);
	set_selector("outro_transition", outro_transition_selector);
	obs_data_set_int(collection_settings, "pre_roll_seconds", pre_roll_seconds_selector->value());
	obs_data_set_int(collection_settings, "post_roll_seconds", post_roll_seconds_selector->value());

	obs_data_release(collection_settings);
	const bool saved = obs_data_save_json_safe(settings, path, "tmp", "bak");
	obs_data_release(settings);
	bfree(path);
	settings_status->setText(saved ? "Settings saved for this scene collection."
					 : "Could not save settings.");
}

void clear_playout_state()
{
	++playback_generation;
	if (event_playout_timer)
		event_playout_timer->stop();
	if (active_replay_playback_source) {
		if (auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source))
			channel->reset();
		signal_handler_disconnect(obs_source_get_signal_handler(active_replay_playback_source),
					  "media_started", media_started_callback, nullptr);
	}
	obs_source_release(previous_program_scene);
	obs_source_release(previous_transition);
	obs_source_release(active_replay_scene);
	obs_source_release(active_replay_playback_source);
	if (outro_transition_wait_source) {
		const auto token = reinterpret_cast<void *>(
			static_cast<uintptr_t>(outro_transition_wait_generation));
		signal_handler_disconnect(obs_source_get_signal_handler(outro_transition_wait_source),
					  "transition_stop", outro_transition_stop_callback, token);
		obs_source_release(outro_transition_wait_source);
		outro_transition_wait_source = nullptr;
	}
	previous_program_scene = nullptr;
	previous_transition = nullptr;
	active_replay_scene = nullptr;
	active_replay_playback_source = nullptr;
	previous_transition_duration = 0;
	replay_playout_queue.clear();
	replay_playout_index = -1;
	pending_seek_milliseconds = -1;
	pending_live_playout = false;
	awaiting_event_start = false;
	intro_transition_point_milliseconds = 0;
	first_event_intro_lead_milliseconds = 0;
	outro_cleanup_pending = false;
	outro_transition_wait_generation = 0;
	update_playout_button();
}

obs_source_t *source_from_selector(QComboBox *selector)
{
	const QByteArray name = selector->currentText().toUtf8();
	return name.isEmpty() ? nullptr : obs_get_source_by_name(name.constData());
}

void set_recording_controls(bool recording)
{
	start_recording_button->setEnabled(!recording);
	stop_recording_button->setEnabled(recording);
	mark_event_button->setEnabled(recording);
}

void refresh_events_list()
{
	if (!events_list)
		return;

	events_list->clear();
	if (replay_session) {
		for (qsizetype index = 0; index < replay_session->events().size(); ++index) {
			const obs_replays::ReplayEvent &event = replay_session->events().at(index);
			const QString label = event.label.isEmpty() ? "Replay event" : event.label;
			events_list->addItem(QString("%1  %2 → %3")
						 .arg(label)
						 .arg(format_timeline(event.inUs))
						 .arg(format_timeline(event.outUs)));
			events_list->item(events_list->count() - 1)
				->setData(Qt::UserRole, static_cast<int>(index));
		}
	}
	for (const PendingReplayEvent &event : pending_events) {
		events_list->addItem(
			QString("Pending  %1 → %2").arg(format_timeline(event.inUs),
								       format_timeline(event.outUs)));
	}
	if (events_list->count() == 0)
		events_list->addItem("No replay events yet");
}

void mark_replay_event()
{
	if (!replay_session || !source_capture) {
		recording_status->setText("Start recording before marking a replay event.");
		return;
	}

	const obs_replays::TimelineUs now =
		static_cast<obs_replays::TimelineUs>(source_capture->timelineUs());
	const obs_replays::TimelineUs pre_roll = pre_roll_seconds_selector->value() * 1000000LL;
	const obs_replays::TimelineUs post_roll = post_roll_seconds_selector->value() * 1000000LL;
	const int event_number = static_cast<int>(replay_session->events().size() +
								 pending_events.size()) +
				 1;
	pending_events.append({qMax<obs_replays::TimelineUs>(0, now - pre_roll), now + post_roll,
				       QString("Event %1").arg(event_number)});
	refresh_events_list();
	recording_status->setText(post_roll > 0 ? "Replay event marked; waiting for post-roll."
						     : "Replay event marked.");
}

void finalize_active_segment()
{
	if (active_segment_path.isEmpty() || !replay_session || !replay_session->isActive())
		return;

	QString error;
	const QFileInfo segment_file(active_segment_path);
	replay_session->finalizeCurrentSegment(source_capture->timelineUs(),
					      segment_file.exists() ? segment_file.size() : 0, &error);
	active_segment_path.clear();
}

void update_recording_session()
{
	if (!replay_session || !source_capture || !segment_writer)
		return;

	const obs_replays::TimelineUs timeline_us =
		static_cast<obs_replays::TimelineUs>(source_capture->timelineUs());
	replay_session->updateLiveTimeline(timeline_us);
	for (qsizetype i = 0; i < pending_events.size();) {
		const PendingReplayEvent &event = pending_events.at(i);
		if (event.outUs > replay_session->latestTimelineUs()) {
			++i;
			continue;
		}

		QString error;
		if (!replay_session->addEvent(event.inUs, event.outUs, event.label, &error)) {
			recording_status->setText(error);
			++i;
			continue;
		}
		pending_events.removeAt(i);
		refresh_events_list();
	}
	const QString latest_segment = segment_writer->latestSegmentPath();
	if (!latest_segment.isEmpty() && latest_segment != active_segment_path) {
		finalize_active_segment();
		QString error;
		const QString relative_path =
			QDir(replay_session->sessionDirectory()).relativeFilePath(latest_segment);
		if (replay_session->beginSegment(relative_path, timeline_us, &error)) {
			active_segment_path = latest_segment;
			if (pending_live_playout) {
				pending_live_playout = false;
				playout_status->setText("Live replay segment finalized; preparing selected events.");
				QTimer::singleShot(0, []() { play_selected_replay_events(); });
			}
		} else
			recording_status->setText(error);
	}

	if (segment_writer->hasStopped()) {
		finalize_active_segment();
		QString error;
		replay_session->stop(&error);
		segment_writer->release();
		source_capture->stop();
		recording_timer->stop();
		set_recording_controls(false);
		recording_status->setText(error.isEmpty() ? "Recording session stopped."
								 : error);
		segment_writer.reset();
		source_capture.reset();
		return;
	}

	recording_status->setText(QString("Recording: %1 video frames, timeline %2.")
					 .arg(source_capture->capturedVideoFrames())
					 .arg(format_duration(timeline_us / 1000000)));
}

void start_recording_session()
{
	if (replay_session && replay_session->isActive()) {
		recording_status->setText("A replay recording session is already active.");
		return;
	}
	pending_live_playout = false;

	obs_source_t *source = source_from_selector(source_selector);
	if (!source || replay_folder_selector->text().isEmpty()) {
		obs_source_release(source);
		recording_status->setText("Select both a replay source and replay folder.");
		return;
	}

	const QByteArray replay_folder = replay_folder_selector->text().toUtf8();
	const char *source_name = obs_source_get_name(source);
	const char *source_uuid = obs_source_get_uuid(source);
	obs_replays::SessionConfiguration configuration;
	configuration.replayFolder = QString::fromUtf8(replay_folder);
	configuration.sourceName = QString::fromUtf8(source_name ? source_name : "");
	configuration.sourceUuid = QString::fromUtf8(source_uuid ? source_uuid : "");
	configuration.videoBitrateMbps = video_bitrate_selector->value();
	configuration.audioBitrateKbps = audio_bitrate_selector->value();

	auto session = std::make_unique<obs_replays::ReplaySession>();
	QString error;
	if (!session->start(configuration, &error)) {
		obs_source_release(source);
		recording_status->setText(error);
		return;
	}

	auto capture = std::make_unique<obs_replays::SourceCapture>();
	const char *capture_error = nullptr;
	if (!capture->start(source, &capture_error)) {
		obs_source_release(source);
		session->stop(&error);
		recording_status->setText(QString::fromUtf8(capture_error));
		return;
	}
	obs_source_release(source);

	auto writer = std::make_unique<obs_replays::SegmentWriter>();
	const QString segment_path =
		QDir(session->sessionDirectory()).filePath("segments/replay.mkv");
	if (!writer->start(capture->videoOutput(), obs_get_audio(), segment_path,
			   configuration.videoBitrateMbps, configuration.audioBitrateKbps,
			   configuration.segmentDurationSeconds, &error)) {
		capture->stop();
		session->stop(&error);
		recording_status->setText(error);
		return;
	}

	replay_session = std::move(session);
	source_capture = std::move(capture);
	segment_writer = std::move(writer);
	active_segment_path = segment_writer->latestSegmentPath();
	const QString initial_relative_path =
		QDir(replay_session->sessionDirectory()).relativeFilePath(active_segment_path);
	if (!replay_session->beginSegment(initial_relative_path, 0, &error)) {
		segment_writer->stop();
		recording_status->setText(error);
		return;
	}
	pending_events.clear();
	refresh_events_list();
	set_recording_controls(true);
	recording_timer->start();
	recording_status->setText("Starting replay recording session…");
}

void stop_recording_session()
{
	if (!segment_writer || !segment_writer->isActive()) {
		recording_status->setText("There is no active replay recording session.");
		return;
	}
	stop_recording_button->setEnabled(false);
	pending_live_playout = false;
	recording_status->setText("Finalizing replay MKV segment…");
	segment_writer->stop();
}

void close_recording_session_for_shutdown()
{
	if (!replay_session || !replay_session->isActive())
		return;

	if (recording_timer)
		recording_timer->stop();

	bool writer_stopped = true;
	if (segment_writer) {
		segment_writer->stop();
		const uint64_t deadline = os_gettime_ns() + 5000000000ULL;
		while (!segment_writer->hasStopped() && os_gettime_ns() < deadline)
			os_sleep_ms(10);
		writer_stopped = segment_writer->hasStopped();
		if (writer_stopped && source_capture) {
			replay_session->updateLiveTimeline(source_capture->timelineUs());
			finalize_active_segment();
		} else if (!writer_stopped) {
			obs_log(LOG_WARNING,
				"OBS Replays: timed out waiting for the recording output to finalize during shutdown.");
		}
		segment_writer->release();
		segment_writer.reset();
	}

	if (source_capture) {
		source_capture->stop();
		source_capture.reset();
	}

	QString error;
	if (!replay_session->stop(&error))
		obs_log(LOG_WARNING, "OBS Replays: could not close replay session during shutdown: %s",
			error.toUtf8().constData());
	else if (writer_stopped)
		obs_log(LOG_INFO, "OBS Replays: replay recording session finalized for shutdown.");

	replay_session.reset();
	active_segment_path.clear();
	pending_events.clear();
	set_recording_controls(false);
}

obs_source_t *transition_from_selector(QComboBox *selector)
{
	const QString selected = selector->currentText();
	if (selected.isEmpty())
		return nullptr;

	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	obs_source_t *selected_transition = nullptr;
	for (size_t i = 0; i < transitions.sources.num; ++i) {
		obs_source_t *transition = transitions.sources.array[i];
		if (selected == QString::fromUtf8(obs_source_get_name(transition))) {
			selected_transition = obs_source_get_ref(transition);
			break;
		}
	}
	obs_frontend_source_list_free(&transitions);
	return selected_transition;
}

void restore_default_transition()
{
	if (previous_transition) {
		obs_frontend_set_current_transition(previous_transition);
		obs_frontend_set_transition_duration(previous_transition_duration);
	}
}

bool take_replay_to_program(QString *error)
{
	if (active_replay_scene) {
		*error = "A replay is already on Program. Use Return to previous PGM first.";
		return false;
	}

	obs_source_t *replay_scene = source_from_selector(replay_scene_selector);
	if (!replay_scene || obs_source_get_type(replay_scene) != OBS_SOURCE_TYPE_SCENE) {
		obs_source_release(replay_scene);
		*error = "Select an existing Replay scene.";
		return false;
	}

	obs_source_t *current_scene = obs_frontend_get_current_scene();
	if (!current_scene || current_scene == replay_scene) {
		obs_source_release(current_scene);
		obs_source_release(replay_scene);
		*error = "Program is already the Replay scene.";
		return false;
	}

	obs_source_t *intro_transition = transition_from_selector(intro_transition_selector);
	if (!intro_transition) {
		obs_source_release(current_scene);
		obs_source_release(replay_scene);
		*error = "Select an intro transition.";
		return false;
	}
	intro_transition_point_milliseconds = stinger_transition_point_milliseconds(intro_transition);

	obs_source_t *playback_source = obs_get_source_by_name(playback_source_name);
	if (!playback_source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_bool(settings, "looping", false);
		obs_data_set_bool(settings, "restart_on_activate", false);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		playback_source = obs_source_create(playback_source_id, playback_source_name, settings, nullptr);
		obs_data_release(settings);
	}
	if (!playback_source ||
	    !obs_replays::ReplayChannelSource::fromSource(playback_source)) {
		obs_source_release(playback_source);
		obs_source_release(intro_transition);
		obs_source_release(current_scene);
		obs_source_release(replay_scene);
		*error = "OBS could not create its managed Replay Channel A source.";
		return false;
	}

	obs_scene_t *scene = obs_scene_from_source(replay_scene);
	if (!scene || !obs_scene_find_source(scene, playback_source_name)) {
		if (!scene || !obs_scene_add(scene, playback_source)) {
			obs_source_release(playback_source);
			obs_source_release(intro_transition);
			obs_source_release(current_scene);
			obs_source_release(replay_scene);
			*error = "OBS could not add Replay Channel A to the selected Replay scene.";
			return false;
		}
	}

	previous_program_scene = current_scene;
	previous_transition = obs_frontend_get_current_transition();
	previous_transition_duration = obs_frontend_get_transition_duration();
	active_replay_scene = replay_scene;
	active_replay_playback_source = playback_source;
	signal_handler_connect(obs_source_get_signal_handler(active_replay_playback_source),
			       "media_started", media_started_callback, nullptr);
	obs_frontend_set_current_transition(intro_transition);
	replay_scene_change_in_progress = true;
	obs_frontend_set_current_scene(active_replay_scene);
	replay_scene_change_in_progress = false;
	restore_default_transition();
	obs_source_release(intro_transition);
	return true;
}

bool return_to_previous_program(QString *error)
{
	if (!active_replay_scene || !previous_program_scene) {
		*error = "There is no replay playout to return from.";
		return false;
	}

	obs_source_t *outro_transition = transition_from_selector(outro_transition_selector);
	if (!outro_transition) {
		*error = "Select an outro transition.";
		return false;
	}
	// OBS emits transition_stop only after the stinger's real animation has
	// finished. Keep a reference and clean up from that signal rather than from
	// a guessed duration.
	outro_transition_wait_generation = playback_generation;
	const auto token = reinterpret_cast<void *>(
		static_cast<uintptr_t>(outro_transition_wait_generation));
	outro_transition_wait_source = outro_transition;
	signal_handler_connect(obs_source_get_signal_handler(outro_transition_wait_source),
			       "transition_stop", outro_transition_stop_callback, token);
	obs_frontend_set_current_transition(outro_transition);
	outro_cleanup_pending = true;
	update_playout_button();
	replay_scene_change_in_progress = true;
	obs_frontend_set_current_scene(previous_program_scene);
	replay_scene_change_in_progress = false;
	restore_default_transition();
	playout_status->setText("Outro transition in progress…");
	return true;
}

void finish_outro_transition(void *data)
{
	const uint64_t generation = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
	if (!outro_cleanup_pending || generation != playback_generation)
		return;
	outro_cleanup_pending = false;
	clear_playout_state();
	playout_status->setText("Returned to the previous Program scene.");
}

void outro_transition_stop_callback(void *data, calldata_t *)
{
	obs_queue_task(OBS_TASK_UI, finish_outro_transition, data, false);
}

void start_active_replay_event(void *data)
{
	const uint64_t generation = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
	if (generation != playback_generation)
		return;
	if (!awaiting_event_start || !active_replay_playback_source || replay_playout_index < 0 ||
	    replay_playout_index >= replay_playout_queue.size())
		return;

	awaiting_event_start = false;
	const ReplayPlayoutItem &item = replay_playout_queue.at(replay_playout_index);
	auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source);
	if (!channel)
		return;
	qint64 timer_duration = item.durationMilliseconds;
	if (replay_playout_index == 0)
		timer_duration += first_event_intro_lead_milliseconds;
	if (event_playout_timer)
		event_playout_timer->start(static_cast<int>(std::max<qint64>(0, timer_duration)));
	playout_status->setText(QString("Playing %1 (%2 of %3).").arg(item.label)
				 .arg(replay_playout_index + 1)
				 .arg(replay_playout_queue.size()));
	if (replay_playout_index + 1 < replay_playout_queue.size()) {
		const ReplayPlayoutItem &next = replay_playout_queue.at(replay_playout_index + 1);
		QString error;
		if (!channel->cueNext(next.segmentPath, next.inMilliseconds, &error))
			playout_status->setText(error);
	}
}

void media_started_callback(void *, calldata_t *)
{
	const auto token = reinterpret_cast<void *>(static_cast<uintptr_t>(playback_generation));
	obs_queue_task(OBS_TASK_UI, start_active_replay_event, token, false);
}

bool play_next_replay_event(QString *error)
{
	if (!active_replay_playback_source) {
		*error = "Replay Playback is not ready.";
		return false;
	}

	++replay_playout_index;
	if (replay_playout_index >= replay_playout_queue.size())
		return return_to_previous_program(error);

	const ReplayPlayoutItem &item = replay_playout_queue.at(replay_playout_index);
	auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source);
	if (!channel) {
		*error = "The selected Replay Channel source is unavailable.";
		return false;
	}
	first_event_intro_lead_milliseconds = 0;
	if (replay_playout_index == 0) {
		first_event_intro_lead_milliseconds =
			std::min(item.inMilliseconds, intro_transition_point_milliseconds);
	}
	pending_seek_milliseconds = item.inMilliseconds - first_event_intro_lead_milliseconds;
	awaiting_event_start = true;
	if (!channel->load(item.segmentPath, pending_seek_milliseconds, error)) {
		awaiting_event_start = false;
		return false;
	}
	return true;
}

void advance_replay_playout()
{
	if (replay_playout_index + 1 < replay_playout_queue.size()) {
		auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source);
		QString error;
		// takeCued emits media_started synchronously. Publish the next queue
		// item before calling it so the callback always arms that event's own
		// out-point timer (rather than being discarded as the previous item).
		const int previous_index = replay_playout_index;
		++replay_playout_index;
		pending_seek_milliseconds = replay_playout_queue.at(replay_playout_index).inMilliseconds;
		awaiting_event_start = true;
		if (!channel || !channel->takeCued(&error)) {
			replay_playout_index = previous_index;
			awaiting_event_start = false;
			playout_status->setText(error);
			return;
		}
		return;
	}
	QString error;
	const bool completed = replay_playout_index + 1 >= replay_playout_queue.size();
	if (!play_next_replay_event(&error))
		playout_status->setText(error);
	else if (completed)
		playout_status->setText("Replay playout complete; returned to the previous Program scene.");
}

void play_selected_replay_events()
{
	if (!replay_session || events_list->selectedItems().isEmpty()) {
		playout_status->setText("Select one or more finalized replay events first.");
		return;
	}

	QVector<ReplayPlayoutItem> queue;
	bool requires_live_split = false;
	QVector<int> selected_event_indices;
	for (QListWidgetItem *selected_item : events_list->selectedItems()) {
		bool has_event_index = false;
		const int event_index = selected_item->data(Qt::UserRole).toInt(&has_event_index);
		if (!has_event_index || event_index < 0 || event_index >= replay_session->events().size()) {
			playout_status->setText("Pending events cannot be played until their post-roll is recorded.");
			return;
		}
		selected_event_indices.append(event_index);
	}
	std::sort(selected_event_indices.begin(), selected_event_indices.end());
	for (const int event_index : selected_event_indices) {
		const obs_replays::ReplayEvent &event = replay_session->events().at(event_index);
		const obs_replays::ReplaySegment *segment = nullptr;
		for (const obs_replays::ReplaySegment &candidate : replay_session->recordedSegments()) {
			if (candidate.finalized && event.inUs >= candidate.startUs && event.outUs <= candidate.endUs) {
				segment = &candidate;
				break;
			}
		}
		if (!segment) {
			if (replay_session->isActive() && segment_writer && segment_writer->isActive()) {
				requires_live_split = true;
				continue;
			}
			playout_status->setText("This event crosses replay segments. Multi-segment playout is the next controller milestone.");
			return;
		}
		const QString path = QDir(replay_session->sessionDirectory()).filePath(segment->relativePath);
		if (!QFileInfo::exists(path)) {
			playout_status->setText("A segment file for the selected event is missing.");
			return;
		}
		queue.append({path, (event.inUs - segment->startUs) / 1000,
			      (event.outUs - event.inUs) / 1000,
			      event.label.isEmpty() ? "Replay event" : event.label});
	}
	if (requires_live_split) {
		QString error;
		if (!segment_writer->requestSplit(&error)) {
			playout_status->setText(error);
			return;
		}
		pending_live_playout = true;
		playout_status->setText("Finalizing the live replay segment at the next keyframe; playback will begin automatically.");
		return;
	}

	QString error;
	if (!take_replay_to_program(&error)) {
		playout_status->setText(error);
		return;
	}
	update_playout_button();
	replay_playout_queue = queue;
	replay_playout_index = -1;
	++playback_generation;
	if (auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source))
		channel->reset();
	if (!play_next_replay_event(&error))
		playout_status->setText(error);
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		close_recording_session_for_shutdown();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		clear_playout_state();
		refresh_source_selector();
		refresh_playout_selectors();
		load_settings();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED && active_replay_scene &&
	    !replay_scene_change_in_progress) {
		obs_source_t *current_scene = obs_frontend_get_current_scene();
		const bool replay_still_on_program = current_scene == active_replay_scene;
		obs_source_release(current_scene);
		if (!replay_still_on_program && !outro_cleanup_pending)
			clear_playout_state();
	}
}

QWidget *create_replay_dock()
{
	auto *dock = new QScrollArea();
	dock->setWidgetResizable(true);
	auto *content = new QWidget(dock);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);

	auto *capture_toggle = new QToolButton(content);
	capture_toggle->setText("Replay capture settings");
	capture_toggle->setCheckable(true);
	capture_toggle->setChecked(true);
	capture_toggle->setArrowType(Qt::DownArrow);
	capture_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	layout->addWidget(capture_toggle);
	auto *capture_group = new QWidget(content);
	auto *capture_form = new QFormLayout(capture_group);
	source_selector = new QComboBox(capture_group);
	capture_form->addRow("Source", source_selector);

	auto *replay_folder_row = new QWidget(capture_group);
	auto *replay_folder_layout = new QHBoxLayout(replay_folder_row);
	replay_folder_layout->setContentsMargins(0, 0, 0, 0);
	replay_folder_selector = new QLineEdit(replay_folder_row);
	replay_folder_selector->setReadOnly(true);
	replay_folder_selector->setPlaceholderText("Choose a folder for replay sessions");
	replay_folder_layout->addWidget(replay_folder_selector);
	auto *choose_replay_folder = new QPushButton("Browse", replay_folder_row);
	QObject::connect(choose_replay_folder, &QPushButton::clicked, []() {
		const QString folder = QFileDialog::getExistingDirectory(
			replay_dock, "Choose replay folder", replay_folder_selector->text(),
			QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (!folder.isEmpty())
			replay_folder_selector->setText(QDir::toNativeSeparators(folder));
	});
	replay_folder_layout->addWidget(choose_replay_folder);
	capture_form->addRow("Replay folder", replay_folder_row);

	video_bitrate_selector = new QSpinBox(capture_group);
	video_bitrate_selector->setRange(1, 250);
	video_bitrate_selector->setValue(25);
	video_bitrate_selector->setSuffix(" Mb/s");
	capture_form->addRow("Target video bitrate", video_bitrate_selector);

	audio_bitrate_selector = new QSpinBox(capture_group);
	audio_bitrate_selector->setRange(32, 512);
	audio_bitrate_selector->setValue(160);
	audio_bitrate_selector->setSuffix(" kb/s");
	capture_form->addRow("Target audio bitrate", audio_bitrate_selector);

	storage_status = new QLabel(capture_group);
	storage_status->setWordWrap(true);
	capture_form->addRow("Storage", storage_status);
	QObject::connect(replay_folder_selector, &QLineEdit::textChanged,
			 [](const QString &) { update_storage_status(); });
	QObject::connect(video_bitrate_selector,
			 QOverload<int>::of(&QSpinBox::valueChanged),
			 [](int) { update_storage_status(); });
	QObject::connect(audio_bitrate_selector,
			 QOverload<int>::of(&QSpinBox::valueChanged),
			 [](int) { update_storage_status(); });
	auto *storage_timer = new QTimer(capture_group);
	storage_timer->setInterval(5000);
	QObject::connect(storage_timer, &QTimer::timeout, []() { update_storage_status(); });
	storage_timer->start();

	pre_roll_seconds_selector = new QSpinBox(capture_group);
	pre_roll_seconds_selector->setRange(0, 60);
	pre_roll_seconds_selector->setValue(5);
	pre_roll_seconds_selector->setSuffix(" s");
	capture_form->addRow("Default event pre-roll", pre_roll_seconds_selector);

	post_roll_seconds_selector = new QSpinBox(capture_group);
	post_roll_seconds_selector->setRange(0, 60);
	post_roll_seconds_selector->setValue(5);
	post_roll_seconds_selector->setSuffix(" s");
	capture_form->addRow("Default event post-roll", post_roll_seconds_selector);

	auto *refresh_sources = new QPushButton("Refresh sources", capture_group);
	QObject::connect(refresh_sources, &QPushButton::clicked,
			 []() { refresh_source_selector(); });
	capture_form->addRow(QString(), refresh_sources);
	layout->addWidget(capture_group);
	QObject::connect(capture_toggle, &QToolButton::toggled, [capture_group, capture_toggle](bool expanded) {
		capture_group->setVisible(expanded);
		capture_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	});

	auto *playout_toggle = new QToolButton(content);
	playout_toggle->setText("Replay playout settings");
	playout_toggle->setCheckable(true);
	playout_toggle->setChecked(true);
	playout_toggle->setArrowType(Qt::DownArrow);
	playout_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	layout->addWidget(playout_toggle);
	auto *playout_group = new QWidget(content);
	auto *playout_form = new QFormLayout(playout_group);
	replay_scene_selector = new QComboBox(playout_group);
	playout_form->addRow("Replay scene", replay_scene_selector);
	intro_transition_selector = new QComboBox(playout_group);
	playout_form->addRow("Intro transition", intro_transition_selector);
	outro_transition_selector = new QComboBox(playout_group);
	playout_form->addRow("Outro transition", outro_transition_selector);

	auto *refresh_playout = new QPushButton("Refresh scenes and transitions", playout_group);
	QObject::connect(refresh_playout, &QPushButton::clicked,
			 []() {
				 refresh_playout_selectors();
			 });
	playout_form->addRow(QString(), refresh_playout);
	layout->addWidget(playout_group);
	QObject::connect(playout_toggle, &QToolButton::toggled, [playout_group, playout_toggle](bool expanded) {
		playout_group->setVisible(expanded);
		playout_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	});

	auto *session_group = new QGroupBox("Replay session", content);
	auto *session_layout = new QVBoxLayout(session_group);
	recording_status = new QLabel("No recording session.", session_group);
	recording_status->setWordWrap(true);
	session_layout->addWidget(recording_status);
	start_recording_button = new QPushButton("Start recording", session_group);
	QObject::connect(start_recording_button, &QPushButton::clicked,
			 []() { start_recording_session(); });
	stop_recording_button = new QPushButton("Stop recording", session_group);
	stop_recording_button->setEnabled(false);
	QObject::connect(stop_recording_button, &QPushButton::clicked,
			 []() { stop_recording_session(); });
	auto *recording_buttons = new QWidget(session_group);
	auto *recording_buttons_layout = new QHBoxLayout(recording_buttons);
	recording_buttons_layout->setContentsMargins(0, 0, 0, 0);
	recording_buttons_layout->addWidget(start_recording_button);
	recording_buttons_layout->addWidget(stop_recording_button);
	session_layout->addWidget(recording_buttons);
	playout_status = new QLabel("Ready.", session_group);
	playout_status->setWordWrap(true);
	session_layout->addWidget(playout_status);
	recording_timer = new QTimer(session_group);
	recording_timer->setInterval(250);
	QObject::connect(recording_timer, &QTimer::timeout,
			 []() { update_recording_session(); });
	layout->addWidget(session_group);

	auto *events_group = new QGroupBox("Replay events", content);
	auto *events_layout = new QVBoxLayout(events_group);
	play_events_button = new QPushButton("Play selected events", events_group);
	QObject::connect(play_events_button, &QPushButton::clicked, []() {
		if (outro_cleanup_pending)
			return;
		if (active_replay_scene) {
			QString error;
			if (!return_to_previous_program(&error))
				playout_status->setText(error);
			return;
		}
		play_selected_replay_events();
	});
	events_layout->addWidget(play_events_button);
	events_list = new QListWidget(events_group);
	events_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	events_layout->addWidget(events_list);
	mark_event_button = new QPushButton("Mark event", events_group);
	mark_event_button->setEnabled(false);
	QObject::connect(mark_event_button, &QPushButton::clicked, []() { mark_replay_event(); });
	events_layout->addWidget(mark_event_button);
	event_playout_timer = new QTimer(events_group);
	event_playout_timer->setSingleShot(true);
	QObject::connect(event_playout_timer, &QTimer::timeout, []() { advance_replay_playout(); });
	layout->addWidget(events_group);

	auto *save_button = new QPushButton("Save settings", content);
	QObject::connect(save_button, &QPushButton::clicked, []() { save_settings(); });
	layout->addWidget(save_button);
	settings_status = new QLabel(content);
	settings_status->setWordWrap(true);
	layout->addWidget(settings_status);

	layout->addStretch();
	refresh_source_selector();
	refresh_playout_selectors();
	load_settings();
	update_storage_status();
	refresh_events_list();
	update_playout_button();
	dock->setWidget(content);
	return dock;
}

} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_replays::ReplayChannelSource::registerSourceTypes();
	char *config_dir = obs_module_config_path(nullptr);
	if (config_dir) {
		if (os_mkdirs(config_dir) != 0)
			obs_log(LOG_WARNING, "Could not create configuration directory: %s",
				config_dir);
		bfree(config_dir);
	}

	replay_dock = create_replay_dock();
	if (!obs_frontend_add_dock_by_id(dock_id, dock_title, replay_dock)) {
		obs_log(LOG_ERROR, "Could not register the replay dock");
		delete replay_dock;
		replay_dock = nullptr;
		return false;
	}
	obs_frontend_add_event_callback(frontend_event, nullptr);

	obs_log(LOG_INFO, "OBS Replays loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	close_recording_session_for_shutdown();
	clear_playout_state();
	if (replay_dock)
		obs_frontend_remove_dock(dock_id);
	replay_dock = nullptr;
	source_selector = nullptr;
	replay_folder_selector = nullptr;
	video_bitrate_selector = nullptr;
	audio_bitrate_selector = nullptr;
	replay_scene_selector = nullptr;
	intro_transition_selector = nullptr;
	outro_transition_selector = nullptr;
	pre_roll_seconds_selector = nullptr;
	post_roll_seconds_selector = nullptr;
	settings_status = nullptr;
	playout_status = nullptr;
	recording_status = nullptr;
	storage_status = nullptr;
	mark_event_button = nullptr;
	play_events_button = nullptr;
	events_list = nullptr;
	obs_log(LOG_INFO, "OBS Replays unloaded");
}
