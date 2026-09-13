# OBS Replays WebSocket API

OBS Replays extends OBS's built-in `obs-websocket` server through its native Vendor API. It does not open another port or implement its own authentication. Connect, authenticate, and identify using the normal obs-websocket v5 protocol.

Vendor name: `obs-replays`

All times are integer milliseconds relative to the start of an event's recording take. Session, take, and event IDs are UUID strings without braces.

## Calling requests

Use standard obs-websocket `CallVendorRequest`:

```json
{
  "requestType": "CallVendorRequest",
  "requestId": "get-replay-session",
  "requestData": {
    "vendorName": "obs-replays",
    "requestType": "GetSession",
    "requestData": {}
  }
}
```

The requested data is returned in the outer request's `responseData`. Mutating requests return `success`. When OBS accepts the request but the replay engine cannot perform it, the outer request remains successful and `responseData` contains `success: false` and `error`.

## Read requests

### `GetSession`

Request data: `{}`.

Returns `protocolVersion: 1` and `available`. When available, it also returns:

```json
{
  "id": "session-uuid",
  "folder": "D:/Videos/OBS/replays/OBS-Replay-...",
  "sourceName": "Game Capture",
  "sourceUuid": "source-uuid",
  "recording": true,
  "activeTakeId": "take-uuid",
  "recordedThroughMs": 125400,
  "takeCount": 2,
  "eventCount": 8
}
```

`activeTakeId` is empty while stopped. `recordedThroughMs` is the active take duration while recording, otherwise the last recorded live position.

### `GetRecordingStatus`

Request data: `{}`. Returns `protocolVersion: 1` and `active`. When active it also contains `sessionId`, `takeId`, `recordedThroughMs`, `capturedVideoFrames`, and `capturedAudioFrames`.

### `GetPlayoutStatus`

Request data: `{}`. Returns:

```json
{
  "protocolVersion": 1,
  "success": true,
  "active": true,
  "ending": false,
  "playbackRatePercent": 100,
  "queueLength": 3,
  "currentQueueIndex": 1,
  "currentEventId": "event-uuid",
  "currentLabel": "Goal"
}
```

`currentQueueIndex` is one-based, or `0` when no queued event is active. `ending` is true while the outro transition is running.

### `ListEvents`

Request data: `{}`. Returns `protocolVersion: 1`, `sessionId` when a session is open, and `events`:

```json
{
  "id": "event-uuid",
  "takeId": "take-uuid",
  "index": 1,
  "label": "Goal",
  "inMs": 1200,
  "outMs": 5300,
  "createdAtUtc": "2026-09-12T10:15:30.123Z"
}
```

`index` is one-based list/creation order. Use `id`, not `index`, as the persistent event identifier.

### `GetEvent`

Request data:

```json
{ "eventId": "event-uuid" }
```

Returns `success` and `event`, with the same shape as an entry returned by `ListEvents`.

## Recording requests

### `StartRecording`

Request data: `{}`. Starts a new take using the source, replay folder, and capture settings configured in the OBS Replays dock. A compatible stopped session in that folder is reused. Returns `success` and active recording-status fields.

### `StopRecording`

Request data: `{}`. Requests finalization of the active take. On acceptance returns `success: true` and `stopping: true`. Wait for `RecordingStopped` before treating the take as finalized and playable.

## Event requests

### `CreateEvent`

```json
{
  "inMs": 1200,
  "outMs": 5300,
  "label": "Goal"
}
```

`inMs` and `outMs` are required; `label` is optional. The range must be inside the active recording take, so this operation is available only while recording. Returns the new `event`.

### `CreateEventFromLive`

```json
{
  "preRollMs": 5000,
  "label": "Goal"
}
```

Creates an immediate event ending at the current live position and beginning `preRollMs` earlier. `preRollMs` must be greater than zero; `label` is optional. There is currently no deferred post-roll marker operation. Returns the new `event`.

### `UpdateEvent`

```json
{
  "eventId": "event-uuid",
  "inMs": 1500,
  "outMs": 5600,
  "label": "Goal, close-up"
}
```

`eventId`, `inMs`, and `outMs` are required. `label` is optional; omitting it preserves the current label. The resulting range must remain inside the event's recording take. Returns the updated `event`.

### `DeleteEvent`

```json
{ "eventId": "event-uuid" }
```

Returns the deleted `eventId` on success.

## Playout requests

### `StartPlayout`

```json
{
  "eventIds": [
    { "eventId": "first-event-uuid" },
    { "eventId": "second-event-uuid" }
  ],
  "playOrder": "creation"
}
```

Starts playout using the replay scene and transitions configured in the dock. `eventIds` must be object entries rather than an array of strings because OBS's native Vendor API data model supports object arrays.

`playOrder` is optional:

- `"creation"` (default): events play in replay-session creation/list order, regardless of the order in `eventIds`.
- `"provided"`: events play in the exact order supplied in `eventIds`.

The request fails if playout is already active, an event does not exist, a required recording take is unavailable, or `playOrder` is invalid. On success it returns playout status.

### `StopPlayout`

Request data: `{}`. Immediately starts the configured outro transition. On acceptance returns `success: true` and `stopping: true`; wait for `PlayoutStopped` before treating the replay scene as off-air.

### `SetPlayoutRate`

```json
{ "ratePercent": 50 }
```

Sets speed from `10` through `100` percent. It applies immediately to active playout and is used for subsequent playout until changed. It is not persisted across an OBS restart. Returns the accepted `ratePercent`.

## Vendor events

Identify with the normal obs-websocket `Vendors` subscription intent (`512`). OBS will then send normal `VendorEvent` messages. Filter for `vendorName: "obs-replays"`; `eventType` is one of:

- `SessionChanged`
- `RecordingStarted`
- `RecordingStopped`
- `EventListChanged`
- `PlayoutStarted`
- `PlayoutStopping`
- `PlayoutStopped`
- `PlayoutRateChanged`

Every event includes `eventData` with `protocolVersion: 1` and the current session summary: `available`, and when available the session ID, folder, source, recording state, active take, recorded position, take count, and event count. For authoritative event or playout details after a notification, call `ListEvents` and/or `GetPlayoutStatus`.

## Current scope

The current engine intentionally has no seek, preview, pause/resume, source-selection, replay-folder management, or deferred post-roll event requests. Each recording take captures one configured source, but later stopped takes in the same session may use another source. Every event belongs to one recording take and cannot span takes.
