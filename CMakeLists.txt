DISABLE_MISSING_PROFILE_WARNING()

INCLUDE_DIRECTORIES(SYSTEM ${CMAKE_SOURCE_DIR}/extra/rapidjson/include ${CMAKE_SOURCE_DIR}/extra/zlib/zlib-1.2.1) 

MYSQL_ADD_COMPONENT(profiler
  profiler.cc
  MODULE_ONLY
  TEST_ONLY
  # LINK_LIBRARIES extra:rapidjson
)

MYSQL_ADD_COMPONENT(profiler_cpu
  cpu.cc
  MODULE_ONLY
  TEST_ONLY
  #  LINK_LIBRARIES profiler
)

MYSQL_ADD_COMPONENT(profiler_memory
  memory.cc
  MODULE_ONLY
  TEST_ONLY
  #  LINK_LIBRARIES tcmalloc
)
