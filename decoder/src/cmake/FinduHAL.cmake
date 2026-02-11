### @author: Roberto Preghenella
### @email: preghenella@bo.infn.it

find_path(uHAL_INCLUDE_DIR
  NAMES uhal.hpp
  PATH_SUFFIXES uhal
  PATHS /opt/cactus/include)

set(uHAL_INCLUDE_DIR ${uHAL_INCLUDE_DIR}/..)

find_library(uHAL_LIBRARIES
  NAMES libcactus_uhal_uhal.so
  PATHS /opt/cactus/lib)

find_library(uHAL_LOG_LIBRARIES
  NAMES libcactus_uhal_log.so
  PATHS /opt/cactus/lib)

find_package_handle_standard_args(uHAL
  REQUIRED_VARS uHAL_INCLUDE_DIR uHAL_LIBRARIES uHAL_LOG_LIBRARIES
  FAIL_MESSAGE "uHAL could not be found")
