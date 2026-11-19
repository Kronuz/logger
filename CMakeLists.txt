cmake_minimum_required(VERSION 3.14)
project(logger LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# wheel: the single-owner timer wheel the LOG thread files deferred lines in (header-only).
FetchContent_Declare(
  wheel
  GIT_REPOSITORY https://github.com/Kronuz/wheel.git
  GIT_TAG        bfd72bc13395e4a232701e752be97ccd236530fc
)
FetchContent_MakeAvailable(wheel)

# workpool: its Thread runs the LOG thread (the richer host-specific bits -- exception describe,
# real backtraces, named threads -- come in through hooks with std defaults, so a consumer
# re-injects them via an adapter).
FetchContent_Declare(
  workpool
  GIT_REPOSITORY https://github.com/Kronuz/workpool.git
  GIT_TAG        640c66bfebccae811377670a265b62f4878ede8a
)
FetchContent_MakeAvailable(workpool)

# term-color: the compile-time stacked color palette (rgb()/CLEAR_COLOR) plus the
# runtime resolution layer (collapse / detect_depth / apply). The logger uses it for
# the severity markers, the gradient timestamp, and to gate + collapse every line to
# the terminal's depth (the NO_COLOR / --color policy). Pinned by commit.
FetchContent_Declare(
  term_color
  GIT_REPOSITORY https://github.com/Kronuz/term-color.git
  GIT_TAG        76a400be217eec6043510d5a812e368eb1898153
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
target_link_libraries(logger PUBLIC workpool::workpool term_color::term_color wheel::wheel)
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

  # Deferred lines on the LOG thread's wheel: once, in order, never early, within a tick plus a wake;
  # and the same under eight producers with cancelled and ring lines mixed in.
  add_executable(logger_deferred_test test/deferred.cc)
  target_link_libraries(logger_deferred_test PRIVATE logger)
  add_test(NAME logger_deferred COMMAND logger_deferred_test)
  add_test(NAME logger_deferred_stress COMMAND logger_deferred_test stress)
  add_test(NAME logger_deferred_ties COMMAND logger_deferred_test ties)

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

  # Exit without finish() while the LOG thread is stalled in a handler: the exit-time stop gives up
  # and the statics are destroyed; the LOG thread must not touch them then.
  add_executable(logger_exit_test test/exit.cc)
  target_link_libraries(logger_exit_test PRIVATE logger)
  add_test(NAME logger_exit COMMAND logger_exit_test)
  # Exit with `once` lines queued: the once filter, made after the exit-time stop, is destroyed first.
  add_test(NAME logger_exit_once COMMAND logger_exit_test once)
  # A static destroyed after config, hooks and handlers logs: its lines go to stderr as they are.
  add_test(NAME logger_exit_late COMMAND logger_exit_test late)
  # Exit with a slow drain (400 lines at 1 ms): the stop waits while lines are written.
  add_test(NAME logger_exit_slow COMMAND logger_exit_test slow)
  # Exit with a steady drain past the 5 s cap: the stop cuts it and reports the lines it cut.
  add_test(NAME logger_exit_capped COMMAND logger_exit_test capped)
  # Exit in a fork child, and exit() on the LOG thread: the stop must not wait for a thread that
  # is not there or is its own caller.
  add_test(NAME logger_exit_fork COMMAND logger_exit_test fork)
  add_test(NAME logger_exit_logexit COMMAND logger_exit_test logexit)

  # An exception logged on one thread and destroyed by it after the LOG thread wrote the line:
  # the line carries the description made at the call (TSan flags a LOG thread that touches it).
  add_executable(logger_exception_test test/exception.cc)
  target_link_libraries(logger_exception_test PRIVATE logger)
  add_test(NAME logger_exception COMMAND logger_exception_test)

  # One scripted run must write the golden bytes (test/transcript.golden), which the implementation
  # before the producer path changed wrote: the path may change, never what is written.
  add_executable(logger_transcript test/transcript.cc)
  target_link_libraries(logger_transcript PRIVATE logger)
  add_test(NAME logger_transcript
    COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:logger_transcript> -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/test/transcript.golden
      -DDIR=${CMAKE_CURRENT_BINARY_DIR}/transcript -P "${CMAKE_CURRENT_SOURCE_DIR}/test/transcript.cmake")
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

# logger's scheduling on its own queues against a prototype on Kronuz/wheel (opt-in).
# docs/perf/wheel-ab.md has the method and the rule; bench/wheel_ab/run.sh runs it.
option(LOGGER_BUILD_WHEEL_AB "Build bench/wheel_ab" OFF)
if(LOGGER_BUILD_WHEEL_AB AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(logger_wheel_ab bench/wheel_ab/ab.cc)
  target_link_libraries(logger_wheel_ab PRIVATE logger wheel::wheel)
  foreach(backend A B)
    foreach(pattern 1 2 3)
      add_test(NAME logger_wheel_ab_check_${backend}${pattern}
        COMMAND logger_wheel_ab --backend ${backend} --pattern ${pattern} --producers 4 --check --rounds 1 --bursts 3)
    endforeach()
  endforeach()
endif()
