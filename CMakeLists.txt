cmake_minimum_required(VERSION 3.16)
project(videoframevr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(RSVG REQUIRED IMPORTED_TARGET librsvg-2.0)
pkg_check_modules(SMB2 REQUIRED IMPORTED_TARGET libsmb2)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh>=0.12)
pkg_check_modules(UDFREAD REQUIRED IMPORTED_TARGET libudfread>=1.2)
pkg_check_modules(BLURAY REQUIRED IMPORTED_TARGET libbluray>=1.4)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL)
find_package(Vulkan REQUIRED)

add_executable(videoframevr
  src/main.cpp
  src/xr_context.cpp
  src/video_player.cpp
  src/gl_util.cpp
  src/autofocus.cpp
  src/bluray.cpp
  src/bluray_disc.cpp
  src/cairo_canvas.cpp
  src/colour_balance.cpp
  src/controller_guide.cpp
  src/density_map.cpp
  src/depth_estimator.cpp
  src/diagnostics.cpp
  src/direct_depth.cpp
  src/direct_video.cpp
  src/eye_resolution.cpp
  src/eye_targets.cpp
  src/file_ops.cpp
  src/file_util.cpp
  src/first_run.cpp
  src/flat_screen.cpp
  src/folder_listing.cpp
  src/focus_controller.cpp
  src/focus_marker.cpp
  src/focus_target.cpp
  src/foveation.cpp
  src/frame_grabber.cpp
  src/gl_interop.cpp
  src/gpu_context.cpp
  src/hints.cpp
  src/key_share.cpp
  src/focus_settings.cpp
  src/glass_backdrop.cpp
  src/green_screen_check.cpp
  src/library.cpp
  src/library_index.cpp
  src/accent.cpp
  src/menu.cpp
  src/menu_browser.cpp
  src/menu_delete.cpp
  src/menu_focus.cpp
  src/menu_hints.cpp
  src/menu_icons.cpp
  src/menu_library.cpp
  src/menu_network.cpp
  src/menu_offline.cpp
  src/menu_places.cpp
  src/menu_player.cpp
  src/menu_presets.cpp
  src/menu_rating.cpp
  src/menu_search.cpp
  src/menu_settings.cpp
  src/menu_setup.cpp
  src/menu_setup_controls.cpp
  src/menu_setup_stash.cpp
  src/menu_setup_folders.cpp
  src/menu_stash_covers.cpp
  src/menu_widgets.cpp
  src/menu_whats_new.cpp
  src/mirror_window.cpp
  src/menu_stash.cpp
  src/menu_stash_draw.cpp
  src/menu_stash_home.cpp
  src/menu_stash_lists.cpp
  src/menu_stash_scene.cpp
  src/menu_stash_performers.cpp
  src/menu_stash_preview.cpp
  src/menu_stash_scenes.cpp
  src/menu_stash_settings.cpp
  src/menu_stash_folders.cpp
  src/menu_stash_studios.cpp
  src/motion.cpp
  src/offline.cpp
  src/oshash.cpp
  src/performance_readout.cpp
  src/photo_preloader.cpp
  src/pipelines.cpp
  src/places.cpp
  src/presets.cpp
  src/refresh_rate.cpp
  src/smb_discovery.cpp
  src/pixel_path.cpp
  src/read_ahead.cpp
  src/release_update.cpp
  src/remote.cpp
  src/remote_avio.cpp
  src/remote_connection.cpp
  src/removable_drives.cpp
  src/room_light.cpp
  src/run_log.cpp
  src/scene_renderer.cpp
  src/screen_control.cpp
  src/see_through.cpp
  src/seek_preview.cpp
  src/settings.cpp
  src/setup_intro.cpp
  src/setup_placement.cpp
  src/setup_places.cpp
  src/sftp.cpp
  src/smb.cpp
  src/stash.cpp
  src/stash_catalog.cpp
  src/stash_similarity.cpp
  src/stash_client.cpp
  src/stash_delete.cpp
  src/stash_folders.cpp
  src/stash_http.cpp
  src/stash_images.cpp
  src/stash_preview.cpp
  src/stash_query.cpp
  src/stash_settings.cpp
  src/stash_store.cpp
  src/stash_stream.cpp
  src/stash_sync.cpp
  src/stash_text.cpp
  src/stash_write_back.cpp
  src/stereo_correction.cpp
  src/strings.cpp
  src/strings_de.cpp
  src/strings_es.cpp
  src/strings_pt_br.cpp
  src/strings_fr.cpp
  src/strings_zh_cn.cpp
  src/strings_ja.cpp
  src/subtitle_overlay.cpp
  src/thumbnailer.cpp
  src/tile_grid.cpp
  src/uploader.cpp
  src/video_format.cpp
  src/video_memory.cpp
  src/video_texture.cpp
  src/vr_space.cpp
  src/whats_new.cpp
)

# The library's schema, built in from the file the importer reads too.
set(LIBRARY_SCHEMA_SQL ${CMAKE_CURRENT_SOURCE_DIR}/src/library_schema.sql)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LIBRARY_SCHEMA_SQL})
file(READ ${LIBRARY_SCHEMA_SQL} LIBRARY_SCHEMA)
configure_file(src/library_schema.h.in generated/library_schema.h @ONLY)
target_include_directories(videoframevr PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# The scene's shaders, compiled to SPIR-V and built in as headers
# (generated/shaders/<name>.h, symbol kSpirv_<name>). glslangValidator runs on
# the build host.
find_program(GLSLANG_VALIDATOR glslangValidator)
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found: install glslang "
                      "(Ubuntu: glslang-tools, Arch and SteamOS: glslang)")
