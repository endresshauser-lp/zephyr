# SPDX-License-Identifier: Apache-2.0

if(EXISTS ${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/gnu/target.cmake)
  include(${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/gnu/target.cmake)
else()
  include(${ZEPHYR_SDK_INSTALL_DIR}/cmake/zephyr/target.cmake)
endif()
