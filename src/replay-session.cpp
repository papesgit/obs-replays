#include "replay-session.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace obs_replays {

namespace {

QJsonObject toJson(const ReplaySegment &segment)
{
	return {{"index", segment.index},
		{"path", segment.relativePath},
		{"startUs", QString::number(segment.startUs)},
		{"endUs", QString::number(segment.endUs)},
		{"sizeBytes", QString::number(segment.sizeBytes)},
		{"finalized", segment.finalized}};
}

QJsonObject toJson(const ReplayEvent &event)
{
	return {{"id", event.id.toString(QUuid::WithoutBraces)},
		{"label", event.label},
		{"inUs", QString::number(event.inUs)},
		{"outUs", QString::number(event.outUs)},
		{"createdAtUtc", event.createdAtUtc.toString(Qt::ISODateWithMs)}};
}

} // namespace

bool ReplaySession::start(const SessionConfiguration &sessionConfiguration, QString *error)
{
	if (active) {
		*error = "A replay session is already active.";
		return false;
	}
	if (sessionConfiguration.replayFolder.isEmpty() || sessionConfiguration.sourceUuid.isEmpty()) {
		*error = "A replay folder and source are required to start a session.";
		return false;
	}

	configuration = sessionConfiguration;
	id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	startedAtUtc = QDateTime::currentDateTimeUtc();
	stoppedAtUtc = {};
	segments.clear();
	replayEvents.clear();
	liveTimelineUs = 0;

	const QString sessionName = QString("OBS-Replay-%1-%2")
					    .arg(startedAtUtc.toString("yyyy-MM-dd_hh-mm-ss"), id.left(8));
	directory = QDir(configuration.replayFolder).filePath(sessionName);
	if (!QDir().mkpath(QDir(directory).filePath("segments"))) {
		*error = "Could not create the replay session folder.";
		directory.clear();
		return false;
	}

	active = true;
	if (!saveManifest(error)) {
		active = false;
		return false;
	}
	return true;
}

bool ReplaySession::stop(QString *error)
{
	if (!active) {
		*error = "No replay session is active.";
		return false;
	}

	stoppedAtUtc = QDateTime::currentDateTimeUtc();
	active = false;
	return saveManifest(error);
}

bool ReplaySession::beginSegment(const QString &relativePath, TimelineUs startUs, QString *error)
{
	if (!active || relativePath.isEmpty() || startUs < 0) {
		*error = "Cannot begin a segment for the current replay session.";
		return false;
	}
	if (!segments.isEmpty() && !segments.last().finalized) {
		*error = "The previous replay segment has not been finalized.";
		return false;
	}

	segments.append({static_cast<int>(segments.size()) + 1, relativePath, startUs, 0, 0,
			 false});
	liveTimelineUs = startUs;
	return saveManifest(error);
}

bool ReplaySession::finalizeCurrentSegment(TimelineUs endUs, qint64 sizeBytes, QString *error)
{
	if (!active || segments.isEmpty() || segments.last().finalized ||
	    endUs < segments.last().startUs || sizeBytes < 0) {
		*error = "Cannot finalize the current replay segment.";
		return false;
	}

	ReplaySegment &segment = segments.last();
	segment.endUs = endUs;
	segment.sizeBytes = sizeBytes;
	segment.finalized = true;
	liveTimelineUs = endUs;
	return saveManifest(error);
}

void ReplaySession::updateLiveTimeline(TimelineUs timelineUs)
{
	if (active && timelineUs > liveTimelineUs)
		liveTimelineUs = timelineUs;
}

bool ReplaySession::addEvent(TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error)
{
	if (!active || inUs < 0 || outUs <= inUs || outUs > latestTimelineUs()) {
		*error = "The event range is outside the recorded replay timeline.";
		return false;
	}

	replayEvents.append({QUuid::createUuid(), label, inUs, outUs, QDateTime::currentDateTimeUtc()});
	return saveManifest(error);
}

bool ReplaySession::isActive() const
{
	return active;
}

TimelineUs ReplaySession::latestTimelineUs() const
{
	return liveTimelineUs;
}

const QString &ReplaySession::sessionDirectory() const
{
	return directory;
}

const QList<ReplaySegment> &ReplaySession::recordedSegments() const
{
	return segments;
}

const QList<ReplayEvent> &ReplaySession::events() const
{
	return replayEvents;
}

bool ReplaySession::saveManifest(QString *error) const
{
	if (directory.isEmpty()) {
		*error = "Replay session folder is not available.";
		return false;
	}

	QJsonArray segmentArray;
	for (const ReplaySegment &segment : segments)
		segmentArray.append(toJson(segment));
	QJsonArray eventArray;
	for (const ReplayEvent &event : replayEvents)
		eventArray.append(toJson(event));

	QJsonObject root{{"schemaVersion", 1},
			 {"id", id},
			 {"status", active ? "recording" : "stopped"},
			 {"startedAtUtc", startedAtUtc.toString(Qt::ISODateWithMs)},
			 {"stoppedAtUtc", stoppedAtUtc.toString(Qt::ISODateWithMs)},
			 {"source", QJsonObject{{"name", configuration.sourceName},
								 {"uuid", configuration.sourceUuid}}},
			 {"encoding", QJsonObject{{"container", "mkv"},
								   {"videoBitrateMbps", configuration.videoBitrateMbps},
								   {"audioBitrateKbps", configuration.audioBitrateKbps}}},
			 {"segmentDurationSeconds", configuration.segmentDurationSeconds},
			 {"segments", segmentArray},
			 {"events", eventArray}};

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
