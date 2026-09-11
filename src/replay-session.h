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
	int segmentDurationSeconds = 120;
};

struct ReplaySegment {
	int index = 0;
	QString relativePath;
	TimelineUs startUs = 0;
	TimelineUs endUs = 0;
	qint64 sizeBytes = 0;
	bool finalized = false;
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

	bool beginSegment(const QString &relativePath, TimelineUs startUs, QString *error);
	bool finalizeCurrentSegment(TimelineUs endUs, qint64 sizeBytes, QString *error);
	void updateLiveTimeline(TimelineUs timelineUs);
	bool addEvent(TimelineUs inUs, TimelineUs outUs, const QString &label, QString *error);

	bool isActive() const;
	TimelineUs latestTimelineUs() const;
	const QString &sessionDirectory() const;
	const QList<ReplaySegment> &recordedSegments() const;
	const QList<ReplayEvent> &events() const;

private:
	bool saveManifest(QString *error) const;

	SessionConfiguration configuration;
	QString id;
	QString directory;
	QDateTime startedAtUtc;
	QDateTime stoppedAtUtc;
	QList<ReplaySegment> segments;
	QList<ReplayEvent> replayEvents;
	TimelineUs liveTimelineUs = 0;
	bool active = false;
};

} // namespace obs_replays
