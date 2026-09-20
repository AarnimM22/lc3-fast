set(SBC_REF ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libsbc)
# The upstream dual-channel encoder calls compute_scale_factors twice even
# though that function already visits both channels. The second call reads and
# writes past two-channel arrays. Remove only that redundant call in a copy.
file(READ ${SBC_REF}/src/sbc.c SBC_PORT)
set(SBC_BAD "    if (frame->mode == SBC_MODE_DUAL_CHANNEL)\n        compute_scale_factors(frame, sb_samples + 1, scale_factors + 1);")
string(FIND "${SBC_PORT}" "${SBC_BAD}" SBC_BAD_POS)
if(SBC_BAD_POS LESS 0)
  message(FATAL_ERROR "Expected SBC dual-channel fix site not found")
endif()
string(REPLACE "${SBC_BAD}" "    /* Both channels were already processed above. */" SBC_PORT "${SBC_PORT}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sbc_port.c "${SBC_PORT}")
set(CODEC_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/sbc_port.c ${SBC_REF}/src/bits.c)
target_sources(app PRIVATE ${CODEC_SOURCES} src/codec_sbc.c)
target_include_directories(app PRIVATE ${SBC_REF}/include ${SBC_REF}/src)
target_compile_definitions(app PRIVATE CODEC_SBC=1)
