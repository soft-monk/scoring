# examples/ · 可跑示例（各自独立可执行 + 独立退出码 0/1）
#
# 示例都需要**外部注入的规则包**（protocol P7 / SCD-CAND-01）：规则包目录由 CMake 以绝对路径注入，
# **不依赖当前工作目录**（SCD-NFR-02），也可用第一个命令行参数覆盖。
#
# 注意：examples/ 属于**引擎产物**，内 MUST NOT 出现方案名与指标演示数值
# （它们只住在 policies/ 与 tests/ 期望值里）—— 示例一律从规则包读。
set(SCORING_POLICY_DIR "${CMAKE_SOURCE_DIR}/policies/mapapp")

set(SCORING_EXAMPLES
  example_minimal
  example_audit
  example_full_flow
  example_decision)

add_executable(example_minimal minimal/main.cc)
add_executable(example_audit audit/main.cc)
add_executable(example_full_flow full_flow/main.cc)
add_executable(example_decision decision/main.cc)

foreach(tgt ${SCORING_EXAMPLES})
  target_link_libraries(${tgt} PRIVATE scoring)
  set_target_properties(${tgt} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_compile_definitions(${tgt} PRIVATE
    SCORING_POLICY_DIR="${SCORING_POLICY_DIR}")
  if(MSVC)
    # /wd4819 /wd4474：源文件 UTF-8，控制台代码页异常时会报"当前代码页(0)"—— 环境噪声
    target_compile_options(${tgt} PRIVATE /utf-8 /W4 /wd4819 /wd4474)
    target_compile_definitions(${tgt} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
  endif()
endforeach()
