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
#include "obs-websocket-vendor-api.h"

#include <QByteArray>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStorageInfo>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QVector>

#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

namespace {

class EventTableSplitter final : public QSplitter {
private:
	class Handle final : public QSplitterHandle {
	public:
		explicit Handle(EventTableSplitter *splitter)
			: QSplitterHandle(Qt::Vertical, splitter), splitter(splitter)
		{
		}

	protected:
		void mousePressEvent(QMouseEvent *event) override
		{
			start_y = event->globalPosition().y();
			start_table_height = splitter->widget(0)->height();
			event->accept();
		}

		void mouseMoveEvent(QMouseEvent *event) override
		{
			splitter->resizeTable(start_table_height +
				static_cast<int>(event->globalPosition().y() - start_y));
			event->accept();
		}

	private:
		EventTableSplitter *splitter;
		qreal start_y = 0.0;
		int start_table_height = 0;
	};

public:
	explicit EventTableSplitter(QWidget *parent) : QSplitter(Qt::Vertical, parent) {}

	void resizeTable(int height)
	{
		constexpr int minimum_table_height = 80;
		const int desired_height = qMax(minimum_table_height, height);
		QWidget *table = widget(0);
		QWidget *controls = widget(1);
		if (!table || !controls)
			return;
		table->setMinimumHeight(desired_height);
		setMinimumHeight(desired_height + controls->minimumSizeHint().height() + handleWidth());
		setSizes({desired_height, controls->height()});
		updateGeometry();
		if (parentWidget())
			parentWidget()->updateGeometry();
	}

protected:
	QSplitterHandle *createHandle() override { return new Handle(this); }
};

class DockSpinBox final : public QSpinBox {
public:
	using QSpinBox::QSpinBox;

protected:
	void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

class DockSlider final : public QSlider {
public:
	using QSlider::QSlider;

protected:
	void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

constexpr const char *dock_id = "obs-replays.dock";
constexpr const char *dock_title = "Replays";
constexpr const char *settings_filename = "settings.json";
constexpr const char *scene_collections_key = "scene_collections";
constexpr const char *playback_source_name = "OBS Replays Channel A";
constexpr const char *playback_source_id = "obs_replays_channel_a";

QWidget *replay_dock = nullptr;
QWidget *capture_settings_group = nullptr;
QComboBox *source_selector = nullptr;
QLineEdit *replay_folder_selector = nullptr;
QPushButton *choose_replay_folder_button = nullptr;
QSpinBox *video_bitrate_selector = nullptr;
QSpinBox *audio_bitrate_selector = nullptr;
QComboBox *replay_scene_selector = nullptr;
QComboBox *intro_transition_selector = nullptr;
QComboBox *outro_transition_selector = nullptr;
QComboBox *between_events_transition_selector = nullptr;
QSpinBox *between_events_fade_duration_selector = nullptr;
QSlider *playback_speed_selector = nullptr;
QLabel *playback_speed_value = nullptr;
QLabel *playout_status = nullptr;
QLabel *recording_status = nullptr;
QLabel *storage_status = nullptr;
QPushButton *start_recording_button = nullptr;
QPushButton *stop_recording_button = nullptr;
QVector<QPushButton *> mark_event_buttons;
QPushButton *delete_events_button = nullptr;
QPushButton *play_events_button = nullptr;
QTableWidget *events_table = nullptr;
QTimer *recording_timer = nullptr;
QTimer *settings_save_timer = nullptr;
// OBS can destroy frontend dock widgets before calling obs_module_unload().
// Keep shutdown cleanup independent from those Qt objects.
bool module_unloading = false;
obs_websocket_vendor websocket_vendor = nullptr;
std::unique_ptr<obs_replays::ReplaySession> replay_session;
std::unique_ptr<obs_replays::SourceCapture> source_capture;
std::unique_ptr<obs_replays::SegmentWriter> segment_writer;
uint64_t recording_stop_requested_ns = 0;
bool recording_force_stop_issued = false;

obs_source_t *previous_program_scene = nullptr;
obs_source_t *previous_transition = nullptr;
obs_source_t *active_replay_scene = nullptr;
obs_source_t *active_replay_playback_source = nullptr;
int previous_transition_duration = 0;
bool replay_scene_change_in_progress = false;
QTimer *event_playout_timer = nullptr;
double event_playout_remaining_media_milliseconds = 0.0;
uint64_t event_playout_last_tick_ns = 0;

struct ReplayPlayoutItem {
	QString eventId;
	QString segmentPath;
	qint64 inMilliseconds = 0;
	qint64 durationMilliseconds = 0;
	QString label;
};

QVector<ReplayPlayoutItem> replay_playout_queue;
int replay_playout_index = -1;
qint64 pending_seek_milliseconds = -1;
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
void advance_replay_playout();
bool parse_timeline(const QString &text, obs_replays::TimelineUs *timestamp_us);
void refresh_events_list();
void start_recording_session();
void stop_recording_session();
bool play_replay_event_indices(const QVector<int> &event_indices, QString *error);

void set_websocket_string(obs_data_t *data, const char *key, const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	obs_data_set_string(data, key, utf8.constData());
}

void write_websocket_session(obs_data_t *data)
{
	obs_data_set_bool(data, "available", replay_session != nullptr);
	if (!replay_session)
		return;

	const auto &configuration = replay_session->sessionConfiguration();
	set_websocket_string(data, "id", replay_session->sessionId());
	set_websocket_string(data, "folder", replay_session->sessionDirectory());
	set_websocket_string(data, "sourceName", configuration.sourceName);
	set_websocket_string(data, "sourceUuid", configuration.sourceUuid);
	obs_data_set_bool(data, "recording", replay_session->isActive());
	set_websocket_string(data, "activeTakeId",
			     replay_session->activeTakeId().toString(QUuid::WithoutBraces));
	obs_data_set_int(data, "recordedThroughMs", replay_session->latestTimelineUs() / 1000);
	obs_data_set_int(data, "takeCount", replay_session->takes().size());
	obs_data_set_int(data, "eventCount", replay_session->events().size());
}

void write_websocket_event(obs_data_t *data, const obs_replays::ReplayEvent &event, int index)
{
	set_websocket_string(data, "id", event.id.toString(QUuid::WithoutBraces));
	set_websocket_string(data, "takeId", event.takeId.toString(QUuid::WithoutBraces));
	set_websocket_string(data, "label", event.label);
	obs_data_set_int(data, "index", index + 1);
	obs_data_set_int(data, "inMs", event.inUs / 1000);
	obs_data_set_int(data, "outMs", event.outUs / 1000);
	set_websocket_string(data, "createdAtUtc", event.createdAtUtc.toString(Qt::ISODateWithMs));
}

void write_websocket_events(obs_data_t *data)
{
	obs_data_array_t *events = obs_data_array_create();
	if (replay_session) {
		for (qsizetype index = 0; index < replay_session->events().size(); ++index) {
			obs_data_t *event = obs_data_create();
			write_websocket_event(event, replay_session->events().at(index), static_cast<int>(index));
			obs_data_array_push_back(events, event);
			obs_data_release(event);
		}
	}
	obs_data_set_array(data, "events", events);
	obs_data_array_release(events);
}

void write_websocket_recording_status(obs_data_t *data)
{
	const bool active = replay_session && replay_session->isActive();
	obs_data_set_bool(data, "active", active);
	if (!active)
		return;
	set_websocket_string(data, "sessionId", replay_session->sessionId());
	set_websocket_string(data, "takeId", replay_session->activeTakeId().toString(QUuid::WithoutBraces));
	obs_data_set_int(data, "recordedThroughMs", replay_session->latestTimelineUs() / 1000);
	if (source_capture) {
		obs_data_set_int(data, "capturedVideoFrames", source_capture->capturedVideoFrames());
		obs_data_set_int(data, "capturedAudioFrames", source_capture->capturedAudioFrames());
	}
}

void emit_websocket_event(const char *name)
{
	if (!websocket_vendor || module_unloading)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "protocolVersion", 1);
	write_websocket_session(data);
	obs_replays_websocket_emit_event(websocket_vendor, name, data);
	obs_data_release(data);
}

using WebsocketResponseWriter = void (*)(obs_data_t *, obs_data_t *);

struct WebsocketResponseTask {
	obs_data_t *request;
	obs_data_t *response;
	WebsocketResponseWriter writer;
};

void write_websocket_response_task(void *param)
{
	auto *task = static_cast<WebsocketResponseTask *>(param);
	task->writer(task->request, task->response);
}

void websocket_request_callback(obs_data_t *request, obs_data_t *response, void *priv_data)
{
	auto writer = *static_cast<WebsocketResponseWriter *>(priv_data);
	WebsocketResponseTask task{request, response, writer};
	if (obs_in_task_thread(OBS_TASK_UI))
		write_websocket_response_task(&task);
	else
		obs_queue_task(OBS_TASK_UI, write_websocket_response_task, &task, true);
}

void write_get_session_response(obs_data_t *, obs_data_t *response)
{
	obs_data_set_int(response, "protocolVersion", 1);
	write_websocket_session(response);
}

void write_get_recording_status_response(obs_data_t *, obs_data_t *response)
{
	obs_data_set_int(response, "protocolVersion", 1);
	write_websocket_recording_status(response);
}

void write_list_events_response(obs_data_t *, obs_data_t *response)
{
	obs_data_set_int(response, "protocolVersion", 1);
	if (replay_session)
		set_websocket_string(response, "sessionId", replay_session->sessionId());
	write_websocket_events(response);
}


void update_playout_button()
{
	if (module_unloading || !play_events_button)
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

qint64 between_events_fade_duration_milliseconds()
{
	if (!between_events_transition_selector || !between_events_fade_duration_selector ||
	    between_events_transition_selector->currentData().toString() != "fade")
		return 0;
	return between_events_fade_duration_selector->value();
}

int playback_speed_percent()
{
	return playback_speed_selector ? playback_speed_selector->value() : 100;
}

void update_playback_speed(int percent)
{
	if (playback_speed_value)
		playback_speed_value->setText(QString("%1%").arg(percent));
	if (active_replay_playback_source) {
		if (auto *channel = obs_replays::ReplayChannelSource::fromSource(
			    active_replay_playback_source))
			channel->setPlaybackSpeed(percent);
	}
}

void update_event_playout_timer()
{
	if (!event_playout_timer) {
		advance_replay_playout();
		return;
	}
	if (event_playout_remaining_media_milliseconds <= 0.0) {
		if (event_playout_timer)
			event_playout_timer->stop();
		advance_replay_playout();
		return;
	}

	const uint64_t now = os_gettime_ns();
	if (event_playout_last_tick_ns) {
		const double elapsed_milliseconds = (now - event_playout_last_tick_ns) / 1000000.0;
		event_playout_remaining_media_milliseconds -=
			elapsed_milliseconds * playback_speed_percent() / 100.0;
	}
	event_playout_last_tick_ns = now;
	const bool has_next_event = replay_playout_index >= 0 &&
		replay_playout_index + 1 < replay_playout_queue.size();
	const double transition_lead_media_milliseconds = has_next_event
		? between_events_fade_duration_milliseconds() * playback_speed_percent() / 100.0
		: 0.0;
	if (event_playout_remaining_media_milliseconds <= transition_lead_media_milliseconds) {
		event_playout_timer->stop();
		advance_replay_playout();
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
	const QString message = QString("Available: %1 GiB. Estimated recording time: %2.")
				    .arg(QString::number(free_gib, 'f', 1), format_duration(recordable_seconds));
	storage_status->setText(message);
}

QString format_recording_duration(obs_replays::TimelineUs timestamp_us)
{
	const qint64 total_seconds = timestamp_us / 1000000;
	const qint64 hours = total_seconds / 3600;
	const qint64 minutes = (total_seconds / 60) % 60;
	const qint64 seconds = total_seconds % 60;
	return QString("%1:%2:%3")
		.arg(hours, 2, 10, QLatin1Char('0'))
		.arg(minutes, 2, 10, QLatin1Char('0'))
		.arg(seconds, 2, 10, QLatin1Char('0'));
}

void clear_replay_folder()
{
	if (!replay_folder_selector || replay_folder_selector->text().isEmpty()) {
		QMessageBox::information(replay_dock, "Clear replay folder", "Choose a replay folder first.");
		return;
	}
	if (replay_session && replay_session->isActive()) {
		QMessageBox::information(replay_dock, "Clear replay folder",
				       "Stop the active replay recording session before clearing its folder.");
		return;
	}

	const QDir replay_folder(replay_folder_selector->text());
	if (!replay_folder.exists()) {
		QMessageBox::information(replay_dock, "Clear replay folder", "The selected replay folder is not available.");
		return;
	}

	QFileInfoList sessions;
	for (const QFileInfo &entry : replay_folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (entry.fileName().startsWith("OBS-Replay-"))
			sessions.append(entry);
	}
	if (sessions.isEmpty()) {
		QMessageBox::information(replay_dock, "Clear replay folder", "There are no OBS Replays sessions to remove.");
		return;
	}

	const QString prompt = QString("Permanently delete %1 replay session%2 from this folder?\n\n"
					 "This cannot be undone.")
				       .arg(sessions.size())
				       .arg(sessions.size() == 1 ? "" : "s");
	if (QMessageBox::warning(replay_dock, "Clear replay folder", prompt,
				 QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel) != QMessageBox::Yes)
		return;

	int removed = 0;
	QStringList failures;
	for (const QFileInfo &session : sessions) {
		if (QDir(session.absoluteFilePath()).removeRecursively())
			++removed;
		else
			failures.append(session.fileName());
	}
	if (removed > 0) {
		replay_session.reset();
		refresh_events_list();
		emit_websocket_event("SessionChanged");
	}
	update_storage_status();
	if (failures.isEmpty()) {
		QMessageBox::information(replay_dock, "Clear replay folder",
				       QString("Removed %1 replay session%2.").arg(removed).arg(removed == 1 ? "" : "s"));
	} else {
		QMessageBox::warning(replay_dock, "Clear replay folder",
				     QString("Removed %1 session%2, but could not remove: %3")
					     .arg(removed)
					     .arg(removed == 1 ? "" : "s")
					     .arg(failures.join(", ")));
	}
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
	if (!collection_settings)
		return;

	const QSignalBlocker source_blocker(source_selector);
	const QSignalBlocker replay_folder_blocker(replay_folder_selector);
	const QSignalBlocker video_bitrate_blocker(video_bitrate_selector);
	const QSignalBlocker audio_bitrate_blocker(audio_bitrate_selector);
	const QSignalBlocker scene_blocker(replay_scene_selector);
	const QSignalBlocker intro_blocker(intro_transition_selector);
	const QSignalBlocker outro_blocker(outro_transition_selector);
	const QSignalBlocker between_events_blocker(between_events_transition_selector);
	const QSignalBlocker fade_duration_blocker(between_events_fade_duration_selector);
	const QSignalBlocker playback_speed_blocker(playback_speed_selector);

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
	const QString between_events = QString::fromUtf8(
		obs_data_get_string(collection_settings, "between_events_transition"));
	const int between_events_index = between_events_transition_selector->findData(
		between_events.isEmpty() ? "fade" : between_events);
	between_events_transition_selector->setCurrentIndex(
		between_events_index >= 0 ? between_events_index : 0);
	const int fade_duration =
		static_cast<int>(obs_data_get_int(collection_settings, "between_events_fade_duration_ms"));
	if (fade_duration > 0)
		between_events_fade_duration_selector->setValue(fade_duration);
	playback_speed_selector->setValue(100);
	update_playback_speed(playback_speed_selector->value());

	obs_data_release(collection_settings);
	update_storage_status();
}

void save_settings()
{
	char *path = obs_module_config_path(settings_filename);
	if (!path) {
		obs_log(LOG_WARNING, "OBS Replays: could not find the settings folder.");
		return;
	}

	obs_data_t *settings = obs_data_create_from_json_file_safe(path, "bak");
	if (!settings)
		settings = obs_data_create();
	obs_data_t *collection_settings = get_scene_collection_settings(settings, true);
	if (!collection_settings) {
		obs_data_release(settings);
		bfree(path);
		obs_log(LOG_WARNING, "OBS Replays: could not identify the current scene collection.");
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
	obs_data_set_string(collection_settings, "between_events_transition",
			    between_events_transition_selector->currentData().toString().toUtf8().constData());
	obs_data_set_int(collection_settings, "between_events_fade_duration_ms",
			 between_events_fade_duration_selector->value());
	obs_data_erase(collection_settings, "playback_speed_percent");
	obs_data_erase(collection_settings, "pre_roll_seconds");
	obs_data_erase(collection_settings, "post_roll_seconds");

	obs_data_release(collection_settings);
	const bool saved = obs_data_save_json_safe(settings, path, "tmp", "bak");
	obs_data_release(settings);
	bfree(path);
	if (!saved)
		obs_log(LOG_WARNING, "OBS Replays: could not save settings.");
}

void schedule_settings_save()
{
	if (!module_unloading && settings_save_timer)
		settings_save_timer->start(350);
}

void flush_scheduled_settings_save()
{
	if (settings_save_timer && settings_save_timer->isActive()) {
		settings_save_timer->stop();
		save_settings();
	}
}

void clear_playout_state()
{
	++playback_generation;
	if (!module_unloading && event_playout_timer)
		event_playout_timer->stop();
	event_playout_remaining_media_milliseconds = 0.0;
	event_playout_last_tick_ns = 0;
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
	if (module_unloading)
		return;
	if (start_recording_button)
		start_recording_button->setEnabled(!recording);
	if (stop_recording_button)
		stop_recording_button->setEnabled(recording);
	if (capture_settings_group)
		capture_settings_group->setEnabled(!recording);
	if (choose_replay_folder_button)
		choose_replay_folder_button->setEnabled(!recording);
	for (QPushButton *button : mark_event_buttons)
		button->setEnabled(recording);
}

void refresh_events_list()
{
	if (!events_table)
		return;

	const QSignalBlocker blocker(events_table);
	events_table->setRowCount(0);
	if (replay_session) {
		for (qsizetype index = 0; index < replay_session->events().size(); ++index) {
			const obs_replays::ReplayEvent &event = replay_session->events().at(index);
			const int row = events_table->rowCount();
			events_table->insertRow(row);
			auto *number = new QTableWidgetItem(QString::number(index + 1));
			number->setData(Qt::UserRole, static_cast<int>(index));
			number->setFlags(number->flags() & ~Qt::ItemIsEditable);
			events_table->setItem(row, 0, number);
			events_table->setItem(row, 1, new QTableWidgetItem(format_timeline(event.inUs)));
			events_table->setItem(row, 2, new QTableWidgetItem(format_timeline(event.outUs)));
			events_table->setItem(row, 3, new QTableWidgetItem(event.label));
		}
	}
}

void update_replay_event_from_table(int row, int column)
{
	if (!replay_session || column == 0 || row < 0)
		return;
	QTableWidgetItem *number = events_table->item(row, 0);
	QTableWidgetItem *in = events_table->item(row, 1);
	QTableWidgetItem *out = events_table->item(row, 2);
	QTableWidgetItem *label = events_table->item(row, 3);
	if (!number || !in || !out || !label)
		return;

	bool index_ok = false;
	const int event_index = number->data(Qt::UserRole).toInt(&index_ok);
	obs_replays::TimelineUs in_us = 0;
	obs_replays::TimelineUs out_us = 0;
	if (!index_ok || !parse_timeline(in->text(), &in_us) || !parse_timeline(out->text(), &out_us)) {
		playout_status->setText("Enter event times as HH:MM:SS.mmm (or seconds).");
		refresh_events_list();
		return;
	}

	QString error;
	if (!replay_session->updateEvent(event_index, in_us, out_us, label->text(), &error))
		playout_status->setText(error);
	else {
		playout_status->setText("Replay event updated.");
		emit_websocket_event("EventListChanged");
	}
	refresh_events_list();
}

void delete_selected_replay_events()
{
	if (!replay_session || !events_table)
		return;
	QVector<int> indices;
	for (const QModelIndex &model_index : events_table->selectionModel()->selectedRows()) {
		QTableWidgetItem *number = events_table->item(model_index.row(), 0);
		bool ok = false;
		const int event_index = number ? number->data(Qt::UserRole).toInt(&ok) : -1;
		if (ok)
			indices.append(event_index);
	}
	if (indices.isEmpty())
		return;
	std::sort(indices.begin(), indices.end(), std::greater<int>());
	bool removed_any = false;
	for (const int event_index : indices) {
		QString error;
		if (!replay_session->removeEvent(event_index, &error)) {
			playout_status->setText(error);
			break;
		}
		removed_any = true;
	}
	if (removed_any)
		emit_websocket_event("EventListChanged");
	refresh_events_list();
}

void mark_replay_event(int seconds_back)
{
	if (!replay_session || !source_capture) {
		recording_status->setText("Start recording before marking a replay event.");
		return;
	}

	const obs_replays::TimelineUs now =
		static_cast<obs_replays::TimelineUs>(source_capture->timelineUs());
	replay_session->updateLiveTimeline(now);
	const obs_replays::TimelineUs in_us = qMax<obs_replays::TimelineUs>(0, now - seconds_back * 1000000LL);
	if (now <= in_us) {
		recording_status->setText("Wait for replay capture to collect enough video before marking an event.");
		return;
	}
	const int event_number = static_cast<int>(replay_session->events().size()) + 1;
	QString error;
	if (!replay_session->addEvent(in_us, now, QString("Event %1").arg(event_number), &error)) {
		recording_status->setText(error);
		return;
	}
	refresh_events_list();
	emit_websocket_event("EventListChanged");
	recording_status->setText(QString("Replay event marked: last %1 seconds.").arg(seconds_back));
}

void update_recording_session()
{
	if (!replay_session || !source_capture || !segment_writer)
		return;

	const obs_replays::TimelineUs timeline_us =
		static_cast<obs_replays::TimelineUs>(source_capture->timelineUs());
	replay_session->updateLiveTimeline(timeline_us);
	if (segment_writer->hasStopped()) {
		QString error;
		replay_session->stop(&error);
		segment_writer->release();
		source_capture->stop();
		recording_timer->stop();
		set_recording_controls(false);
		recording_status->setText(error.isEmpty() ? "Recording take stopped."
								 : error);
		segment_writer.reset();
		source_capture.reset();
		recording_stop_requested_ns = 0;
		recording_force_stop_issued = false;
		emit_websocket_event("RecordingStopped");
		return;
	}

	if (recording_stop_requested_ns && !recording_force_stop_issued &&
	    os_gettime_ns() - recording_stop_requested_ns >= 5000000000ULL) {
		recording_force_stop_issued = true;
		segment_writer->forceStop();
		recording_status->setText("Recording finalization timed out; forcing output shutdown…");
		return;
	}

	recording_status->setText(QString("Recording: %1 frames, %2 audio samples, %3")
					 .arg(source_capture->capturedVideoFrames())
					 .arg(source_capture->capturedAudioFrames())
					 .arg(format_recording_duration(timeline_us)));
}

void start_recording_session()
{
	if (replay_session && replay_session->isActive()) {
		recording_status->setText("A replay recording take is already active.");
		return;
	}

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
	if (replay_session && !replay_session->isActive()) {
		const QString current_folder = QFileInfo(replay_session->sessionDirectory()).absoluteDir().absolutePath();
		if (QDir::cleanPath(current_folder) != QDir::cleanPath(QDir(configuration.replayFolder).absolutePath()))
			replay_session.reset();
	}

	std::unique_ptr<obs_replays::ReplaySession> new_session;
	if (!replay_session)
		new_session = std::make_unique<obs_replays::ReplaySession>();
	auto *session = replay_session ? replay_session.get() : new_session.get();
	QString error;
	if (!session || !session->start(configuration, &error)) {
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
	if (!writer->start(capture->videoOutput(), capture->audioOutput(), session->recordingPath(),
			   configuration.videoBitrateMbps, configuration.audioBitrateKbps, &error)) {
		capture->stop();
		session->stop(&error);
		recording_status->setText(error);
		return;
	}

	if (new_session)
		replay_session = std::move(new_session);
	source_capture = std::move(capture);
	segment_writer = std::move(writer);
	recording_stop_requested_ns = 0;
	recording_force_stop_issued = false;
	refresh_events_list();
	set_recording_controls(true);
	recording_timer->start();
	recording_status->setText("Starting replay recording take…");
	emit_websocket_event("RecordingStarted");
}

void stop_recording_session()
{
	if (!segment_writer || !segment_writer->isActive()) {
		recording_status->setText("There is no active replay recording take.");
		return;
	}
	stop_recording_button->setEnabled(false);
	recording_stop_requested_ns = os_gettime_ns();
	recording_force_stop_issued = false;
	recording_status->setText("Finalizing replay MP4 recording…");
	segment_writer->stop();
}

void close_recording_session_for_shutdown()
{
	if (!replay_session || !replay_session->isActive())
		return;

	if (!module_unloading && recording_timer)
		recording_timer->stop();

	bool writer_stopped = true;
	if (segment_writer) {
		segment_writer->stop();
		const uint64_t deadline = os_gettime_ns() + 5000000000ULL;
		while (!segment_writer->hasStopped() && os_gettime_ns() < deadline)
			os_sleep_ms(10);
		if (!segment_writer->hasStopped()) {
			obs_log(LOG_WARNING,
				"OBS Replays: forcing replay output shutdown after finalization timeout.");
			segment_writer->forceStop();
			const uint64_t forced_deadline = os_gettime_ns() + 1000000000ULL;
			while (!segment_writer->hasStopped() && os_gettime_ns() < forced_deadline)
				os_sleep_ms(10);
		}
		writer_stopped = segment_writer->hasStopped();
		if (writer_stopped && source_capture) {
			replay_session->updateLiveTimeline(source_capture->timelineUs());
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
	emit_websocket_event("PlayoutStopped");
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
	event_playout_remaining_media_milliseconds = item.durationMilliseconds;
	if (replay_playout_index == 0)
		event_playout_remaining_media_milliseconds += first_event_intro_lead_milliseconds;
	event_playout_remaining_media_milliseconds =
		std::max(0.0, event_playout_remaining_media_milliseconds);
	event_playout_last_tick_ns = os_gettime_ns();
	if (event_playout_timer)
		event_playout_timer->start(20);
	playout_status->setText(QString("Playing %1 (%2 of %3).").arg(item.label)
				 .arg(replay_playout_index + 1)
				 .arg(replay_playout_queue.size()));
	if (replay_playout_index + 1 < replay_playout_queue.size()) {
		const ReplayPlayoutItem &next = replay_playout_queue.at(replay_playout_index + 1);
		auto cue_next = [next]() {
			if (!active_replay_playback_source)
				return;
			auto *replay_channel = obs_replays::ReplayChannelSource::fromSource(
				active_replay_playback_source);
			QString error;
			if (!replay_channel ||
			    !replay_channel->cueNext(next.segmentPath, next.inMilliseconds, &error))
				playout_status->setText(error.isEmpty() ? "The next replay event could not be cued."
								       : error);
		};
		const qint64 fade_duration = between_events_fade_duration_milliseconds();
		if (replay_playout_index > 0 && fade_duration > 0) {
			const uint64_t generation = playback_generation;
			const int event_index = replay_playout_index;
			QTimer::singleShot(static_cast<int>(fade_duration + 50),
					       [cue_next, generation, event_index]() {
						       if (!module_unloading && generation == playback_generation &&
							   event_index == replay_playout_index)
							       cue_next();
					       });
		} else {
			cue_next();
		}
	}
}

bool parse_timeline(const QString &text, obs_replays::TimelineUs *timestamp_us)
{
	const QString value = text.trimmed();
	if (value.isEmpty())
		return false;

	const QStringList fields = value.split(':');
	double seconds = 0.0;
	if (fields.size() == 1) {
		bool ok = false;
		seconds = fields.at(0).toDouble(&ok);
		if (!ok)
			return false;
	} else if (fields.size() == 2 || fields.size() == 3) {
		bool hours_ok = true;
		bool minutes_ok = true;
		const qint64 hours = fields.size() == 3 ? fields.at(0).toLongLong(&hours_ok) : 0;
		const qint64 minutes = fields.at(fields.size() - 2).toLongLong(&minutes_ok);
		bool seconds_ok = false;
		const double final_seconds = fields.last().toDouble(&seconds_ok);
		if (!hours_ok || !minutes_ok || !seconds_ok || hours < 0 || minutes < 0 ||
		    minutes >= 60 || final_seconds < 0.0 || final_seconds >= 60.0)
			return false;
		seconds = hours * 3600.0 + minutes * 60.0 + final_seconds;
	} else {
		return false;
	}

	if (!std::isfinite(seconds) || seconds < 0.0)
		return false;
	*timestamp_us = static_cast<obs_replays::TimelineUs>(std::llround(seconds * 1000000.0));
	return true;
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
		if (!channel || !channel->takeCued(
				between_events_fade_duration_milliseconds(), &error)) {
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

bool play_replay_event_indices(const QVector<int> &event_indices, QString *error)
{
	if (!replay_session) {
		*error = "There is no replay session open.";
		return false;
	}
	if (event_indices.isEmpty()) {
		*error = "Select one or more replay events first.";
		return false;
	}

	QVector<ReplayPlayoutItem> queue;
	for (const int event_index : event_indices) {
		if (event_index < 0 || event_index >= replay_session->events().size()) {
			*error = "A requested replay event is no longer available.";
			return false;
		}
		const obs_replays::ReplayEvent &event = replay_session->events().at(event_index);
		const QString recording_path = replay_session->recordingPath(event.takeId);
		if (!QFileInfo::exists(recording_path)) {
			*error = "The recording take for a requested replay event is not available.";
			return false;
		}
		queue.append({event.id.toString(QUuid::WithoutBraces), recording_path, event.inUs / 1000,
			      (event.outUs - event.inUs) / 1000,
			      event.label.isEmpty() ? "Replay event" : event.label});
	}

	if (!take_replay_to_program(error))
		return false;
	update_playout_button();
	replay_playout_queue = queue;
	replay_playout_index = -1;
	++playback_generation;
	if (auto *channel = obs_replays::ReplayChannelSource::fromSource(active_replay_playback_source)) {
		channel->reset();
		channel->setPlaybackSpeed(playback_speed_percent());
	}
	const bool started = play_next_replay_event(error);
	if (started)
		emit_websocket_event("PlayoutStarted");
	return started;
}

void play_selected_replay_events()
{
	if (!events_table || events_table->selectionModel()->selectedRows().isEmpty()) {
		playout_status->setText("Select one or more replay events first.");
		return;
	}

	QVector<int> selected_event_indices;
	for (const QModelIndex &model_index : events_table->selectionModel()->selectedRows()) {
		QTableWidgetItem *selected_item = events_table->item(model_index.row(), 0);
		bool has_event_index = false;
		const int event_index = selected_item ? selected_item->data(Qt::UserRole).toInt(&has_event_index) : -1;
		if (!has_event_index) {
			playout_status->setText("The selected replay event is no longer available.");
			return;
		}
		selected_event_indices.append(event_index);
	}
	std::sort(selected_event_indices.begin(), selected_event_indices.end());
	QString error;
	if (!play_replay_event_indices(selected_event_indices, &error))
		playout_status->setText(error);
}

bool stop_replay_playout(QString *error)
{
	if (outro_cleanup_pending) {
		*error = "Replay playout is already ending.";
		return false;
	}
	if (!active_replay_scene) {
		*error = "There is no active replay playout.";
		return false;
	}
	const bool stopping = return_to_previous_program(error);
	if (stopping)
		emit_websocket_event("PlayoutStopping");
	return stopping;
}

int replay_event_index_from_id(const QString &id)
{
	if (!replay_session)
		return -1;
	const QUuid requested_id(id);
	if (requested_id.isNull())
		return -1;
	for (qsizetype index = 0; index < replay_session->events().size(); ++index) {
		if (replay_session->events().at(index).id == requested_id)
			return static_cast<int>(index);
	}
	return -1;
}

void write_websocket_success(obs_data_t *response)
{
	obs_data_set_int(response, "protocolVersion", 1);
	obs_data_set_bool(response, "success", true);
}

void write_websocket_error(obs_data_t *response, const QString &error)
{
	obs_data_set_int(response, "protocolVersion", 1);
	obs_data_set_bool(response, "success", false);
	set_websocket_string(response, "error", error);
}

void write_get_playout_status_response(obs_data_t *, obs_data_t *response)
{
	write_websocket_success(response);
	const bool active = active_replay_scene != nullptr;
	obs_data_set_bool(response, "active", active);
	obs_data_set_bool(response, "ending", outro_cleanup_pending);
	obs_data_set_int(response, "playbackRatePercent", playback_speed_percent());
	obs_data_set_int(response, "queueLength", replay_playout_queue.size());
	obs_data_set_int(response, "currentQueueIndex", replay_playout_index + 1);
	if (replay_playout_index >= 0 && replay_playout_index < replay_playout_queue.size()) {
		set_websocket_string(response, "currentEventId", replay_playout_queue.at(replay_playout_index).eventId);
		set_websocket_string(response, "currentLabel", replay_playout_queue.at(replay_playout_index).label);
	}
}

void write_get_event_response(obs_data_t *request, obs_data_t *response)
{
	const QString id = QString::fromUtf8(obs_data_get_string(request, "eventId"));
	const int index = replay_event_index_from_id(id);
	if (index < 0) {
		write_websocket_error(response, "The requested replay event was not found.");
		return;
	}
	write_websocket_success(response);
	obs_data_t *event = obs_data_create();
	write_websocket_event(event, replay_session->events().at(index), index);
	obs_data_set_obj(response, "event", event);
	obs_data_release(event);
}

void write_start_recording_response(obs_data_t *, obs_data_t *response)
{
	start_recording_session();
	if (!replay_session || !replay_session->isActive() || !segment_writer || !segment_writer->isActive()) {
		write_websocket_error(response, recording_status ? recording_status->text()
								 : "OBS could not start replay recording.");
		return;
	}
	write_websocket_success(response);
	write_websocket_recording_status(response);
}

void write_stop_recording_response(obs_data_t *, obs_data_t *response)
{
	if (!segment_writer || !segment_writer->isActive()) {
		write_websocket_error(response, "There is no active replay recording take.");
		return;
	}
	stop_recording_session();
	write_websocket_success(response);
	obs_data_set_bool(response, "stopping", true);
	write_websocket_recording_status(response);
}

void write_create_event_response(obs_data_t *request, obs_data_t *response)
{
	if (!replay_session || !replay_session->isActive() || !source_capture) {
		write_websocket_error(response, "Start recording before creating a replay event.");
		return;
	}
	if (!obs_data_has_user_value(request, "inMs") || !obs_data_has_user_value(request, "outMs")) {
		write_websocket_error(response, "CreateEvent requires inMs and outMs.");
		return;
	}
	const qint64 in_ms = obs_data_get_int(request, "inMs");
	const qint64 out_ms = obs_data_get_int(request, "outMs");
	if (in_ms < 0 || out_ms <= in_ms) {
		write_websocket_error(response, "The replay event range is invalid.");
		return;
	}
	replay_session->updateLiveTimeline(static_cast<obs_replays::TimelineUs>(source_capture->timelineUs()));
	const QString label = obs_data_has_user_value(request, "label")
			      ? QString::fromUtf8(obs_data_get_string(request, "label"))
			      : QString("Event %1").arg(replay_session->events().size() + 1);
	QString error;
	if (!replay_session->addEvent(in_ms * 1000, out_ms * 1000, label, &error)) {
		write_websocket_error(response, error);
		return;
	}
	refresh_events_list();
	emit_websocket_event("EventListChanged");
	write_websocket_success(response);
	obs_data_t *event = obs_data_create();
	write_websocket_event(event, replay_session->events().last(), replay_session->events().size() - 1);
	obs_data_set_obj(response, "event", event);
	obs_data_release(event);
}

void write_create_event_from_live_response(obs_data_t *request, obs_data_t *response)
{
	if (!replay_session || !replay_session->isActive() || !source_capture) {
		write_websocket_error(response, "Start recording before creating a replay event.");
		return;
	}
	if (!obs_data_has_user_value(request, "preRollMs")) {
		write_websocket_error(response, "CreateEventFromLive requires preRollMs.");
		return;
	}
	const qint64 pre_roll_ms = obs_data_get_int(request, "preRollMs");
	if (pre_roll_ms <= 0) {
		write_websocket_error(response, "preRollMs must be greater than zero.");
		return;
	}
	const obs_replays::TimelineUs now = static_cast<obs_replays::TimelineUs>(source_capture->timelineUs());
	replay_session->updateLiveTimeline(now);
	const obs_replays::TimelineUs in = qMax<obs_replays::TimelineUs>(0, now - pre_roll_ms * 1000);
	if (now <= in) {
		write_websocket_error(response, "The recording has not yet reached the requested event duration.");
		return;
	}
	const QString label = obs_data_has_user_value(request, "label")
			      ? QString::fromUtf8(obs_data_get_string(request, "label"))
			      : QString("Event %1").arg(replay_session->events().size() + 1);
	QString error;
	if (!replay_session->addEvent(in, now, label, &error)) {
		write_websocket_error(response, error);
		return;
	}
	refresh_events_list();
	emit_websocket_event("EventListChanged");
	write_websocket_success(response);
	obs_data_t *event = obs_data_create();
	write_websocket_event(event, replay_session->events().last(), replay_session->events().size() - 1);
	obs_data_set_obj(response, "event", event);
	obs_data_release(event);
}

void write_update_event_response(obs_data_t *request, obs_data_t *response)
{
	const QString id = QString::fromUtf8(obs_data_get_string(request, "eventId"));
	const int index = replay_event_index_from_id(id);
	if (index < 0) {
		write_websocket_error(response, "The requested replay event was not found.");
		return;
	}
	if (!obs_data_has_user_value(request, "inMs") || !obs_data_has_user_value(request, "outMs")) {
		write_websocket_error(response, "UpdateEvent requires inMs and outMs.");
		return;
	}
	const qint64 in_ms = obs_data_get_int(request, "inMs");
	const qint64 out_ms = obs_data_get_int(request, "outMs");
	const QString label = obs_data_has_user_value(request, "label")
			      ? QString::fromUtf8(obs_data_get_string(request, "label"))
			      : replay_session->events().at(index).label;
	QString error;
	if (!replay_session->updateEvent(index, in_ms * 1000, out_ms * 1000, label, &error)) {
		write_websocket_error(response, error);
		return;
	}
	refresh_events_list();
	emit_websocket_event("EventListChanged");
	write_websocket_success(response);
	obs_data_t *event = obs_data_create();
	write_websocket_event(event, replay_session->events().at(index), index);
	obs_data_set_obj(response, "event", event);
	obs_data_release(event);
}

void write_delete_event_response(obs_data_t *request, obs_data_t *response)
{
	const QString id = QString::fromUtf8(obs_data_get_string(request, "eventId"));
	const int index = replay_event_index_from_id(id);
	if (index < 0) {
		write_websocket_error(response, "The requested replay event was not found.");
		return;
	}
	QString error;
	if (!replay_session->removeEvent(index, &error)) {
		write_websocket_error(response, error);
		return;
	}
	refresh_events_list();
	emit_websocket_event("EventListChanged");
	write_websocket_success(response);
	set_websocket_string(response, "eventId", id);
}

void write_start_playout_response(obs_data_t *request, obs_data_t *response)
{
	if (active_replay_scene || outro_cleanup_pending) {
		write_websocket_error(response, "Replay playout is already active.");
		return;
	}
	const QString play_order = obs_data_has_user_value(request, "playOrder")
				       ? QString::fromUtf8(obs_data_get_string(request, "playOrder"))
				       : "creation";
	if (play_order != "creation" && play_order != "provided") {
		write_websocket_error(response, "playOrder must be 'creation' or 'provided'.");
		return;
	}
	obs_data_array_t *event_ids = obs_data_get_array(request, "eventIds");
	if (!event_ids || obs_data_array_count(event_ids) == 0) {
		if (event_ids)
			obs_data_array_release(event_ids);
		write_websocket_error(response, "StartPlayout requires one or more eventIds.");
		return;
	}
	QVector<int> event_indices;
	for (size_t index = 0; index < obs_data_array_count(event_ids); ++index) {
		obs_data_t *item = obs_data_array_item(event_ids, index);
		const int event_index = item ? replay_event_index_from_id(
			QString::fromUtf8(obs_data_get_string(item, "eventId"))) : -1;
		if (item)
			obs_data_release(item);
		if (event_index < 0) {
			obs_data_array_release(event_ids);
			write_websocket_error(response, "A requested replay event was not found.");
			return;
		}
		event_indices.append(event_index);
	}
	obs_data_array_release(event_ids);
	// Treat IDs as a selected set by default, consistent with the dock. A
	// controller can explicitly request the supplied sequence when it owns an
	// intentional editorial ordering.
	if (play_order == "creation")
		std::sort(event_indices.begin(), event_indices.end());
	QString error;
	if (!play_replay_event_indices(event_indices, &error)) {
		write_websocket_error(response, error);
		return;
	}
	write_get_playout_status_response(nullptr, response);
}

void write_stop_playout_response(obs_data_t *, obs_data_t *response)
{
	QString error;
	if (!stop_replay_playout(&error)) {
		write_websocket_error(response, error);
		return;
	}
	write_websocket_success(response);
	obs_data_set_bool(response, "stopping", true);
}

void write_set_playout_rate_response(obs_data_t *request, obs_data_t *response)
{
	if (!obs_data_has_user_value(request, "ratePercent")) {
		write_websocket_error(response, "SetPlayoutRate requires ratePercent.");
		return;
	}
	const int rate = static_cast<int>(obs_data_get_int(request, "ratePercent"));
	if (rate < 10 || rate > 100) {
		write_websocket_error(response, "ratePercent must be between 10 and 100.");
		return;
	}
	if (playback_speed_selector)
		playback_speed_selector->setValue(rate);
	else
		update_playback_speed(rate);
	emit_websocket_event("PlayoutRateChanged");
	write_websocket_success(response);
	obs_data_set_int(response, "ratePercent", rate);
}

constexpr const char *websocket_request_types[] = {
	"GetSession", "GetRecordingStatus", "GetPlayoutStatus", "ListEvents", "GetEvent",
	"StartRecording", "StopRecording", "CreateEvent", "CreateEventFromLive", "UpdateEvent",
	"DeleteEvent", "StartPlayout", "StopPlayout", "SetPlayoutRate",
};
WebsocketResponseWriter websocket_response_writers[] = {
	write_get_session_response,
	write_get_recording_status_response,
	write_get_playout_status_response,
	write_list_events_response,
	write_get_event_response,
	write_start_recording_response,
	write_stop_recording_response,
	write_create_event_response,
	write_create_event_from_live_response,
	write_update_event_response,
	write_delete_event_response,
	write_start_playout_response,
	write_stop_playout_response,
	write_set_playout_rate_response,
};

void reopen_saved_replay_session()
{
	if (replay_session || !replay_folder_selector || replay_folder_selector->text().isEmpty())
		return;
	obs_source_t *source = source_from_selector(source_selector);
	if (!source)
		return;
	obs_replays::SessionConfiguration configuration;
	configuration.replayFolder = replay_folder_selector->text();
	configuration.sourceName = QString::fromUtf8(obs_source_get_name(source));
	configuration.sourceUuid = QString::fromUtf8(obs_source_get_uuid(source));
	configuration.videoBitrateMbps = video_bitrate_selector->value();
	configuration.audioBitrateKbps = audio_bitrate_selector->value();
	obs_source_release(source);

	auto session = std::make_unique<obs_replays::ReplaySession>();
	QString error;
	if (!session->open(configuration, &error))
		return;
	replay_session = std::move(session);
	emit_websocket_event("SessionChanged");
	recording_status->setText(QString("Reopened replay session with %1 event%2 and %3 take%4.")
					 .arg(replay_session->events().size())
					 .arg(replay_session->events().size() == 1 ? "" : "s")
					 .arg(replay_session->takes().size())
					 .arg(replay_session->takes().size() == 1 ? "" : "s"));
}

void change_replay_folder()
{
	if (!replay_folder_selector)
		return;
	if (replay_session && replay_session->isActive()) {
		const QString active_folder = QFileInfo(replay_session->sessionDirectory()).absoluteDir().absolutePath();
		const QSignalBlocker blocker(replay_folder_selector);
		replay_folder_selector->setText(active_folder);
		update_storage_status();
		recording_status->setText("Stop recording before changing the replay folder.");
		return;
	}

	clear_playout_state();
	replay_session.reset();
	reopen_saved_replay_session();
	refresh_events_list();
	emit_websocket_event("SessionChanged");
	if (!replay_session)
		recording_status->setText("No replay session in the selected folder.");
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		flush_scheduled_settings_save();
		close_recording_session_for_shutdown();
		return;
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		if (settings_save_timer)
			settings_save_timer->stop();
		clear_playout_state();
		refresh_source_selector();
		refresh_playout_selectors();
		load_settings();
		if (!replay_session || !replay_session->isActive()) {
			replay_session.reset();
			reopen_saved_replay_session();
			refresh_events_list();
		}
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
	dock->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	dock->setMinimumWidth(360);
	auto *content = new QWidget(dock);
	content->setMinimumWidth(360);
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
	capture_settings_group = capture_group;
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
	choose_replay_folder_button = new QPushButton("Browse", replay_folder_row);
	QObject::connect(choose_replay_folder_button, &QPushButton::clicked, []() {
		const QString folder = QFileDialog::getExistingDirectory(
			replay_dock, "Choose replay folder", replay_folder_selector->text(),
			QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (!folder.isEmpty())
			replay_folder_selector->setText(QDir::toNativeSeparators(folder));
	});
	replay_folder_layout->addWidget(choose_replay_folder_button);
	capture_form->addRow("Replay folder", replay_folder_row);

	video_bitrate_selector = new DockSpinBox(capture_group);
	video_bitrate_selector->setRange(1, 250);
	video_bitrate_selector->setValue(25);
	video_bitrate_selector->setSuffix(" Mb/s");
	capture_form->addRow("Target video bitrate", video_bitrate_selector);

	audio_bitrate_selector = new DockSpinBox(capture_group);
	audio_bitrate_selector->setRange(32, 512);
	audio_bitrate_selector->setValue(160);
	audio_bitrate_selector->setSuffix(" kb/s");
	capture_form->addRow("Target audio bitrate", audio_bitrate_selector);

	storage_status = new QLabel(capture_group);
	storage_status->setWordWrap(true);
	capture_form->addRow("Storage", storage_status);
	auto *clear_replay_folder_button = new QPushButton("Clear replay folder", capture_group);
	QObject::connect(clear_replay_folder_button, &QPushButton::clicked,
			 []() { clear_replay_folder(); });
	capture_form->addRow(QString(), clear_replay_folder_button);
	QObject::connect(replay_folder_selector, &QLineEdit::textChanged,
			 [](const QString &) { update_storage_status(); });
	QObject::connect(replay_folder_selector, &QLineEdit::textChanged,
			 [](const QString &) { change_replay_folder(); });
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

	auto *refresh_sources_row = new QWidget(capture_group);
	auto *refresh_sources_layout = new QHBoxLayout(refresh_sources_row);
	refresh_sources_layout->setContentsMargins(0, 0, 0, 0);
	auto *refresh_sources = new QPushButton("Refresh sources", refresh_sources_row);
	QObject::connect(refresh_sources, &QPushButton::clicked,
			 []() { refresh_source_selector(); });
	refresh_sources_layout->addStretch();
	refresh_sources_layout->addWidget(refresh_sources);
	refresh_sources_layout->addStretch();
	capture_form->addRow(refresh_sources_row);
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
	between_events_transition_selector = new QComboBox(playout_group);
	between_events_transition_selector->addItem("Cut", "cut");
	between_events_transition_selector->addItem("Fade", "fade");
	between_events_transition_selector->setCurrentIndex(1);
	playout_form->addRow("Between replay events", between_events_transition_selector);
	between_events_fade_duration_selector = new DockSpinBox(playout_group);
	between_events_fade_duration_selector->setRange(50, 2000);
	between_events_fade_duration_selector->setValue(150);
	between_events_fade_duration_selector->setSuffix(" ms");
	playout_form->addRow("Event fade duration", between_events_fade_duration_selector);
	auto *refresh_playout_row = new QWidget(playout_group);
	auto *refresh_playout_layout = new QHBoxLayout(refresh_playout_row);
	refresh_playout_layout->setContentsMargins(0, 0, 0, 0);
	auto *refresh_playout = new QPushButton("Refresh scenes/transitions", refresh_playout_row);
	QObject::connect(refresh_playout, &QPushButton::clicked,
			 []() {
				 refresh_playout_selectors();
			 });
	refresh_playout_layout->addStretch();
	refresh_playout_layout->addWidget(refresh_playout);
	refresh_playout_layout->addStretch();
	playout_form->addRow(refresh_playout_row);
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
			if (!stop_replay_playout(&error))
				playout_status->setText(error);
			return;
		}
		play_selected_replay_events();
	});
	events_layout->addWidget(play_events_button);
	auto *speed_row = new QWidget(events_group);
	auto *speed_layout = new QHBoxLayout(speed_row);
	speed_layout->setContentsMargins(0, 0, 0, 0);
	auto *speed_label = new QLabel("Playback speed", speed_row);
	playback_speed_selector = new DockSlider(Qt::Horizontal, speed_row);
	playback_speed_selector->setRange(10, 100);
	playback_speed_selector->setValue(100);
	playback_speed_selector->setTickInterval(10);
	playback_speed_selector->setTickPosition(QSlider::TicksBelow);
	playback_speed_value = new QLabel("100%", speed_row);
	speed_layout->addWidget(speed_label);
	speed_layout->addWidget(playback_speed_selector);
	speed_layout->addWidget(playback_speed_value);
	events_layout->addWidget(speed_row);
	QObject::connect(playback_speed_selector, &QSlider::valueChanged,
			 [](int percent) { update_playback_speed(percent); });
	auto *events_splitter = new EventTableSplitter(events_group);
	events_table = new QTableWidget(events_splitter);
	events_table->setColumnCount(4);
	events_table->setHorizontalHeaderLabels({"#", "In", "Out", "Label"});
	events_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	events_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
	events_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
					      QAbstractItemView::SelectedClicked);
	events_table->verticalHeader()->setVisible(false);
	events_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	events_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	events_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	events_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	// Keep enough room for the header and one row, while leaving the splitter
	// free to make this table substantially smaller when dock space is needed.
	events_table->setMinimumHeight(80);
	QObject::connect(events_table, &QTableWidget::cellChanged,
			 [](int row, int column) { update_replay_event_from_table(row, column); });
	events_splitter->addWidget(events_table);

	auto *event_controls = new QWidget(events_splitter);
	auto *event_controls_layout = new QVBoxLayout(event_controls);
	event_controls_layout->setContentsMargins(0, 0, 0, 0);
	auto *mark_buttons_row = new QWidget(event_controls);
	auto *mark_buttons_layout = new QHBoxLayout(mark_buttons_row);
	mark_buttons_layout->setContentsMargins(0, 0, 0, 0);
	for (const int seconds_back : {10, 5, 3, 2, 1}) {
		auto *button = new QPushButton(QString("-%1").arg(seconds_back), mark_buttons_row);
		button->setEnabled(false);
		button->setToolTip(QString("Mark an event from %1 seconds ago until now.").arg(seconds_back));
		QObject::connect(button, &QPushButton::clicked,
				 [seconds_back]() { mark_replay_event(seconds_back); });
		mark_event_buttons.append(button);
		mark_buttons_layout->addWidget(button);
	}
	event_controls_layout->addWidget(mark_buttons_row);
	delete_events_button = new QPushButton("Delete selected events", event_controls);
	QObject::connect(delete_events_button, &QPushButton::clicked,
			 []() { delete_selected_replay_events(); });
	event_controls_layout->addWidget(delete_events_button);
	events_splitter->addWidget(event_controls);
	events_splitter->setCollapsible(0, false);
	events_splitter->setCollapsible(1, false);
	events_splitter->setStretchFactor(0, 1);
	events_splitter->setSizes({260, 70});
	events_layout->addWidget(events_splitter, 1);
	event_playout_timer = new QTimer(events_group);
	event_playout_timer->setInterval(20);
	QObject::connect(event_playout_timer, &QTimer::timeout,
			 []() { update_event_playout_timer(); });
	// Let the event table own any spare dock height so its splitter can grow
	// upward as well as shrink. A trailing layout stretch would consume it.
	layout->addWidget(events_group, 1);

	refresh_source_selector();
	refresh_playout_selectors();
	load_settings();
	reopen_saved_replay_session();
	update_storage_status();
	refresh_events_list();
	update_playout_button();
	settings_save_timer = new QTimer(dock);
	settings_save_timer->setSingleShot(true);
	QObject::connect(settings_save_timer, &QTimer::timeout, []() { save_settings(); });
	QObject::connect(source_selector, &QComboBox::currentTextChanged,
			 [](const QString &) { schedule_settings_save(); });
	QObject::connect(replay_folder_selector, &QLineEdit::textChanged,
			 [](const QString &) { schedule_settings_save(); });
	auto save_spinbox = [](QSpinBox *selector) {
		QObject::connect(selector, QOverload<int>::of(&QSpinBox::valueChanged),
				 [](int) { schedule_settings_save(); });
	};
	save_spinbox(video_bitrate_selector);
	save_spinbox(audio_bitrate_selector);
	save_spinbox(between_events_fade_duration_selector);
	QObject::connect(replay_scene_selector, &QComboBox::currentTextChanged,
			 [](const QString &) { schedule_settings_save(); });
	QObject::connect(intro_transition_selector, &QComboBox::currentTextChanged,
			 [](const QString &) { schedule_settings_save(); });
	QObject::connect(outro_transition_selector, &QComboBox::currentTextChanged,
			 [](const QString &) { schedule_settings_save(); });
	QObject::connect(between_events_transition_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
			 [](int) { schedule_settings_save(); });
	dock->setWidget(content);
	return dock;
}

} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	module_unloading = false;
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

void obs_module_post_load(void)
{
	websocket_vendor = obs_replays_websocket_register_vendor("obs-replays");
	if (!websocket_vendor) {
		obs_log(LOG_INFO, "OBS Replays: obs-websocket Vendor API unavailable; external control is disabled.");
		return;
	}

	for (size_t index = 0; index < std::size(websocket_request_types); ++index) {
		if (!obs_replays_websocket_register_request(websocket_vendor, websocket_request_types[index],
							    websocket_request_callback,
							    &websocket_response_writers[index])) {
			obs_log(LOG_WARNING, "OBS Replays: could not register obs-websocket request '%s'.",
				websocket_request_types[index]);
		}
	}
	obs_log(LOG_INFO, "OBS Replays: obs-websocket vendor API registered.");
}

void obs_module_unload(void)
{
	flush_scheduled_settings_save();
	// obs-websocket can be unloaded before this module during OBS shutdown. Its
	// Vendor API has no vendor-unregister operation and owns vendor lifetime, so
	// calling a cached process handler here can dereference an already destroyed
	// mutex. Stop future emits locally and let obs-websocket release its vendor.
	websocket_vendor = nullptr;
	module_unloading = true;
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	close_recording_session_for_shutdown();
	clear_playout_state();
	if (replay_dock)
		obs_frontend_remove_dock(dock_id);
	replay_dock = nullptr;
	capture_settings_group = nullptr;
	source_selector = nullptr;
	replay_folder_selector = nullptr;
	choose_replay_folder_button = nullptr;
	video_bitrate_selector = nullptr;
	audio_bitrate_selector = nullptr;
	replay_scene_selector = nullptr;
	intro_transition_selector = nullptr;
	outro_transition_selector = nullptr;
	between_events_transition_selector = nullptr;
	between_events_fade_duration_selector = nullptr;
	playback_speed_selector = nullptr;
	playback_speed_value = nullptr;
	playout_status = nullptr;
	recording_status = nullptr;
	storage_status = nullptr;
	start_recording_button = nullptr;
	stop_recording_button = nullptr;
	mark_event_buttons.clear();
	delete_events_button = nullptr;
	play_events_button = nullptr;
	events_table = nullptr;
	recording_timer = nullptr;
	settings_save_timer = nullptr;
	event_playout_timer = nullptr;
	obs_log(LOG_INFO, "OBS Replays unloaded");
}