endif()
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
# Included by the stages; every stage is rebuilt when one changes.
set(SHADER_INCLUDES
  ${SHADER_DIR}/common.glsl
  ${SHADER_DIR}/video.glsl
  ${SHADER_DIR}/projection.glsl
  ${SHADER_DIR}/screen.glsl
  ${SHADER_DIR}/panel.glsl
  ${SHADER_DIR}/pointer.glsl
  ${SHADER_DIR}/preview.glsl
)
set(SHADER_HEADERS)
foreach(shader IN ITEMS
    fullscreen.vert
    foveation.frag
    depth_patch.frag
    sky.frag
    blur.frag
    surround.frag
    screen.vert
    screen.frag
    panel.vert
    panel.frag
    pointer.vert
    pointer.frag
    preview.vert
    preview.frag)
  string(MAKE_C_IDENTIFIER ${shader} symbol)
  set(header ${SHADER_OUT}/${shader}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.2 --vn kSpirv_${symbol}
            -o ${header} ${SHADER_DIR}/${shader}
    DEPENDS ${SHADER_DIR}/${shader} ${SHADER_INCLUDES}
    COMMENT "Compiling shader ${shader}"
    VERBATIM)
  list(APPEND SHADER_HEADERS ${header})
endforeach()
# The video shaders again as variants (name:shader:defines, defines joined
# by +): the screen with sharpening, for Q10C decoder frames (Pipelines,
# DirectDepth, DirectVideo's stills) and Q08C ones' depth patch (DirectDepth).
# The other variants of surround.frag and screen.frag are specialization
# constants (Pipelines).
foreach(variant
    screen_sharpen:screen.frag:SHARPEN
    surround_tp10:surround.frag:TP10
    screen_tp10:screen.frag:TP10
    depth_patch_tp10:depth_patch.frag:TP10
    depth_patch_ycbcr:depth_patch.frag:YCBCR
    still_tp10:still.frag:TP10)
  string(REPLACE ":" ";" parts ${variant})
  list(GET parts 0 name)
  list(GET parts 1 shader)
  list(GET parts 2 defines)
  string(REPLACE "+" ";" defines ${defines})
  list(TRANSFORM defines PREPEND -D OUTPUT_VARIABLE define_args)
  set(header ${SHADER_OUT}/${name}.frag.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.2 ${define_args} --vn kSpirv_${name}_frag
            -o ${header} ${SHADER_DIR}/${shader}
    DEPENDS ${SHADER_DIR}/${shader} ${SHADER_INCLUDES}
    COMMENT "Compiling shader ${shader} as ${name}"
    VERBATIM)
  list(APPEND SHADER_HEADERS ${header})
endforeach()
add_custom_target(scene_shaders DEPENDS ${SHADER_HEADERS})
add_dependencies(videoframevr scene_shaders)
target_sources(videoframevr PRIVATE ${SHADER_HEADERS})

# The version shown (--version, the log): a release build's tag and commit,
# which packaging/release/release.py passes in the environment, else this
# version marked as a development build.
set(VIDEOFRAMEVR_VERSION_TEXT "$ENV{VIDEOFRAMEVR_RELEASE_VERSION}")
string(REGEX REPLACE "^v" "" VIDEOFRAMEVR_VERSION_TEXT "${VIDEOFRAMEVR_VERSION_TEXT}")
set(VIDEOFRAMEVR_RELEASE_TEXT "${VIDEOFRAMEVR_VERSION_TEXT}")
if(VIDEOFRAMEVR_VERSION_TEXT STREQUAL "")
  set(VIDEOFRAMEVR_VERSION_TEXT "${PROJECT_VERSION}-dev")
endif()

# The release notes, built in for What's new (src/whats_new.h). A release
# build's must be well-formed and have its version's notes.
include(cmake/changelog.cmake)
set(CHANGELOG_FILE ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CHANGELOG_FILE})
check_changelog(${CHANGELOG_FILE} "${VIDEOFRAMEVR_RELEASE_TEXT}")
file(READ ${CHANGELOG_FILE} CHANGELOG)
configure_file(src/changelog.h.in generated/changelog.h @ONLY)
target_compile_definitions(videoframevr PRIVATE GL_GLEXT_PROTOTYPES
                                                VIDEOFRAMEVR_VERSION="${VIDEOFRAMEVR_VERSION_TEXT}"
                                                VIDEOFRAMEVR_COMMIT="$ENV{VIDEOFRAMEVR_RELEASE_COMMIT}")
# OpenXR structs are initialized as {XR_TYPE_...}, leaving the rest zeroed on purpose.
target_compile_options(videoframevr PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(videoframevr PRIVATE
  PkgConfig::OPENXR
  PkgConfig::MPV
  PkgConfig::GLFW
  PkgConfig::PANGOCAIRO
  PkgConfig::CURL
  PkgConfig::PIXBUF
  PkgConfig::GIO
  PkgConfig::FFMPEG
  PkgConfig::SQLITE
  PkgConfig::RSVG
  PkgConfig::SMB2
  PkgConfig::LIBSSH
  PkgConfig::UDFREAD
  PkgConfig::BLURAY
  nlohmann_json::nlohmann_json
  Threads::Threads
  OpenGL::OpenGL
  Vulkan::Vulkan
)

# Installation (also what the AppImage is made from): the program, a menu
# entry and its icon.
include(GNUInstallDirs)
install(TARGETS videoframevr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/videoframevr.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/videoframevr.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(size IN ITEMS 32 256 512)
  install(FILES packaging/steam/icon-${size}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
          RENAME videoframevr.png)
endforeach()
install(FILES
        packaging/steam/cover.png
        packaging/steam/wide.png
        packaging/steam/hero.png
        packaging/steam/logo.png
        packaging/steam/icon-32.png
        packaging/steam/icon-256.png
        packaging/steam/icon-512.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/videoframevr/steam)
