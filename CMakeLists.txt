cmake_minimum_required(VERSION 3.20)
project(luxium CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(luxium
    src/main.cpp
    src/dom/node.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/luxscript/lexer.cpp
)

target_include_directories(luxium PRIVATE src)
target_compile_options(luxium PRIVATE -Wall -Wextra)
