cmake_minimum_required(VERSION 3.16...3.21)

project(obs-screenshot-filter VERSION 1.6.0)
add_library(obs-screenshot-filter MODULE)

set(PROJECT_FULL_NAME "Screenshot filter")

set(PLUGIN_AUTHOR "synap5e")

# Add your custom source files here - header files are optional and only required for visibility
# e.g. in Xcode or Visual Studio
target_sources(${CMAKE_PROJECT_NAME} PRIVATE screenshot-filter.c stb_image_write.h)

# Import libobs as main plugin dependency
find_package(libobs REQUIRED)
include(cmake/ObsPluginHelpers.cmake)

# No FFmpeg dependency: PNG encoding uses the bundled stb_image_write.h, so the
# plugin does not break when OBS bumps its FFmpeg DLL versions.
target_link_libraries(obs-screenshot-filter PRIVATE OBS::libobs wininet)

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
