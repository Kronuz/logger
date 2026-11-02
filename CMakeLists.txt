cmake_minimum_required(VERSION 3.14)
project(logger LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# The logger is built on the already-extracted scheduler, which itself pulls in
# threadpool + stash (the lock-free hierarchical timer wheel). The async and
# deferred log paths run on that scheduler; the immediate path does not touch it.
# See ARCHITECTURE.md. The richer host-specific bits (exception describe, real
# backtraces, named threads) still come in through hooks with std defaults, so a
# consumer re-injects them via an adapter.
FetchContent_Declare(
  scheduler
  GIT_REPOSITORY https://github.com/Kronuz/scheduler.git
  GIT_TAG        1e5ccc9c6dfc62611751dae2143467ef00305fc3
)
FetchContent_MakeAvailable(scheduler)

# term-color: the compile-time stacked color palette (rgb()/CLEAR_COLOR) plus the
# runtime resolution layer (collapse / detect_depth / apply). The logger uses it for
# the severity markers, the gradient timestamp, and to gate + collapse every line to
# the terminal's depth (the NO_COLOR / --color policy). Pinned by commit.
FetchContent_Declare(
  term_color
  GIT_REPOSITORY https://github.com/Kronuz/term-color.git
  GIT_TAG        94df6e5e1cadebbba45cd1f5fbb679407550be56
)
FetchContent_MakeAvailable(term_color)

# cppcodec: a dedicated base64 codec (base64_rfc4648), used only in logger.cc for the
# iTerm2 badge escape. Header-only; SOURCE_SUBDIR points at a non-existent dir so its
# CMakeLists (which requests an old CMake policy) is not run -- we only need headers.
# Pinned to the commit its master names (2022-09-06; the last tag, v0.2, is from 2018).
if(NOT DEFINED cppcodec_SOURCE_DIR)
  FetchContent_Declare(cppcodec
    GIT_REPOSITORY https://github.com/tplgy/cppcodec.git
    GIT_TAG        8019b8b580f8573c33c50372baec7039dfe5a8ce
    SOURCE_SUBDIR  do-not-configure)
  FetchContent_MakeAvailable(cppcodec)
endif()

# --- Library target ---
add_library(logger STATIC logger.cc)
target_include_directories(logger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(logger PRIVATE "${cppcodec_SOURCE_DIR}")
target_compile_features(logger PUBLIC cxx_std_20)
target_link_libraries(logger PUBLIC scheduler::scheduler term_color::term_color)
add_library(logger::logger ALIAS logger)

# Build the smoke test only when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(logger_test test/test.cc)
  target_link_libraries(logger_test PRIVATE logger)
  add_test(NAME logger COMMAND logger_test)
  add_executable(logger_tint_test test/tint.cc)
  target_link_libraries(logger_tint_test PRIVATE logger)
  add_test(NAME logger_tint COMMAND logger_tint_test)
  add_test(NAME logger_tint_off COMMAND logger_tint_test off)

  add_executable(logger_timestamp_test test/timestamp.cc)
  target_link_libraries(logger_timestamp_test PRIVATE logger)
  add_test(NAME logger_timestamp COMMAND logger_timestamp_test)

  # Ordering of lines due within, and across, the wheel's 1 ms tick.
  add_executable(logger_order_test test/order.cc)
  target_link_libraries(logger_order_test PRIVATE logger)
  add_test(NAME logger_order COMMAND logger_order_test)

  # The producer path's own clocks against the standard ones.
  add_executable(logger_clock_test test/clock.cc)
  target_link_libraries(logger_clock_test PRIVATE logger)
  add_test(NAME logger_clock COMMAND logger_clock_test)

  add_executable(logger_demo examples/demo.cc)
  target_link_libraries(logger_demo PRIVATE logger)

  # The macros' formatter against std::vformat, on random formats and arguments.
  add_executable(logger_format_test test/format.cc)
  target_link_libraries(logger_format_test PRIVATE logger)
  add_test(NAME logger_format COMMAND logger_format_test)

  # Many producers against the LOG thread: exactly-once, order across threads, counters, and
  # a finish() that races the producers (each round is its own process).
  add_executable(logger_producer_test test/producer.cc)
  target_link_libraries(logger_producer_test PRIVATE logger)
  add_test(NAME logger_producer COMMAND logger_producer_test)

  # How soon a routine line is written: at once when nothing is pending, in the 1 ms batch otherwise.
  add_executable(logger_wake_test test/wake.cc)
  target_link_libraries(logger_wake_test PRIVATE logger)
  add_test(NAME logger_wake COMMAND logger_wake_test)
  # A latency bound: alone, so another test's threads do not delay the LOG thread's wakeup.
  set_tests_properties(logger_wake PROPERTIES RUN_SERIAL TRUE)

  # The per-thread queues: exactly once and in order from 4 to 16 producers, order between threads,
  # the sleep handshake, threads that end, exit without finish(), finish() racing 16 producers.
  add_executable(logger_queue_test test/queue.cc)
  target_link_libraries(logger_queue_test PRIVATE logger)
  add_test(NAME logger_queue COMMAND logger_queue_test)
  set_tests_properties(logger_queue PROPERTIES TIMEOUT 600)

  # Differential test: one scripted run, built against the library and against the frozen copy
  # of the implementation before the producer path changed (test/frozen), must write identical bytes.
  # The frozen copy publishes `Logging::config` to the LOG thread with relaxed counters, so the
  # test's own configuration writes race there by construction: ThreadSanitizer builds skip it
  # (CI passes the flags through the per-config and linker variables).
  string(TOUPPER "${CMAKE_BUILD_TYPE}" LOGGER_BUILD_TYPE)
  set(LOGGER_ALL_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${LOGGER_BUILD_TYPE}} ${CMAKE_EXE_LINKER_FLAGS}")
  if(NOT LOGGER_ALL_FLAGS MATCHES "-fsanitize=thread")
    add_executable(logger_transcript_new test/transcript.cc)
    target_link_libraries(logger_transcript_new PRIVATE logger)
    add_executable(logger_transcript_old test/transcript.cc test/frozen/logger.cc)
    target_include_directories(logger_transcript_old BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test/frozen")
    target_include_directories(logger_transcript_old PRIVATE "${cppcodec_SOURCE_DIR}")
    target_link_libraries(logger_transcript_old PRIVATE scheduler::scheduler term_color::term_color)
    add_test(NAME logger_transcript
      COMMAND "${CMAKE_COMMAND}" -DNEW=$<TARGET_FILE:logger_transcript_new> -DOLD=$<TARGET_FILE:logger_transcript_old>
        -DDIR=${CMAKE_CURRENT_BINARY_DIR}/transcript -P "${CMAKE_CURRENT_SOURCE_DIR}/test/transcript.cmake")
  endif()
endif()

# Producer-cost benchmark (opt-in): fetches fmt for the synchronous comparator, and with
# LOGGER_BENCH_QUILL the asynchronous comparator quill, at a release tag.
# Nothing here is a dependency of the library.
option(LOGGER_BUILD_BENCH "Build bench/lg.cc" OFF)
option(LOGGER_BENCH_QUILL "Time quill in bench/lg.cc (fetched)" OFF)
if(LOGGER_BUILD_BENCH AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1)
  FetchContent_MakeAvailable(fmt)
  add_executable(logger_bench bench/lg.cc)
  target_link_libraries(logger_bench PRIVATE logger fmt::fmt)
  if(LOGGER_BENCH_QUILL)
    FetchContent_Declare(quill
      GIT_REPOSITORY https://github.com/odygrd/quill.git
      GIT_TAG        v13.1.0
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(quill)
    target_link_libraries(logger_bench PRIVATE quill::quill)
    target_compile_definitions(logger_bench PRIVATE LOGGER_BENCH_QUILL)
  endif()
  add_test(NAME logger_bench_check COMMAND logger_bench --check)
  # Where a producer's time goes (each piece alone), a profiling loop, the LOG thread's drain over time,
  # and one isolated line's latency.
  add_executable(logger_parts bench/parts.cc)
  target_compile_features(logger_parts PRIVATE cxx_std_20)   # it links nothing that would request it
  add_executable(logger_steady bench/steady.cc)
  target_link_libraries(logger_steady PRIVATE logger)
  add_executable(logger_drain bench/drain.cc)
  target_link_libraries(logger_drain PRIVATE logger)
  # How long one isolated line takes from the call to its handler.
  add_executable(logger_latency bench/latency.cc)
  target_link_libraries(logger_latency PRIVATE logger)
endif()
