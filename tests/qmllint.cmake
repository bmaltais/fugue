# The qmllint test (CMakeLists.txt): runs QMLLINT with ARGS over FILES and fails on any
# warning, printing each as file:line:column. It reads qmllint's JSON, because its plain
# output leaves some warnings out (Qt 6.12: CanvasNode.qml's showed in neither).
#   cmake -DQMLLINT=<qmllint> -DARGS=<options> -DFILES=<qml files> -P qmllint.cmake
execute_process(COMMAND ${QMLLINT} ${ARGS} --json - ${FILES}
                OUTPUT_VARIABLE json ERROR_VARIABLE errors RESULT_VARIABLE result)
string(JSON files ERROR_VARIABLE bad GET "${json}" files)
if(bad)
  message(FATAL_ERROR "qmllint gave no report (exit ${result}):\n${errors}${json}")
endif()

set(count 0)
string(JSON file_count LENGTH "${json}" files)
if(file_count GREATER 0)
  math(EXPR last "${file_count} - 1")
  foreach(f RANGE ${last})
    string(JSON name GET "${json}" files ${f} filename)
    string(JSON warning_count LENGTH "${json}" files ${f} warnings)
    if(warning_count EQUAL 0)
      continue()
    endif()
    math(EXPR last_warning "${warning_count} - 1")
    foreach(w RANGE ${last_warning})
      string(JSON type GET "${json}" files ${f} warnings ${w} type)
      string(JSON text GET "${json}" files ${f} warnings ${w} message)
      string(JSON id ERROR_VARIABLE no_id GET "${json}" files ${f} warnings ${w} id)
      string(JSON line ERROR_VARIABLE no_line GET "${json}" files ${f} warnings ${w} line)
      string(JSON column ERROR_VARIABLE no_column GET "${json}" files ${f} warnings ${w} column)
      message("${type}: ${name}:${line}:${column}: ${text} [${id}]")
      if(NOT type STREQUAL "info")
        math(EXPR count "${count} + 1")
      endif()
    endforeach()
  endforeach()
endif()

if(count GREATER 0 OR NOT result EQUAL 0)
  message(FATAL_ERROR "qmllint: ${count} warnings (exit ${result})")
endif()
