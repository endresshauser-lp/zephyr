# SPDX-License-Identifier: Apache-2.0

if(EXISTS ${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/gnu/generic.cmake)
  include(${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/gnu/generic.cmake)
else()
  include(${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/generic.cmake)
endif()

set(TOOLCHAIN_KCONFIG_DIR ${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr)

message(STATUS "Found toolchain: zephyr ${SDK_VERSION} (${ZEPHYR_SDK_INSTALL_DIR})")
