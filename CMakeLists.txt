cmake_minimum_required(VERSION 3.14)
project(logger LANGUAGES CXX)

include(FetchContent)

# --- Dependencies ---
# The logger is built on the already-extracted scheduler, which itself pulls in
# threadpool + stash (the lock-free hierarchical timer wheel). The async and
# deferred log paths run on that scheduler; the immediate path does not touch it.
# See ARCHITECTURE.md. The core library depends on NOTHING else: the richer bits
# (exception describe, real backtraces, named threads, timestamp formatting) come
# in through hooks with std defaults, so a consumer re-injects them via an adapter.
FetchContent_Declare(
  scheduler
  GIT_REPOSITORY https://github.com/Kronuz/scheduler.git
  GIT_TAG        a55596ff3ad58fb960676d0a07394f51813091b7
)
FetchContent_MakeAvailable(scheduler)

# --- Library target ---
add_library(logger STATIC logger.cc)
target_include_directories(logger PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(logger PUBLIC cxx_std_20)
target_link_libraries(logger PUBLIC scheduler::scheduler)
add_library(logger::logger ALIAS logger)

# Build the smoke test only when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(logger_test test/test.cc)
  target_link_libraries(logger_test PRIVATE logger)
  add_test(NAME logger COMMAND logger_test)
endif()
