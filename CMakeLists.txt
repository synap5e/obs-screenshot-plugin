cmake_minimum_required(VERSION 3.16...3.21)

project(obs-screenshot-filter VERSION 1.4.0)
add_library(obs-screenshot-filter MODULE)

set(PROJECT_FULL_NAME "Screenshot filter")

set(PLUGIN_AUTHOR "synap5e")

# Add your custom source files here - header files are optional and only required for visibility
# e.g. in Xcode or Visual Studio
target_sources(${CMAKE_PROJECT_NAME} PRIVATE screenshot-filter.c)

# Import libobs as main plugin dependency
find_package(libobs REQUIRED)
include(cmake/ObsPluginHelpers.cmake)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat swscale swresample)
target_include_directories(obs-screenshot-filter PRIVATE ${FFMPEG_INCLUDE_DIRS})

target_link_libraries(obs-screenshot-filter PRIVATE OBS::libobs wininet)

if(MSVC)
  target_include_directories(obs-screenshot-filter
                             PRIVATE ${CMAKE_SOURCE_DIR}/../obs-studio/deps/w32-pthreads/)
  target_link_libraries(obs-screenshot-filter PRIVATE OBS::w32-pthreads)
endif()

target_link_libraries(obs-screenshot-filter PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::avformat
                                                    FFmpeg::swscale FFmpeg::swresample)

# --- Windows-specific build settings and tasks ---
if(OS_WINDOWS)
  configure_file(cmake/bundle/windows/installer-Windows.iss.in
                 ${CMAKE_BINARY_DIR}/installer-Windows.generated.iss)

  configure_file(cmake/bundle/windows/resource.rc.in ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.rc)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.rc)

  if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /WX-)
  endif()
  # --- End of section ---
endif()

set_target_properties(obs-screenshot-filter PROPERTIES FOLDER "plugins")
setup_plugin_target(${CMAKE_PROJECT_NAME})
