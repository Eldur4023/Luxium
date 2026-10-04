cmake_minimum_required(VERSION 3.20)
project(luxium CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
pkg_check_modules(SDL2 REQUIRED sdl2)

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
    src/script/engine.cpp
    src/render/text.cpp
    src/render/layout.cpp
    src/shell/shell.cpp
    src/page/page.cpp
)
target_include_directories(luxium PRIVATE src)
target_compile_options(luxium PRIVATE -Wall -Wextra)
target_link_libraries(luxium PRIVATE lux_script ${SDL2_LIBRARIES} ${FREETYPE_LIBRARIES} ${HARFBUZZ_LIBRARIES})
target_include_directories(luxium PRIVATE ${SDL2_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${HARFBUZZ_INCLUDE_DIRS})

# Headless test harness for the script engine (no window).
add_executable(luxium_headless
    src/tools/headless.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/script/engine.cpp
    src/page/page.cpp
)
target_include_directories(luxium_headless PRIVATE src)
target_link_libraries(luxium_headless PRIVATE lux_script)
