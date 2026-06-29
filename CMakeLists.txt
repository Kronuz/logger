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
  GIT_TAG        main  # track tip
)
FetchContent_MakeAvailable(scheduler)

# term-color: the compile-time stacked color palette (rgb()/CLEAR_COLOR) plus the
# runtime resolution layer (collapse / detect_depth / apply). The logger uses it for
# the severity markers, the gradient timestamp, and to gate + collapse every line to
# the terminal's depth (the NO_COLOR / --color policy). Tracks the term-color
# default branch (tip).
FetchContent_Declare(
  term_color
  GIT_REPOSITORY https://github.com/Kronuz/term-color.git
  GIT_TAG        main  # track tip
)
FetchContent_MakeAvailable(term_color)

# --- Library target ---
add_library(logger STATIC logger.cc)
target_include_directories(logger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
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

  add_executable(logger_demo examples/demo.cc)
  target_link_libraries(logger_demo PRIVATE logger)
endif()
