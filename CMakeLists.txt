cmake_minimum_required(VERSION 3.16)
project(opicochat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common library
add_library(opico_common
    src/common.cpp
    src/config.cpp
    src/protocol.cpp
    src/utils.cpp
)

target_include_directories(opico_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (WIN32)
    target_compile_definitions(opico_common PUBLIC _WIN32_WINNT=0x0601 NOMINMAX)
endif()

# Server executable
add_executable(opicochat_server src/server_main.cpp)

target_link_libraries(opicochat_server PRIVATE opico_common)

if (WIN32)
    target_link_libraries(opicochat_server PRIVATE ws2_32)
endif()

# Client executable
add_executable(opicochat_client src/client_main.cpp)

target_link_libraries(opicochat_client PRIVATE opico_common)

if (WIN32)
    target_link_libraries(opicochat_client PRIVATE ws2_32)
endif()

# Install rules (optional)
install(TARGETS opicochat_server opicochat_client RUNTIME DESTINATION bin)
# opicochat customizations: exe names + Windows icons
if(TARGET opicochat_client)
  # Make the client binary name just "opicochat"
  set_target_properties(opicochat_client PROPERTIES OUTPUT_NAME "opicochat")
  if(WIN32)
    target_sources(opicochat_client PRIVATE src/opicochat.rc)
  endif()
endif()

if(TARGET opicochat_server)
  # Ensure server binary is "opicochat_server" everywhere
  set_target_properties(opicochat_server PROPERTIES OUTPUT_NAME "opicochat_server")
  if(WIN32)
    target_sources(opicochat_server PRIVATE src/opicochatserver.rc)
  endif()
endif()
