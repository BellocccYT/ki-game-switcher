cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Das Plugin braucht beides; die Schalter bleiben für die CMake-Hilfsskripte der Vorlage erhalten
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(CURL REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets CURL::libcurl
)

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE user32)
endif()

# Lokale Spracherkennung mit whisper.cpp (unter macOS vorerst aus: Universal-Binary mit ggml ungetestet)
if(APPLE)
  option(ENABLE_VOICE "Lokale Spracherkennung mit whisper.cpp" OFF)
else()
  option(ENABLE_VOICE "Lokale Spracherkennung mit whisper.cpp" ON)
endif()
if(ENABLE_VOICE)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/whisper.cmake")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE KGS_VOICE)
endif()

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/settings.cpp
    src/settings.hpp
    src/http.cpp
    src/http.hpp
    src/frame-grabber.cpp
    src/frame-grabber.hpp
    src/game-db.cpp
    src/game-db.hpp
    src/window-hint.cpp
    src/window-hint.hpp
    src/capture-info.cpp
    src/capture-info.hpp
    src/ai-client.cpp
    src/ai-client.hpp
    src/twitch.cpp
    src/twitch.hpp
    src/switcher.cpp
    src/switcher.hpp
    src/title.cpp
    src/title.hpp
    src/voice.cpp
    src/voice.hpp
    src/dock.cpp
    src/dock.hpp
    src/settings-dialog.cpp
    src/settings-dialog.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
