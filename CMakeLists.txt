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

# ── Luxium proper ─────────────────────────────────────────────────────────────
add_executable(luxium
    src/main.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/css/css.cpp
    src/net/fetch.cpp
    src/script/engine.cpp
    src/render/text.cpp
    src/render/images.cpp
    src/render/layout.cpp
    src/shell/shell.cpp
    src/page/page.cpp
)
if(LUXIUM_CEF)
    list(APPEND LUXIUM_SRCS src/cef/cef_backend.cpp)
else()
    list(APPEND LUXIUM_SRCS src/cef/cef_backend_stub.cpp)
endif()
target_sources(luxium PRIVATE ${LUXIUM_SRCS})

target_include_directories(luxium PRIVATE src)
target_compile_options(luxium PRIVATE -Wall -Wextra)
target_link_libraries(luxium PRIVATE lux_script ${SDL2_LIBRARIES} ${FREETYPE_LIBRARIES}
                                    ${HARFBUZZ_LIBRARIES} ${CURL_LIBRARIES} ZLIB::ZLIB)
target_include_directories(luxium PRIVATE ${SDL2_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS}
                                    ${HARFBUZZ_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})

if(LUXIUM_CEF)
    ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    target_compile_definitions(luxium PRIVATE LUXIUM_CEF=1)
    target_include_directories(luxium PRIVATE ${CEF_ROOT})
    target_link_libraries(luxium PRIVATE libcef_lib libcef_dll_wrapper X11)
    set_target_properties(luxium PROPERTIES BUILD_RPATH "${CEF_ROOT}/Release")
    add_custom_command(TARGET luxium POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:luxium>"
        COMMAND ${CMAKE_COMMAND} -E copy "${CEF_ROOT}/Release/libcef.so" "$<TARGET_FILE_DIR:luxium>"
        COMMAND ${CMAKE_COMMAND} -E copy "${CEF_ROOT}/Release/libvk_swiftshader.so" "$<TARGET_FILE_DIR:luxium>"
        COMMENT "copiando recursos de CEF junto al binario")
endif()

# Headless test harness for the script engine (no window).
add_executable(luxium_headless
    src/tools/headless.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/css/css.cpp
    src/script/engine.cpp
    src/page/page.cpp
)
target_include_directories(luxium_headless PRIVATE src)
target_link_libraries(luxium_headless PRIVATE lux_script)
