# Build upstream's source groups with Zephyr's Cortex-M4F ABI and LTO flags.
# No codec source modifications. No desktop CPU detection or NEON on Cortex-M4.
set(REF ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/opus)
include(${REF}/cmake/OpusFunctions.cmake)
get_opus_sources(CELT_SOURCES ${REF}/celt_sources.mk celt_sources)
get_opus_sources(OPUS_SOURCES ${REF}/opus_sources.mk opus_sources)
get_opus_sources(OPUS_SOURCES_FLOAT ${REF}/opus_sources.mk opus_sources_float)
get_opus_sources(SILK_SOURCES ${REF}/silk_sources.mk silk_sources)
if(BENCH_CODEC MATCHES "^opus-fixed")
  get_opus_sources(SILK_SOURCES_FIXED ${REF}/silk_sources.mk silk_variant)
  target_include_directories(app PRIVATE ${REF}/silk/fixed)
  target_compile_definitions(app PRIVATE FIXED_POINT=1
    OPUS_ARM_INLINE_EDSP=1 OPUS_ARM_INLINE_MEDIA=1)
  if(NOT BENCH_CODEC STREQUAL "opus-fixed16-lto")
    target_compile_definitions(app PRIVATE ENABLE_RES24=1)
  endif()
else()
  get_opus_sources(SILK_SOURCES_FLOAT ${REF}/silk_sources.mk silk_variant)
  target_include_directories(app PRIVATE ${REF}/silk/float)
  target_compile_definitions(app PRIVATE FLOAT_APPROX=1)
endif()
set(CODEC_SOURCES ${celt_sources} ${opus_sources} ${opus_sources_float} ${silk_sources} ${silk_variant})
list(TRANSFORM CODEC_SOURCES PREPEND "${REF}/")
target_sources(app PRIVATE ${CODEC_SOURCES} src/codec_opus.c)
target_include_directories(app PRIVATE ${REF}/include ${REF}/celt ${REF}/silk ${REF}/dnn)
target_compile_definitions(app PRIVATE CODEC_OPUS=1 OPUS_BUILD=1 VAR_ARRAYS=1
  HAVE_LRINTF=1 HAVE_LRINT=1 ENABLE_HARDENING=1 DISABLE_DEBUG_FLOAT=1
  PACKAGE_VERSION="1.6.1")
