set(WP_REF ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/wavpack-stream)
set(CODEC_SOURCES)
foreach(unit common_utils decorr_tables decorr_utils entropy_utils extra1 extra2
             pack pack_dns pack_floats pack_utils write_words)
  list(APPEND CODEC_SOURCES ${WP_REF}/src/${unit}.c)
endforeach()
target_sources(app PRIVATE ${CODEC_SOURCES} src/codec_wavpack.c src/radio_async.c)
target_include_directories(app PRIVATE ${WP_REF}/include ${WP_REF}/src)
target_compile_definitions(app PRIVATE CODEC_WAVPACK=1 BENCH_HAS_ASYNC=1)
