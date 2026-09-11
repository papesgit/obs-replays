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
	QString recordingRelativePath = "replay.mp4";
};

struct ReplayEvent {
	QUuid id;
	QString label;
	TimelineUs inUs = 0;
	TimelineUs outUs = 0;
	QDateTime createdAtUtc;
};

class ReplaySession {
public:
	bool start(const SessionConfiguration &configuration, QString *error);
	bool stop(QString *error);

	void updateLiveTimeline(TimelineUs timelineUs);
	bool addEvent(TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error);
	bool updateEvent(qsizetype index, TimelineUs inUs, TimelineUs outUs, const QString &label,
			 QString *error);
	bool removeEvent(qsizetype index, QString *error);

	bool isActive() const;
	TimelineUs latestTimelineUs() const;
	const QString &sessionDirectory() const;
	QString recordingPath() const;
	const QList<ReplayEvent> &events() const;

private:
	bool saveManifest(QString *error) const;

	SessionConfiguration configuration;
	QString id;
	QString directory;
	QDateTime startedAtUtc;
	QDateTime stoppedAtUtc;
	QList<ReplayEvent> replayEvents;
	TimelineUs liveTimelineUs = 0;
	bool active = false;
};

} // namespace obs_replays
