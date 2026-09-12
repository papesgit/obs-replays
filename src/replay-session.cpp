#include "replay-session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

namespace obs_replays {
namespace {

QJsonObject toJson(const ReplayEvent &event)
{
	return {{"id", event.id.toString(QUuid::WithoutBraces)},
		{"takeId", event.takeId.toString(QUuid::WithoutBraces)},
		{"label", event.label},
		{"inUs", QString::number(event.inUs)},
		{"outUs", QString::number(event.outUs)},
		{"createdAtUtc", event.createdAtUtc.toString(Qt::ISODateWithMs)}};
}

QJsonObject toJson(const ReplayTake &take)
{
	return {{"id", take.id.toString(QUuid::WithoutBraces)},
		{"path", take.recordingRelativePath},
		{"durationUs", QString::number(take.durationUs)},
		{"startedAtUtc", take.startedAtUtc.toString(Qt::ISODateWithMs)},
		{"stoppedAtUtc", take.stoppedAtUtc.toString(Qt::ISODateWithMs)}};
}

bool parseTimeline(const QJsonObject &object, const char *key, TimelineUs *value)
{
	bool ok = false;
	const TimelineUs parsed = object.value(key).toVariant().toLongLong(&ok);
	if (!ok || parsed < 0)
		return false;
	*value = parsed;
	return true;
}

} // namespace

bool ReplaySession::start(const SessionConfiguration &sessionConfiguration, QString *error)
{
	if (active) {
		*error = "A replay recording take is already active.";
		return false;
	}
	if (sessionConfiguration.replayFolder.isEmpty() || sessionConfiguration.sourceUuid.isEmpty()) {
		*error = "A replay folder and source are required to start recording.";
		return false;
	}
	if (directory.isEmpty()) {
		if (!openMostRecentSession(sessionConfiguration, error) && !createSession(sessionConfiguration, error))
			return false;
	} else if (configuration.sourceUuid != sessionConfiguration.sourceUuid) {
		*error = "This replay session belongs to a different source. Choose a new replay folder to begin a new session.";
		return false;
	}
	if (!QDir().mkpath(QDir(directory).filePath("takes"))) {
		*error = "Could not create the replay take folder.";
		return false;
	}

	ReplayTake take;
	take.id = QUuid::createUuid();
	take.recordingRelativePath = QString("takes/take-%1.mp4").arg(replayTakes.size() + 1, 3, 10, QLatin1Char('0'));
	take.startedAtUtc = QDateTime::currentDateTimeUtc();
	replayTakes.append(take);
	liveTimelineUs = 0;
	stoppedAtUtc = {};
	active = true;
	if (saveManifest(error))
		return true;
	replayTakes.removeLast();
	active = false;
	return false;
}

bool ReplaySession::open(const SessionConfiguration &sessionConfiguration, QString *error)
{
	if (active) {
		*error = "Cannot reopen a replay session while recording is active.";
		return false;
	}
	if (!directory.isEmpty())
		return configuration.sourceUuid == sessionConfiguration.sourceUuid;
	return openMostRecentSession(sessionConfiguration, error);
}

bool ReplaySession::stop(QString *error)
{
	if (!active) {
		*error = "No replay recording take is active.";
		return false;
	}
	if (ReplayTake *take = activeTake())
		take->stoppedAtUtc = QDateTime::currentDateTimeUtc();
	stoppedAtUtc = QDateTime::currentDateTimeUtc();
	active = false;
	return saveManifest(error);
}

void ReplaySession::updateLiveTimeline(TimelineUs timelineUs)
{
	if (!active || timelineUs <= liveTimelineUs)
		return;
	liveTimelineUs = timelineUs;
	if (ReplayTake *take = activeTake())
		take->durationUs = timelineUs;
}

bool ReplaySession::addEvent(TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error)
{
	ReplayTake *take = activeTake();
	if (!take || inUs < 0 || outUs <= inUs || outUs > take->durationUs) {
		*error = "The event range is outside the active recording take.";
		return false;
	}
	replayEvents.append({QUuid::createUuid(), take->id, label, inUs, outUs, QDateTime::currentDateTimeUtc()});
	return saveManifest(error);
}

bool ReplaySession::updateEvent(qsizetype index, TimelineUs inUs, TimelineUs outUs, const QString &label,
			QString *error)
{
	if (index < 0 || index >= replayEvents.size() || inUs < 0 || outUs <= inUs) {
		*error = "The replay event range is invalid.";
		return false;
	}
	ReplayEvent &event = replayEvents[index];
	const ReplayTake *take = findTake(event.takeId);
	if (!take || outUs > take->durationUs) {
		*error = "The event range is outside its recording take.";
		return false;
	}
	const ReplayEvent original = event;
	event.inUs = inUs;
	event.outUs = outUs;
	event.label = label;
	if (saveManifest(error))
		return true;
	event = original;
	return false;
}

bool ReplaySession::removeEvent(qsizetype index, QString *error)
{
	if (index < 0 || index >= replayEvents.size()) {
		*error = "The replay event no longer exists.";
		return false;
	}
	const ReplayEvent removed = replayEvents.takeAt(index);
	if (saveManifest(error))
		return true;
	replayEvents.insert(index, removed);
	return false;
}

bool ReplaySession::isActive() const { return active; }
TimelineUs ReplaySession::latestTimelineUs() const { return liveTimelineUs; }
QString ReplaySession::sessionId() const { return id; }
QUuid ReplaySession::activeTakeId() const
{
	const ReplayTake *take = activeTake();
	return take ? take->id : QUuid();
}
const SessionConfiguration &ReplaySession::sessionConfiguration() const { return configuration; }
const QString &ReplaySession::sessionDirectory() const { return directory; }

QString ReplaySession::recordingPath() const
{
	const ReplayTake *take = activeTake();
	return take ? recordingPath(take->id) : QString();
}

QString ReplaySession::recordingPath(const QUuid &takeId) const
{
	const ReplayTake *take = findTake(takeId);
	return take ? QDir(directory).filePath(take->recordingRelativePath) : QString();
}

const QList<ReplayEvent> &ReplaySession::events() const { return replayEvents; }
const QList<ReplayTake> &ReplaySession::takes() const { return replayTakes; }

bool ReplaySession::openMostRecentSession(const SessionConfiguration &sessionConfiguration, QString *error)
{
	const QDir replayFolder(sessionConfiguration.replayFolder);
	const QFileInfoList candidates = replayFolder.entryInfoList(QStringList{"OBS-Replay-*"}, QDir::Dirs | QDir::NoDotAndDotDot,
								   QDir::Time);
	for (const QFileInfo &candidate : candidates) {
		if (!loadManifest(QDir(candidate.absoluteFilePath()).filePath("session.json"), error))
			continue;
		if (configuration.sourceUuid == sessionConfiguration.sourceUuid)
			return true;
		directory.clear();
		replayEvents.clear();
		replayTakes.clear();
	}
	return false;
}

bool ReplaySession::createSession(const SessionConfiguration &sessionConfiguration, QString *error)
{
	configuration = sessionConfiguration;
	id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	startedAtUtc = QDateTime::currentDateTimeUtc();
	stoppedAtUtc = {};
	replayEvents.clear();
	replayTakes.clear();
	liveTimelineUs = 0;
	const QString sessionName = QString("OBS-Replay-%1-%2")
					    .arg(startedAtUtc.toString("yyyy-MM-dd_hh-mm-ss"), id.left(8));
	directory = QDir(configuration.replayFolder).filePath(sessionName);
	if (!QDir().mkpath(QDir(directory).filePath("takes"))) {
		*error = "Could not create the replay session folder.";
		directory.clear();
		return false;
	}
	return true;
}

bool ReplaySession::loadManifest(const QString &manifestPath, QString *error)
{
	QFile file(manifestPath);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	if (root.value("schemaVersion").toInt() != 2)
		return false;
	SessionConfiguration loadedConfiguration;
	const QJsonObject source = root.value("source").toObject();
	loadedConfiguration.sourceName = source.value("name").toString();
	loadedConfiguration.sourceUuid = source.value("uuid").toString();
	const QJsonObject encoding = root.value("encoding").toObject();
	loadedConfiguration.videoBitrateMbps = encoding.value("videoBitrateMbps").toInt(25);
	loadedConfiguration.audioBitrateKbps = encoding.value("audioBitrateKbps").toInt(160);
	QList<ReplayTake> loadedTakes;
	for (const QJsonValue &value : root.value("takes").toArray()) {
		const QJsonObject object = value.toObject();
		ReplayTake take;
		take.id = QUuid(object.value("id").toString());
		take.recordingRelativePath = object.value("path").toString();
		if (take.id.isNull() || take.recordingRelativePath.isEmpty() ||
		    !parseTimeline(object, "durationUs", &take.durationUs))
			return false;
		take.startedAtUtc = QDateTime::fromString(object.value("startedAtUtc").toString(), Qt::ISODateWithMs);
		take.stoppedAtUtc = QDateTime::fromString(object.value("stoppedAtUtc").toString(), Qt::ISODateWithMs);
		loadedTakes.append(take);
	}
	QList<ReplayEvent> loadedEvents;
	for (const QJsonValue &value : root.value("events").toArray()) {
		const QJsonObject object = value.toObject();
		ReplayEvent event;
		event.id = QUuid(object.value("id").toString());
		event.takeId = QUuid(object.value("takeId").toString());
		event.label = object.value("label").toString();
		if (event.id.isNull() || event.takeId.isNull() || !parseTimeline(object, "inUs", &event.inUs) ||
		    !parseTimeline(object, "outUs", &event.outUs) || event.outUs <= event.inUs)
			return false;
		event.createdAtUtc = QDateTime::fromString(object.value("createdAtUtc").toString(), Qt::ISODateWithMs);
		loadedEvents.append(event);
	}
	configuration = loadedConfiguration;
	id = root.value("id").toString();
	directory = QFileInfo(manifestPath).absoluteDir().absolutePath();
	startedAtUtc = QDateTime::fromString(root.value("startedAtUtc").toString(), Qt::ISODateWithMs);
	stoppedAtUtc = QDateTime::fromString(root.value("stoppedAtUtc").toString(), Qt::ISODateWithMs);
	replayTakes = std::move(loadedTakes);
	replayEvents = std::move(loadedEvents);
	liveTimelineUs = 0;
	active = false;
	Q_UNUSED(error);
	return true;
}

ReplayTake *ReplaySession::activeTake()
{
	return active && !replayTakes.isEmpty() ? &replayTakes.last() : nullptr;
}

const ReplayTake *ReplaySession::activeTake() const
{
	return active && !replayTakes.isEmpty() ? &replayTakes.last() : nullptr;
}

const ReplayTake *ReplaySession::findTake(const QUuid &takeId) const
{
	for (const ReplayTake &take : replayTakes) {
		if (take.id == takeId)
			return &take;
	}
	return nullptr;
}

bool ReplaySession::saveManifest(QString *error) const
{
	if (directory.isEmpty()) {
		*error = "Replay session folder is not available.";
		return false;
	}
	QJsonArray eventArray;
	for (const ReplayEvent &event : replayEvents)
		eventArray.append(toJson(event));
	QJsonArray takeArray;
	for (const ReplayTake &take : replayTakes)
		takeArray.append(toJson(take));
	QJsonObject root{{"schemaVersion", 2},
			 {"id", id}, {"status", active ? "recording" : "stopped"},
			 {"startedAtUtc", startedAtUtc.toString(Qt::ISODateWithMs)},
			 {"stoppedAtUtc", stoppedAtUtc.toString(Qt::ISODateWithMs)},
			 {"source", QJsonObject{{"name", configuration.sourceName}, {"uuid", configuration.sourceUuid}}},
			 {"encoding", QJsonObject{{"container", "mp4"}, {"videoBitrateMbps", configuration.videoBitrateMbps},
						    {"audioBitrateKbps", configuration.audioBitrateKbps}}},
			 {"takes", takeArray}, {"events", eventArray}};
	QSaveFile file(QDir(directory).filePath("session.json"));
	if (!file.open(QIODevice::WriteOnly)) {
		*error = "Could not open the replay session manifest for writing.";
		return false;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		*error = "Could not save the replay session manifest.";
		return false;
	}
	return true;
}

} // namespace obs_replays
