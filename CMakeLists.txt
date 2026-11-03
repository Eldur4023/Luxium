cmake_minimum_required(VERSION 3.20)
project(luxium CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(WEBP REQUIRED libwebp)

# ── CEF (doble motor): la web real via Chromium, si la distribucion esta ────
# presente en third_party/cef.  Sin ella, http(s) usa el motor propio+curl.
set(LUXIUM_CEF OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef/libcef_dll/CMakeLists.txt")
    set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)
    add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
    set(LUXIUM_CEF ON)
    message(STATUS "CEF: ON (${CEF_ROOT})")
else()
    message(STATUS "CEF: OFF (third_party/cef no existe; http usa el motor propio)")
endif()

# ── Media engine: FFmpeg's libs (system .so) + headers from the system or from
# third_party/ffmpeg (tools/fetch_ffmpeg_dev.sh, no sudo needed). ────────────
set(LUXIUM_MEDIA OFF)
set(_ff_inc "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/usr/include/x86_64-linux-gnu")
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${_ff_inc}" /usr/include/x86_64-linux-gnu /usr/include)
if(FFMPEG_INCLUDE_DIR)
    set(FFMPEG_LIBS)
    set(_ff_ok ON)
    foreach(_l avformat avcodec avutil swscale swresample)
        find_library(FFMPEG_${_l}_LIB NAMES ${_l} lib${_l}.so.60 lib${_l}.so.58 lib${_l}.so.7 lib${_l}.so.4
                     PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
        if(FFMPEG_${_l}_LIB)
            list(APPEND FFMPEG_LIBS ${FFMPEG_${_l}_LIB})
        else()
            set(_ff_ok OFF)
        endif()
    endforeach()
    if(_ff_ok)
        set(LUXIUM_MEDIA ON)
        message(STATUS "Media: ON (${FFMPEG_INCLUDE_DIR})")
    endif()
endif()
if(NOT LUXIUM_MEDIA)
    message(STATUS "Media: OFF (no FFmpeg headers; run tools/fetch_ffmpeg_dev.sh)")
endif()

# ── The vendored Lux Script engine (frontend + VM, browser natives) ──────────
add_library(lux_script STATIC
    src/lux_script/token.cpp
    src/lux_script/lexer.cpp
    src/lux_script/parser.cpp
    src/lux_script/type.cpp
    src/lux_script/value.cpp
    src/lux_script/vm.cpp
    src/lux_script/emitter.cpp
    src/lux_script/diagnostic.cpp
    src/lux_script/tiny_regex.cpp
    src/lux_script/template.cpp
    src/lux_script/builtin_module.cpp
    src/lux_script/natives.cpp
)
target_include_directories(lux_script PUBLIC src)
target_compile_options(lux_script PRIVATE -w)   # vendored code: Lux owns its warnings

# The engine (src/media) is a static lib shared by the browser and the self-test.
if(LUXIUM_MEDIA)
    add_library(luxium_media STATIC src/media/player.cpp src/media/own_media.cpp)
    set_target_properties(luxium_media PROPERTIES POSITION_INDEPENDENT_CODE ON)   # also linked into the Chromium plugin
    target_include_directories(luxium_media PUBLIC src ${FFMPEG_INCLUDE_DIR} ${SDL2_INCLUDE_DIRS})
    target_link_libraries(luxium_media PUBLIC ${FFMPEG_LIBS} ${SDL2_LIBRARIES} pthread)
    target_compile_options(luxium_media PRIVATE -Wall -Wextra)
else()
    add_library(luxium_media STATIC src/media/player_stub.cpp src/media/own_media_stub.cpp)
    target_include_directories(luxium_media PUBLIC src)
endif()
add_executable(luxium_mediatest src/media/mediatest.cpp)
target_include_directories(luxium_mediatest PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(luxium_mediatest PRIVATE luxium_media ${SDL2_LIBRARIES})

# ── Luxium proper ─────────────────────────────────────────────────────────────
add_executable(luxium
    src/main.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/style/selector.cpp
    src/style/stylesheet.cpp
    src/style/values.cpp
    src/style/properties.cpp
    src/style/cascade.cpp
    src/style/ua.cpp
    src/style/dump.cpp
    src/net/fetch.cpp
    src/script/engine.cpp
    src/script/bytecode_io.cpp
    src/script/page_compile.cpp
    src/render/text.cpp
    src/render/images.cpp
    src/layout/build.cpp
    src/layout/block.cpp
    src/layout/inline.cpp
    src/layout/flex.cpp
    src/layout/grid.cpp
    src/layout/table.cpp
    src/layout/paint.cpp
    src/layout/layout.cpp
    src/layout/media_ui.cpp
    src/layout/controls.cpp
    src/paint/raster.cpp
    src/shell/shell.cpp
    src/page/page.cpp
    src/page/bundle.cpp
    src/page/doc_codec.cpp
    src/page/luxp.cpp
)
# The media shim is embedded in the binary as a raw string literal.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/media/shim.js" _shim_js)
file(WRITE "${CMAKE_BINARY_DIR}/gen/shim_js.inc" "R\"LXSHIM(${_shim_js})LXSHIM\"\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/media/shim.js")
if(LUXIUM_CEF)
    list(APPEND LUXIUM_SRCS src/cef/cef_lazy.cpp src/host/app_host.cpp src/host/menubar.cpp)
else()
    list(APPEND LUXIUM_SRCS src/cef/cef_backend_stub.cpp)
endif()
target_sources(luxium PRIVATE ${LUXIUM_SRCS})

target_include_directories(luxium PRIVATE src)
target_compile_options(luxium PRIVATE -Wall -Wextra)
target_compile_definitions(luxium PRIVATE LUXP_ZLIB=1)
target_link_libraries(luxium PRIVATE lux_script luxium_media ${SDL2_LIBRARIES} ${FREETYPE_LIBRARIES}
                                    ${HARFBUZZ_LIBRARIES} ${CURL_LIBRARIES} ${FONTCONFIG_LIBRARIES} ${JPEG_LIBRARIES} ${WEBP_LIBRARIES} ZLIB::ZLIB)
target_include_directories(luxium PRIVATE ${SDL2_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS}
                                    ${HARFBUZZ_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})

if(LUXIUM_CEF)
    ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    target_compile_definitions(luxium PRIVATE LUXIUM_CEF=1)
    target_include_directories(luxium SYSTEM PRIVATE ${CEF_ROOT})
    target_include_directories(luxium PRIVATE "${CMAKE_BINARY_DIR}/gen")
    target_link_libraries(luxium PRIVATE X11 ${CMAKE_DL_LIBS})
    set_target_properties(luxium PROPERTIES BUILD_RPATH "$ORIGIN")
    # Chromium lives in a plugin the binary dlopen()s on the first JavaScript page (src/cef/cef_lazy.cpp):
    # own-engine windows never map libcef.so.  libcef.so + its resources are copied next to the binary.
    add_library(luxium_cef SHARED src/cef/cef_backend.cpp src/cef/media_bridge.cpp src/cef/cef_plugin.cpp)
    target_include_directories(luxium_cef PRIVATE src "${CMAKE_BINARY_DIR}/gen" ${SDL2_INCLUDE_DIRS})
    target_include_directories(luxium_cef SYSTEM PRIVATE ${CEF_ROOT})
    target_compile_definitions(luxium_cef PRIVATE LUXIUM_CEF=1)
    target_compile_options(luxium_cef PRIVATE -Wall -Wextra -fPIC)
    target_link_libraries(luxium_cef PRIVATE libcef_lib libcef_dll_wrapper luxium_media X11)
    target_link_options(luxium_cef PRIVATE -Wl,-Bsymbolic-functions)
    set_target_properties(luxium_cef PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN" LIBRARY_OUTPUT_NAME luxium_cef)
    add_dependencies(luxium luxium_cef)
    add_custom_command(TARGET luxium POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:luxium>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Release" "$<TARGET_FILE_DIR:luxium>"
        COMMENT "copiando recursos de CEF junto al binario")
endif()

# Headless test harness for the script engine (no window).
add_executable(luxium_headless
    src/tools/headless.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/script/engine.cpp
    src/script/bytecode_io.cpp
    src/script/page_compile.cpp
    src/page/page.cpp
    src/style/stylesheet.cpp
    src/style/selector.cpp
    src/page/bundle.cpp
    src/page/doc_codec.cpp
    src/page/luxp.cpp
)
target_include_directories(luxium_headless PRIVATE src)
target_link_libraries(luxium_headless PRIVATE lux_script ZLIB::ZLIB)
target_compile_definitions(luxium_headless PRIVATE LUXP_ZLIB=1)
