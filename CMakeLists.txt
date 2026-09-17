# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)
project(org.webosports.service.eink C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC4)
webos_component(1 0 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)

include_directories(${GLIB2_INCLUDE_DIRS} ${LS2_INCLUDE_DIRS}
                    ${PBNJSON_C_INCLUDE_DIRS})

webos_add_compiler_flags(ALL -Wall -Wextra -Wno-unused-parameter)

add_executable(einkd src/main.c)
target_link_libraries(einkd ${GLIB2_LDFLAGS} ${LS2_LDFLAGS} ${PBNJSON_C_LDFLAGS})

webos_build_daemon(NAME einkd LAUNCH files/launch)
webos_build_system_bus_files()
