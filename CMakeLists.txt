# scoring · 方案评分与推荐引擎（独立构建）
#
# 契约依据：include/scoring/scoring.h（唯一公开头）
# 需求依据：docs/需求/scoring需求专篇.md（SCD-* 共 34 条）/ SCD-NFR-02（独立交付：一条命令构建 → 一条命令验收）
# 上游共享契约：../phase-engine/docs/契约/protocol.md v1.0、../phase-engine/docs/契约/冲突裁决.md（C1/C2/C15/C16/C17）
#
# 目标：
#   scoring      静态库（宿主唯一依赖）
#   example_*    examples/ 下的示例
#   selftest     tests/ 下的零依赖自测（注册为 ctest 用例）
#
# 依赖：仅 nlohmann/json（优先系统包，找不到时回落内置单头 third_party/nlohmann/json.hpp）。
# 本模块**永远不需要** Web 框架 / SQL / 其它引擎（protocol.md P1/P2/P3、SCD-NFR-01）。
cmake_minimum_required(VERSION 3.20)
project(scoring
  VERSION 0.1.0
  DESCRIPTION "方案评分与推荐引擎：候选生成 / 归一加权评分 / 排序推荐 / 可解释输出 / 自动优化 / 采纳确认裁决"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------- 选项
option(SCORING_BUILD_TESTS    "编译 tests/（零依赖自测）" ON)
option(SCORING_BUILD_EXAMPLES "编译 examples/（最小示例、可复算、全流程、决策）" ON)
option(SCORING_WARNINGS_AS_ERRORS "把编译告警当错误" OFF)

# ---------------------------------------------------------------- 依赖
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  set(SCORING_JSON_TARGET nlohmann_json::nlohmann_json)
  message(STATUS "[scoring] nlohmann/json: 使用已安装的包")
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(FATAL_ERROR
      "[scoring] 未找到 nlohmann/json：既没有安装包，也没有内置副本 "
      "third_party/nlohmann/json.hpp。请安装 nlohmann-json 或放入内置副本。")
  endif()
  add_library(scoring_nlohmann INTERFACE)
  target_include_directories(scoring_nlohmann INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  set(SCORING_JSON_TARGET scoring_nlohmann)
  message(STATUS "[scoring] nlohmann/json: 使用内置单头副本 third_party/nlohmann/json.hpp")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- 库
file(GLOB_RECURSE SCORING_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

add_library(scoring STATIC ${SCORING_SOURCES})
add_library(scoring::scoring ALIAS scoring)

target_include_directories(scoring
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(scoring
  PUBLIC  ${SCORING_JSON_TARGET}
  PRIVATE Threads::Threads)

target_compile_definitions(scoring PUBLIC
  SCORING_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(scoring PRIVATE /utf-8 /W4 /permissive-)
  target_compile_definitions(scoring PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS)
  if(SCORING_WARNINGS_AS_ERRORS)
    target_compile_options(scoring PRIVATE /WX)
  endif()
else()
  target_compile_options(scoring PRIVATE -Wall -Wextra)
  if(SCORING_WARNINGS_AS_ERRORS)
    target_compile_options(scoring PRIVATE -Werror)
  endif()
endif()

set_target_properties(scoring PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------- 示例 / 测试
if(SCORING_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(SCORING_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 汇总
message(STATUS "[scoring] ${PROJECT_NAME} ${PROJECT_VERSION} · C++${CMAKE_CXX_STANDARD}")
message(STATUS "[scoring]   构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "[scoring]   examples/tests : "
               "${SCORING_BUILD_EXAMPLES}/${SCORING_BUILD_TESTS}")
message(STATUS "[scoring]   规则包目录 : policies/mapapp（scoringMetrics.json + planTemplates.json）")
