# whisper.cpp für die lokale Spracherkennung (statisch ins Plugin gelinkt)
#
# Die Vorlage setzt projektweit strenge Compiler-Optionen (Warnungen als Fehler, UNICODE, /permissive- …).
# Für whisper.cpp und ggml werden sie vorübergehend ausgesetzt, damit fremder Code nicht daran scheitert.

include(FetchContent)

FetchContent_Declare(
  whisper
  URL https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.4.tar.gz
  EXCLUDE_FROM_ALL
)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
set(WHISPER_FFMPEG OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE) # portable Binärdatei statt Optimierung für den Build-Rechner
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)

get_directory_property(_kgs_compile_options COMPILE_OPTIONS)
get_directory_property(_kgs_compile_definitions COMPILE_DEFINITIONS)
get_directory_property(_kgs_link_options LINK_OPTIONS)
set(_kgs_warning_as_error ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(_kgs_shared_libs ${BUILD_SHARED_LIBS})
# whisper.cpp und ggml verlangen eine alte CMake-Version; ohne CMP0077 überschreibt ihr option(BUILD_SHARED_LIBS)
# unsere Vorgabe und unter Linux entstünde eine libwhisper.so, die das Plugin-Paket nicht mitbringt
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set_directory_properties(
  PROPERTIES COMPILE_OPTIONS "" COMPILE_DEFINITIONS "" LINK_OPTIONS ""
)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(BUILD_SHARED_LIBS OFF)
# Die Vorlage setzt „x64,version=…“ – ggml erkennt die Architektur nur am reinen Plattformnamen
set(_kgs_generator_platform "${CMAKE_GENERATOR_PLATFORM}")
if(CMAKE_GENERATOR_PLATFORM MATCHES "^([^,]+),")
  set(CMAKE_GENERATOR_PLATFORM "${CMAKE_MATCH_1}")
endif()
if(MSVC)
  add_compile_options(/utf-8 /MP)
else()
  add_compile_options(-w)
endif()

FetchContent_MakeAvailable(whisper)

set_directory_properties(
  PROPERTIES
    COMPILE_OPTIONS "${_kgs_compile_options}"
    COMPILE_DEFINITIONS "${_kgs_compile_definitions}"
    LINK_OPTIONS "${_kgs_link_options}"
)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_kgs_warning_as_error})
set(BUILD_SHARED_LIBS ${_kgs_shared_libs})
set(CMAKE_GENERATOR_PLATFORM "${_kgs_generator_platform}")

# Statische Bibliotheken landen im Plugin – sie müssen positionsunabhängig sein (Linux)
foreach(_kgs_target IN ITEMS whisper ggml ggml-base ggml-cpu)
  if(TARGET ${_kgs_target})
    set_target_properties(${_kgs_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
endforeach()
