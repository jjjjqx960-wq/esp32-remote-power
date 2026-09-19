cmake_minimum_required(VERSION 3.16)

set(RELAY_ACTIVE_LOW "1" CACHE STRING "1 = L jumper, 0 = H jumper")
if(NOT RELAY_ACTIVE_LOW MATCHES "^[01]$")
    message(FATAL_ERROR "RELAY_ACTIVE_LOW must be 0 or 1")
endif()
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_remote_power)
