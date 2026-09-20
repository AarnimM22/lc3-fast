# Same application-core startup image used by Nordic's ESB PRX sample.
if(benchmark_BENCH_CODEC MATCHES "-lto$")
  set_config_bool(benchmark CONFIG_ISR_TABLES_LOCAL_DECLARATION y)
  set_config_bool(benchmark CONFIG_LTO y)
endif()
if(benchmark_BENCH_CODEC MATCHES "^opus")
  set_config_int(benchmark CONFIG_MAIN_STACK_SIZE 49152)
endif()
if(SB_CONFIG_SOC_NRF5340_CPUNET)
  ExternalZephyrProject_Add(
    APPLICATION rx_app_core
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/app_core
    BOARD nrf5340dk/nrf5340/cpuapp)
  set_property(GLOBAL APPEND PROPERTY PM_DOMAINS CPUAPP)
  set_property(GLOBAL APPEND PROPERTY PM_CPUAPP_IMAGES rx_app_core)
  set_property(GLOBAL PROPERTY DOMAIN_APP_CPUAPP rx_app_core)
  set(CPUAPP_PM_DOMAIN_DYNAMIC_PARTITION rx_app_core CACHE INTERNAL "")
endif()
