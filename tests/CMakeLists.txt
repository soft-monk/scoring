# tests/ · 零依赖自测（一个可执行 + 手写断言，不引任何测试框架）
#
# 覆盖需求专篇 docs/需求/scoring需求专篇.md 的 34 条
# （SCD-CAND 4 / SCD-SCORE 6 / SCD-PICK 5 / SCD-EXP 4 / SCD-OPT 5 / SCD-DECIDE 5 / SCD-NFR 5）
# 与共享契约 protocol.md 的可机检清单、冲突裁决 C1/C2/C15/C16/C17。
#
# **不需要任何外部服务、不需要网络、不依赖真实时间**（时钟一律注入假时钟 —— SCD-NFR-05）。
#
# 运行： selftest            → 跑全部
#        selftest --list     → 只列用例名
#        selftest --json     → 机检输出（acceptance.ps1 读它做需求↔用例对账）
#        selftest <名字片段>  → 只跑名字里含该片段的用例
add_executable(selftest selftest.cc)
target_link_libraries(selftest PRIVATE scoring)

set_target_properties(selftest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  target_compile_options(selftest PRIVATE /utf-8 /W4 /wd4819 /wd4474)
  target_compile_definitions(selftest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(selftest PRIVATE -Wall -Wextra)
endif()

# 规则包目录：自测通过 CMake 注入的绝对路径拿到，**不依赖当前工作目录**（SCD-NFR-02）
target_compile_definitions(selftest PRIVATE
  SCORING_POLICY_DIR="${CMAKE_SOURCE_DIR}/policies/mapapp")

add_test(NAME scoring_selftest COMMAND selftest)
