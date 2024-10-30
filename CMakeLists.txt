DISABLE_MISSING_PROFILE_WARNING()
INCLUDE(mysql_version) 
IF(${MAJOR_VERSION}  STREQUAL "8")
   SET(ZLIB_EXTRA_VER, "zlib-1.2.13") 
ELSE()
   SET(ZLIB_EXTRA_VER, "zlib-1.3.1") 
ENDIF()

INCLUDE_DIRECTORIES(SYSTEM ${CMAKE_SOURCE_DIR}/extra/rapidjson/include ${CMAKE_BINARY_DIR}/extra/zlib/${ZLIB_EXTRA_VER}) 

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

MYSQL_ADD_COMPONENT(profiler_jemalloc_memory
  jemalloc_memory.cc
  MODULE_ONLY
  TEST_ONLY
  #  LINK_LIBRARIES tcmalloc
)

