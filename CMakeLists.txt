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

# Compile OBS's media-playback implementation into this plugin.  The OBS
# plugintemplate dependency bundle already carries the matching FFmpeg headers
# and import libraries, but it deliberately does not install FFmpegConfig.cmake.
# Referencing those bundled files directly keeps this build self-contained.
set(OBS_REPLAYS_MEDIA_PLAYBACK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-31.1.1/shared/media-playback")
set(OBS_REPLAYS_DEPS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64")
set(OBS_REPLAYS_FFMPEG_RUNTIME_DLLS
    "${OBS_REPLAYS_DEPS_DIR}/bin/avcodec-61.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/avdevice-61.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/avfilter-10.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/avformat-61.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/avutil-59.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/librist.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/libx264-164.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/srt.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/swresample-5.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/swscale-8.dll"
    "${OBS_REPLAYS_DEPS_DIR}/bin/zlib.dll"
)

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
			const int64_t seek_target_ns = pos * 1000;
			while (mp_media_prepare_frames(m)) {
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
  PRIVATE "${OBS_REPLAYS_DEPS_DIR}/lib/avcodec.lib"
          "${OBS_REPLAYS_DEPS_DIR}/lib/avdevice.lib"
          "${OBS_REPLAYS_DEPS_DIR}/lib/avformat.lib"
          "${OBS_REPLAYS_DEPS_DIR}/lib/avutil.lib"
          "${OBS_REPLAYS_DEPS_DIR}/lib/swscale.lib"
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

# The direct replay decoder links against the FFmpeg runtime shipped by the OBS
# dependency bundle. OBS loads runtime DLLs from bin/64bit; putting them in
# obs-plugins/64bit makes its plugin scanner try (and correctly fail) to treat
# them as modules. The install layout keeps them under bin/64bit accordingly.
install(FILES ${OBS_REPLAYS_FFMPEG_RUNTIME_DLLS}
        DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${OBS_REPLAYS_FFMPEG_RUNTIME_DLLS}
          "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit"
  COMMENT "Copy FFmpeg replay runtime dependencies"
  VERBATIM
)
