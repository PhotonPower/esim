# The editor: a Qt 6 Widgets application. It checks the input with the
# esimcircuit library and runs the simulator as a separate process (the esim
# program is copied next to the editor's executable).

set(CMAKE_AUTOMOC ON)

qt_add_executable(esim-editor MACOSX_BUNDLE WIN32
  main.cpp
  MainWindow.cpp
  CodeEditor.cpp
  JsonHighlighter.cpp
  Checker.cpp
  SimulationRun.cpp
  ResultsPanel.cpp
  PlotWidget.cpp
  ResultData.cpp
  Format.cpp
  Theme.cpp)

target_link_libraries(esim-editor PRIVATE esimcircuit Qt6::Widgets Qt6::Concurrent)
target_compile_options(esim-editor PRIVATE ${ESIM_WARNINGS})
set_target_properties(esim-editor PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "ESIM Editor"
  MACOSX_BUNDLE_GUI_IDENTIFIER "de.esim.editor"
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

# The simulator travels with the editor.
add_dependencies(esim-editor esim)
add_custom_command(TARGET esim-editor POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:esim> $<TARGET_FILE_DIR:esim-editor>
  COMMENT "Copying esim next to the editor")

# Development builds find the examples of the source tree.
target_compile_definitions(esim-editor PRIVATE ESIM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Tests of the parts without a window (check of the input, curves).
if(ESIM_BUILD_TESTS)
  add_executable(editor_tests tests/editor_tests.cpp Checker.cpp ResultData.cpp Format.cpp Theme.cpp)
  target_include_directories(editor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(editor_tests PRIVATE esimcircuit Qt6::Widgets Qt6::Concurrent)
  target_compile_options(editor_tests PRIVATE ${ESIM_WARNINGS})
  target_compile_definitions(editor_tests PRIVATE ESIM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME editor COMMAND editor_tests)
  set_tests_properties(editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
