#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUuid>

namespace obs_replays {

using TimelineUs = qint64;

struct SessionConfiguration {
	QString replayFolder;
	QString sourceName;
	QString sourceUuid;
	int videoBitrateMbps = 25;
	int audioBitrateKbps = 160;
};

struct ReplayEvent {
	QUuid id;
	QUuid takeId;
	QString label;
	TimelineUs inUs = 0;
	TimelineUs outUs = 0;
	QDateTime createdAtUtc;
};

struct ReplayTake {
	QUuid id;
	QString recordingRelativePath;
	QString sourceName;
	QString sourceUuid;
	TimelineUs durationUs = 0;
	QDateTime startedAtUtc;
	QDateTime stoppedAtUtc;
};

class ReplaySession {
public:
	// Opens the most recent session in the configured replay folder, or creates
	// one when none exists, then begins a new recording take in that session.
	bool start(const SessionConfiguration &configuration, QString *error);
	bool open(const SessionConfiguration &configuration, QString *error);
	bool stop(QString *error);

	void updateLiveTimeline(TimelineUs timelineUs);
	bool addEvent(TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error);
	bool updateEvent(qsizetype index, TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error);
	bool removeEvent(qsizetype index, QString *error);

	bool isActive() const;
	TimelineUs latestTimelineUs() const;
	QString sessionId() const;
	QUuid activeTakeId() const;
	const SessionConfiguration &sessionConfiguration() const;
	const QString &sessionDirectory() const;
	QString recordingPath() const;
	QString recordingPath(const QUuid &takeId) const;
	const QList<ReplayEvent> &events() const;
	const QList<ReplayTake> &takes() const;

private:
	bool saveManifest(QString *error) const;
	bool openMostRecentSession(const SessionConfiguration &configuration, QString *error);
	bool createSession(const SessionConfiguration &configuration, QString *error);
	bool loadManifest(const QString &manifestPath, QString *error);
	ReplayTake *activeTake();
	const ReplayTake *activeTake() const;
	const ReplayTake *findTake(const QUuid &takeId) const;

	SessionConfiguration configuration;
	QString id;
	QString directory;
	QDateTime startedAtUtc;
	QDateTime stoppedAtUtc;
	QList<ReplayEvent> replayEvents;
	QList<ReplayTake> replayTakes;
	TimelineUs liveTimelineUs = 0;
	bool active = false;
};

} // namespace obs_replays
