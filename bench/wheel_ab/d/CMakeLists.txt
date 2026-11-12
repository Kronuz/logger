# Arm (D) of docs/perf/wheel-ab.md, built apart from the library's own CMakeLists: this project
# builds (A) (logger as it is, from the repository root) and (D) (d/logger.{h,cc}) against
# Kronuz/wheel at the commit pinned below. bench/wheel_ab/run.sh build-d runs it.
cmake_minimum_required(VERSION 3.14)
project(logger_wheel_ab_d LANGUAGES CXX)
include(FetchContent)

FetchContent_Declare(wheel
  GIT_REPOSITORY https://github.com/Kronuz/wheel.git
  GIT_TAG        45f98eb05e669ed39544f2b7059d2ac57df4333b)
FetchContent_MakeAvailable(wheel)

# The library's dependencies, at the library's pins (the root project fetches them once here).
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
FetchContent_Declare(cppcodec
  GIT_REPOSITORY https://github.com/tplgy/cppcodec.git
  GIT_TAG        8019b8b580f8573c33c50372baec7039dfe5a8ce
  SOURCE_SUBDIR  do-not-configure)
FetchContent_MakeAvailable(cppcodec)
add_subdirectory("${ROOT}" logger EXCLUDE_FROM_ALL)

# (A) and (B): the harness against the library as it is.
add_executable(logger_wheel_ab "${ROOT}/bench/wheel_ab/ab.cc")
target_link_libraries(logger_wheel_ab PRIVATE logger wheel::wheel)

# (D): the same harness against this directory's copy of logger.
add_executable(logger_wheel_ab_d "${ROOT}/bench/wheel_ab/ab.cc" logger.cc)
target_include_directories(logger_wheel_ab_d BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(logger_wheel_ab_d PRIVATE "${cppcodec_SOURCE_DIR}")
target_compile_features(logger_wheel_ab_d PRIVATE cxx_std_20)
target_compile_definitions(logger_wheel_ab_d PRIVATE WAB_ARM_D)
target_link_libraries(logger_wheel_ab_d PRIVATE scheduler::scheduler term_color::term_color wheel::wheel)

# (D) with the LOG thread's counters printed to stderr when it closes (`run.sh stats-d`).
add_executable(logger_wheel_ab_d_stats "${ROOT}/bench/wheel_ab/ab.cc" logger.cc)
target_include_directories(logger_wheel_ab_d_stats BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(logger_wheel_ab_d_stats PRIVATE "${cppcodec_SOURCE_DIR}")
target_compile_features(logger_wheel_ab_d_stats PRIVATE cxx_std_20)
target_compile_definitions(logger_wheel_ab_d_stats PRIVATE WAB_ARM_D WAB_STATS)
target_link_libraries(logger_wheel_ab_d_stats PRIVATE scheduler::scheduler term_color::term_color wheel::wheel)

# Sizes of the record in each arm, printed by `run.sh sizeof`.
add_executable(sizeof_a "${ROOT}/bench/wheel_ab/sizeof.cc")
target_link_libraries(sizeof_a PRIVATE logger)
add_executable(sizeof_c "${ROOT}/bench/wheel_ab/sizeof.cc")
target_include_directories(sizeof_c BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../c")
target_include_directories(sizeof_c PRIVATE "${cppcodec_SOURCE_DIR}")
target_compile_features(sizeof_c PRIVATE cxx_std_20)
target_link_libraries(sizeof_c PRIVATE scheduler::scheduler term_color::term_color wheel::wheel)
add_executable(sizeof_d "${ROOT}/bench/wheel_ab/sizeof.cc")
target_include_directories(sizeof_d BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(sizeof_d PRIVATE "${cppcodec_SOURCE_DIR}")
target_compile_features(sizeof_d PRIVATE cxx_std_20)
target_link_libraries(sizeof_d PRIVATE scheduler::scheduler term_color::term_color wheel::wheel)
