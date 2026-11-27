# Warnings: GCC and Clang's, or MSVC's
if(MSVC)
  set(G2M_WARNINGS /W3 CACHE INTERNAL "")
  set(G2M_WARNINGS_PEDANTIC /W3 CACHE INTERNAL "")
else()
  set(G2M_WARNINGS -Wall -Wextra CACHE INTERNAL "")
  set(G2M_WARNINGS_PEDANTIC -Wall -Wextra -Wpedantic CACHE INTERNAL "")
endif()

add_library(g2m_core STATIC
  src/json.cpp
  src/model_file.cpp
  src/network.cpp
  src/transcriber.cpp
  src/string_allocator.cpp
  src/mpe.cpp
  src/sustain.cpp
  src/bend_mode.cpp
  src/resampler.cpp
)
target_include_directories(g2m_core
  PUBLIC include
  PRIVATE src
)
# linked into plugin shared objects
set_target_properties(g2m_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(g2m_core PRIVATE ${G2M_WARNINGS_PEDANTIC})

# Tests: one executable per file, sharing a small check harness (test/check.h)
add_library(g2m_check STATIC test/check_main.cpp)
target_include_directories(g2m_check PUBLIC test)

foreach(name json model_file network transcriber note_name string_allocator mpe noise_gate sustain bend_mode resampler)
  add_executable(test_${name} test/test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE g2m_core g2m_check)
  target_include_directories(test_${name} PRIVATE src)
  target_compile_options(test_${name} PRIVATE ${G2M_WARNINGS})
  target_compile_definitions(test_${name} PRIVATE G2M_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Command-line tools: g2m-transcribe (WAV → MIDI file) and g2m-bench (time per block)
add_library(g2m_tools_io STATIC tools/audio_io.cpp)
target_include_directories(g2m_tools_io PUBLIC tools)
target_link_libraries(g2m_tools_io PUBLIC g2m_core)
target_compile_options(g2m_tools_io PRIVATE ${G2M_WARNINGS_PEDANTIC})

foreach(tool transcribe bench)
  add_executable(g2m-${tool} tools/g2m_${tool}.cpp)
  target_link_libraries(g2m-${tool} PRIVATE g2m_tools_io)
  target_compile_options(g2m-${tool} PRIVATE ${G2M_WARNINGS_PEDANTIC})
endforeach()
target_include_directories(g2m-transcribe PRIVATE src)  # the decoder, for --decode

add_executable(test_audio_io test/test_audio_io.cpp)
target_link_libraries(test_audio_io PRIVATE g2m_tools_io g2m_check)
target_compile_options(test_audio_io PRIVATE ${G2M_WARNINGS})
add_test(NAME audio_io COMMAND test_audio_io)

# g2m-transcribe on the test WAV must write reference.py's MIDI file byte for byte
add_test(NAME transcribe_cli
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/small.mid -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.mid
    "-DCOMMAND=$<TARGET_FILE:g2m-transcribe>|--model|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.g2m|--block|100|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.wav|-o|${CMAKE_CURRENT_BINARY_DIR}/small.mid"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_and_compare.cmake)

# its decoder and MPE writer on the reference's outputs for the version 4 test model must write reference.py
# --mpe's file byte for byte (the WAV only gives the sample rate)
add_test(NAME transcribe_cli_mpe
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/bend.mpe.mid -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.mpe.mid
    "-DCOMMAND=$<TARGET_FILE:g2m-transcribe>|--model|${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.g2m|--decode|${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.outputs.f32|--mpe|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.wav|-o|${CMAKE_CURRENT_BINARY_DIR}/bend.mpe.mid"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_and_compare.cmake)

# --bend-mode natural in MPE output and off in plain output change nothing: the same files
add_test(NAME transcribe_cli_bend_mode_mpe
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/bend.natural.mpe.mid -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.mpe.mid
    "-DCOMMAND=$<TARGET_FILE:g2m-transcribe>|--model|${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.g2m|--decode|${CMAKE_CURRENT_SOURCE_DIR}/test/data/bend.outputs.f32|--mpe|--bend-mode|natural|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.wav|-o|${CMAKE_CURRENT_BINARY_DIR}/bend.natural.mpe.mid"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_and_compare.cmake)
add_test(NAME transcribe_cli_bend_mode_plain
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/small.off.mid -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.mid
    "-DCOMMAND=$<TARGET_FILE:g2m-transcribe>|--model|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.g2m|--block|100|--bend-mode|off|${CMAKE_CURRENT_SOURCE_DIR}/test/data/small.wav|-o|${CMAKE_CURRENT_BINARY_DIR}/small.off.mid"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_and_compare.cmake)
