# OBS Replays

OBS Replays is an instant-replay plugin for OBS Studio. It continuously records a selected OBS source, lets an operator mark recent moments as replay events, and plays selected events through a dedicated replay scene.

This project is currently in early development. Test it with your own sources and workflow before using it in a production broadcast.

## Features

- Continuous recording of one OBS source with configurable video and audio bitrates
- Buttons for marking the previous 1, 2, 3, 5, or 10 seconds as an event
- Editable event in/out times and labels
- Playback of one or multiple selected events
- Adjustable playback speed from 10% to 100%
- Configurable intro, outro, and between-event transitions
- Automatic return to the previous Program scene after playout
- Persistent replay sessions containing recording takes and event metadata
- An obs-websocket Vendor API for external control

## Requirements

- OBS Studio 32.2.2
- Windows x64
- Sufficient storage for continuous replay recording

The current release is built and tested on Windows. Other platforms are not currently supported.

## Installation

1. Close OBS Studio.
2. Download the Windows archive from the GitHub Releases page.
3. Extract the included `obs-replays.dll` to:

   ```text
   C:\ProgramData\obs-studio\plugins\obs-replays\bin\64bit\
   ```

   You will have to create the folder structure in the plugins folder yourself.
4. Start OBS Studio.
5. Open the **Replays** dock from OBS's **Docks** menu if it is not already visible.

## Basic usage

1. Create or choose a scene that will be used for replay playout.
2. In the Replays dock, select the source to record and a folder for replay sessions.
3. Configure the recording bitrates, replay scene, and transitions.
4. Click **Start recording**.
5. Use the `-1`, `-2`, `-3`, `-5`, or `-10` buttons to create an event ending at the current live position.
6. Select one or more events in the table and click **Play selected events**.
7. Click the same button during playout to stop and run the configured outro transition.

The plugin creates an **OBS Replays Channel A** source in the selected replay scene when needed. Recording keeps the selected capture source active even when it is not part of the current Program scene.

Replay recordings can use substantial disk space. The dock displays an estimate based on the selected bitrates and available storage.

## WebSocket API

OBS Replays extends OBS's built-in obs-websocket server with recording, event, playout, and playback-rate controls. See [docs/Websocket.md](docs/Websocket.md) for the request and event reference.

## Current limitations

- One capture source and one replay playback channel
- No multi-angle replay or synchronized camera switching
- No event preview, pause, or manual seek controls
- Source, network, decoder, or rendering stalls may be present in the recorded replay
- Recording is stored as high-bitrate fragmented MP4 and is intended for replay use rather than archival recording

## Building on Windows

Requirements:

- Visual Studio 2022 with C++ development tools
- CMake 3.28 or newer

Configure and build:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## License

OBS Replays is licensed under the [GNU General Public License v2](https://github.com/papesgit/obs-replays/blob/main/LICENSE).
