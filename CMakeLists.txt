project(obs-screenshot-filter)

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat swscale swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(obs-screenshot-filter_SOURCES
      screenshot-filter.c
)

add_library(obs-screenshot-filter MODULE
      ${obs-screenshot-filter_SOURCES})
target_link_libraries(obs-screenshot-filter
      libobs
      wininet.lib
      ${obs-ffmpeg_PLATFORM_DEPS}
      ${FFMPEG_LIBRARIES}
)

install_obs_plugin_with_data(obs-screenshot-filter data)
