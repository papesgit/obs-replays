cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Compile OBS's media-playback implementation into this plugin. The OBS
# plugintemplate dependency bundle carries the FFmpeg headers and import
# libraries, but deliberately does not install FFmpegConfig.cmake. Resolve the
# active dependency prefix rather than pinning a dated bundle name so the FFmpeg
# ABI selected in buildspec.json is also the ABI imported by this module.
# Windows extracts the upstream tag ZIP to obs-studio-<version>; macOS uses the
# release source tarball, whose top-level directory is obs-studio-<version>-sources.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" OBS_REPLAYS_BUILDSPEC)
string(JSON OBS_REPLAYS_OBS_VERSION GET "${OBS_REPLAYS_BUILDSPEC}" dependencies obs-studio version)
set(OBS_REPLAYS_OBS_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-${OBS_REPLAYS_OBS_VERSION}")
if(OS_MACOS)
  string(APPEND OBS_REPLAYS_OBS_SOURCE_DIR "-sources")
endif()
set(OBS_REPLAYS_MEDIA_PLAYBACK_DIR
    "${OBS_REPLAYS_OBS_SOURCE_DIR}/shared/media-playback")
find_path(OBS_REPLAYS_DEPS_DIR
          NAMES include/libavcodec/avcodec.h
          PATHS ${CMAKE_PREFIX_PATH}
          NO_DEFAULT_PATH
          REQUIRED)
find_library(OBS_REPLAYS_AVCODEC_LIBRARY NAMES avcodec
             PATHS "${OBS_REPLAYS_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(OBS_REPLAYS_AVDEVICE_LIBRARY NAMES avdevice
             PATHS "${OBS_REPLAYS_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(OBS_REPLAYS_AVFORMAT_LIBRARY NAMES avformat
             PATHS "${OBS_REPLAYS_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(OBS_REPLAYS_AVUTIL_LIBRARY NAMES avutil
             PATHS "${OBS_REPLAYS_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(OBS_REPLAYS_SWSCALE_LIBRARY NAMES swscale
             PATHS "${OBS_REPLAYS_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

# The shared playback helper supports speed only at creation time. Generate a
# locally patched decode unit which delegates timestamp scaling to our runtime
# controller, allowing continuous speed changes without re-basing timestamps.
set(OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/obs-replays-media-playback")
file(MAKE_DIRECTORY "${OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR}")
file(READ "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}/media-playback/decode.c"
     OBS_REPLAYS_DECODE_SOURCE)
set(OBS_REPLAYS_STATIC_SPEED_SCALING [=[
		if (d->m->speed != 100) {
			d->frame_pts = av_rescale_q(d->frame_pts, (AVRational){1, d->m->speed}, (AVRational){1, 100});
			duration = av_rescale_q(duration, (AVRational){1, d->m->speed}, (AVRational){1, 100});
		}
]=])
set(OBS_REPLAYS_RUNTIME_SPEED_SCALING [=[
		obs_replays_scale_decode_timestamp(d, &d->frame_pts, &duration);
]=])
string(FIND "${OBS_REPLAYS_DECODE_SOURCE}" "${OBS_REPLAYS_STATIC_SPEED_SCALING}"
            OBS_REPLAYS_SPEED_SCALING_OFFSET)
if(OBS_REPLAYS_SPEED_SCALING_OFFSET EQUAL -1)
  message(FATAL_ERROR "Could not apply OBS Replays' media playback speed patch")
endif()
string(REPLACE "${OBS_REPLAYS_STATIC_SPEED_SCALING}"
               "${OBS_REPLAYS_RUNTIME_SPEED_SCALING}"
               OBS_REPLAYS_DECODE_SOURCE "${OBS_REPLAYS_DECODE_SOURCE}")
file(WRITE "${OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR}/decode.c"
     "#include \"media-playback-runtime.h\"\n${OBS_REPLAYS_DECODE_SOURCE}")

# FFmpeg seeks backward to a preceding keyframe. For replay markers we need
# the first decoded frame at or after the requested position, not that GOP's
# keyframe. Patch the local media-playback unit to decode through seek preroll
# while paused before invoking our seek callback.
file(READ "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}/media-playback/media.c"
     OBS_REPLAYS_MEDIA_SOURCE)
set(OBS_REPLAYS_ORIGINAL_SEEK_PRELOAD [=[
	if (m->has_video && m->is_local_file) {
		mp_decode_flush(&m->v);
		if (m->seek_next_ts && m->pause && m->v_preload_cb && mp_media_prepare_frames(m))
			mp_media_next_video(m, true);
	}
]=])
set(OBS_REPLAYS_EXACT_SEEK_PRELOAD [=[
	if (m->has_video && m->is_local_file) {
		mp_decode_flush(&m->v);
		if (m->seek_next_ts && m->pause && m->v_preload_cb) {
			const int64_t seek_target_ns = av_rescale(
				pos * 1000, 100, m->speed > 0 ? m->speed : 100);
			for (;;) {
				bool cancel_seek;
				pthread_mutex_lock(&m->mutex);
				cancel_seek = m->kill || m->stopping || !m->active;
				pthread_mutex_unlock(&m->mutex);
				if (cancel_seek || !mp_media_prepare_frames(m) ||
				    !m->v.frame_ready)
					break;
				const bool video_at_target =
					m->v.frame_ready && m->v.frame_pts >= seek_target_ns;
				if (m->has_audio && m->a.frame_ready &&
				    m->a.frame_pts < seek_target_ns)
					m->a.frame_ready = false;
				if (video_at_target) {
					mp_media_next_video(m, true);
					break;
				}
				if (m->v.frame_ready)
					m->v.frame_ready = false;
			}
		}
	}
]=])
string(FIND "${OBS_REPLAYS_MEDIA_SOURCE}" "${OBS_REPLAYS_ORIGINAL_SEEK_PRELOAD}"
            OBS_REPLAYS_SEEK_PRELOAD_OFFSET)
if(OBS_REPLAYS_SEEK_PRELOAD_OFFSET EQUAL -1)
  message(FATAL_ERROR "Could not apply OBS Replays' exact seek patch")
endif()
string(REPLACE "${OBS_REPLAYS_ORIGINAL_SEEK_PRELOAD}"
               "${OBS_REPLAYS_EXACT_SEEK_PRELOAD}"
               OBS_REPLAYS_MEDIA_SOURCE "${OBS_REPLAYS_MEDIA_SOURCE}")

# Upstream allocates or removes a packet from its pool before av_read_frame().
# On EOF/error that packet must be returned, particularly because exact seek
# preroll can reach EOF repeatedly while looking for a requested frame.
set(OBS_REPLAYS_ORIGINAL_PACKET_READ_FAILURE [=[
	int ret = av_read_frame(media->fmt, pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
			blog(LOG_WARNING, "MP: av_read_frame failed: %s (%d)", av_err2str(ret), ret);
		return ret;
	}
]=])
set(OBS_REPLAYS_SAFE_PACKET_READ_FAILURE [=[
	int ret = av_read_frame(media->fmt, pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
			blog(LOG_WARNING, "MP: av_read_frame failed: %s (%d)", av_err2str(ret), ret);
		mp_media_free_packet(media, pkt);
		return ret;
	}
]=])
string(FIND "${OBS_REPLAYS_MEDIA_SOURCE}" "${OBS_REPLAYS_ORIGINAL_PACKET_READ_FAILURE}"
            OBS_REPLAYS_PACKET_READ_FAILURE_OFFSET)
if(OBS_REPLAYS_PACKET_READ_FAILURE_OFFSET EQUAL -1)
  message(FATAL_ERROR "Could not apply OBS Replays' packet cleanup patch")
endif()
string(REPLACE "${OBS_REPLAYS_ORIGINAL_PACKET_READ_FAILURE}"
               "${OBS_REPLAYS_SAFE_PACKET_READ_FAILURE}"
               OBS_REPLAYS_MEDIA_SOURCE "${OBS_REPLAYS_MEDIA_SOURCE}")

# The upstream helper expresses playback speed by changing the advertised
# audio sample rate. That is fine for a fixed rate, but live slider updates
# repeatedly reconfigure OBS's source resampler and produce audible clicks.
# Keep decoded audio at its native rate; the replay channel applies the
# continuously changing resample ratio before submitting it to OBS.
set(OBS_REPLAYS_ORIGINAL_AUDIO_SAMPLE_RATE
    "audio.samples_per_sec = f->sample_rate * m->speed / 100;")
set(OBS_REPLAYS_NATIVE_AUDIO_SAMPLE_RATE
    "audio.samples_per_sec = f->sample_rate;")
string(FIND "${OBS_REPLAYS_MEDIA_SOURCE}" "${OBS_REPLAYS_ORIGINAL_AUDIO_SAMPLE_RATE}"
            OBS_REPLAYS_AUDIO_SAMPLE_RATE_OFFSET)
if(OBS_REPLAYS_AUDIO_SAMPLE_RATE_OFFSET EQUAL -1)
  message(FATAL_ERROR "Could not apply OBS Replays' stable audio-rate patch")
endif()
string(REPLACE "${OBS_REPLAYS_ORIGINAL_AUDIO_SAMPLE_RATE}"
               "${OBS_REPLAYS_NATIVE_AUDIO_SAMPLE_RATE}"
               OBS_REPLAYS_MEDIA_SOURCE "${OBS_REPLAYS_MEDIA_SOURCE}")
file(WRITE "${OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR}/media.c"
     "${OBS_REPLAYS_MEDIA_SOURCE}")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}/media-playback/cache.c"
          "${OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR}/decode.c"
          "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}/media-playback/media-playback.c"
          "${OBS_REPLAYS_MEDIA_PLAYBACK_BUILD_DIR}/media.c"
)
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}"
          "${OBS_REPLAYS_MEDIA_PLAYBACK_DIR}/media-playback"
          "${OBS_REPLAYS_DEPS_DIR}/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE ${OBS_REPLAYS_AVCODEC_LIBRARY}
          ${OBS_REPLAYS_AVDEVICE_LIBRARY}
          ${OBS_REPLAYS_AVFORMAT_LIBRARY}
          ${OBS_REPLAYS_AVUTIL_LIBRARY}
          ${OBS_REPLAYS_SWSCALE_LIBRARY}
)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/obs-websocket-vendor-api.h src/replay-session.cpp src/replay-session.h src/source-capture.cpp
          src/source-capture.h src/segment-writer.cpp src/segment-writer.h
          src/replay-channel-source.cpp src/replay-channel-source.h
          src/media-playback-runtime.c src/media-playback-runtime.h
          data/obs-replays.qrc
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
