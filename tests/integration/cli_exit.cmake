# CLI、SOURCE、ROOT由CTest提供；只删除本二进制目录专属的测试结果。
set(run_directory "${ROOT}/cli-unconverged")
file(REMOVE_RECURSE "${run_directory}")
file(REMOVE_RECURSE "${ROOT}/must-not-create")
execute_process(COMMAND "${CLI}" run --aux "${SOURCE}/tests/fixtures/tiny/tiny.aux"
  --stage all --continue-unconverged --max-iterations 1 --set overflow_stop=0
  --set image_width=320 --output "${run_directory}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 3)
  message(FATAL_ERROR "未收敛显式继续应返回3，实际${status}: ${output} ${errors}")
endif()
if(NOT EXISTS "${run_directory}/stages/standard_legal/placement.json")
  message(FATAL_ERROR "显式继续必须确实完成标准合法化")
endif()
execute_process(COMMAND "${CLI}" check --aux "${SOURCE}/tests/fixtures/tiny/tiny.aux" --snapshot "${run_directory}/stages/standard_legal/placement.json"
  RESULT_VARIABLE checked OUTPUT_VARIABLE checked_output ERROR_VARIABLE checked_errors)
if(NOT checked EQUAL 0)
  message(FATAL_ERROR "独立重载复检应为0，实际${checked}: ${checked_output} ${checked_errors}")
endif()
execute_process(COMMAND "${CLI}" run --aux "${SOURCE}/tests/fixtures/tiny/tiny.aux" --stage macro
  --output "${ROOT}/must-not-create" RESULT_VARIABLE missing OUTPUT_VARIABLE missing_output ERROR_VARIABLE missing_errors)
if(NOT missing EQUAL 2)
  message(FATAL_ERROR "缺少前置快照应返回2，实际${missing}: ${missing_output} ${missing_errors}")
endif()
if(NOT missing_errors MATCHES "global")
  message(FATAL_ERROR "失败必须来自缺少global前置条件，不能由已有目录等其他错误替代: ${missing_errors}")
endif()
message(STATUS "CLI未收敛退出码、显式继续、独立合法检查与前置条件均通过")
