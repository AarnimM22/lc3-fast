cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rx_app_core)
set(AUDIO_CLOCK_STRESS 0 CACHE STRING "Inject PLL disturbances for clock servo testing")
zephyr_get(AUDIO_CLOCK_STRESS SYSBUILD)
message(STATUS "Audio clock stress=${AUDIO_CLOCK_STRESS}")
target_compile_definitions(app PRIVATE AUDIO_CLOCK_STRESS=${AUDIO_CLOCK_STRESS})

set(BENCH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBLC3 ${BENCH_ROOT}/../third_party/liblc3)
set(CUSTOM_DIR ${CMAKE_CURRENT_BINARY_DIR}/lc3-custom)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${BENCH_ROOT}/../../tools/prepare-lc3-custom.py
  ${BENCH_ROOT}/src/custom_sns.inc
  ${BENCH_ROOT}/src/custom_spec.inc)
execute_process(COMMAND ${PYTHON_EXECUTABLE}
  ${BENCH_ROOT}/../../tools/prepare-lc3-custom.py
  --source ${LIBLC3}/src --output ${CUSTOM_DIR}
  COMMAND_ERROR_IS_FATAL ANY)

file(GLOB CODEC_SOURCES ${CUSTOM_DIR}/*.c)
target_sources(app PRIVATE src/main.c
  ${CODEC_SOURCES}
  ${BENCH_ROOT}/src/lc3_custom.c
  ${BENCH_ROOT}/src/lc3_lite.c)
target_include_directories(app PRIVATE
  ${BENCH_ROOT}/src
  ${CUSTOM_DIR}
  ${LIBLC3}/include)
target_compile_definitions(app PRIVATE
  BENCH_OPTIMIZATIONS=0
  CODEC_LC3_CUSTOM=1
  CODEC_LC3_LITE=1)
set_source_files_properties(${CODEC_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3;-std=c11")
target_link_libraries(app PRIVATE m)
# Observe the stock I2S driver's DMA buffer releases without modifying the SDK.
zephyr_link_libraries(-Wl,--wrap=k_mem_slab_free)
