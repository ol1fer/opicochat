cmake_minimum_required(VERSION 3.20)
project(opicochat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common library
add_library(opico_common STATIC
    src/common.cpp
    src/config.cpp
    src/crypto.cpp
    src/protocol.cpp
    src/utils.cpp
)
target_include_directories(opico_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    target_compile_definitions(opico_common PUBLIC _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(opico_common PUBLIC ws2_32 bcrypt)
endif()

if(MSVC)
    target_compile_options(opico_common PUBLIC /W3 /utf-8)
else()
    target_compile_options(opico_common PUBLIC -Wall -Wextra -Wno-unused-parameter)
    if(NOT WIN32)
        target_compile_options(opico_common PUBLIC -pthread)
        target_link_options(opico_common PUBLIC -pthread)
    endif()
endif()

# Server
add_executable(opicochat_server src/server_main.cpp)
target_link_libraries(opicochat_server PRIVATE opico_common)
set_target_properties(opicochat_server PROPERTIES OUTPUT_NAME "opicochat_server")
if(WIN32)
    target_sources(opicochat_server PRIVATE src/opicochatserver.rc)
endif()

# Client
add_executable(opicochat_client src/client_main.cpp)
target_link_libraries(opicochat_client PRIVATE opico_common)
set_target_properties(opicochat_client PROPERTIES OUTPUT_NAME "opicochat")
if(WIN32)
    target_sources(opicochat_client PRIVATE src/opicochat.rc)
endif()

install(TARGETS opicochat_server opicochat_client RUNTIME DESTINATION bin)
