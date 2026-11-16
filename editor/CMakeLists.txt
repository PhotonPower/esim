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
  Theme.cpp
  JsonSpan.cpp
  Schematic.cpp
  Symbols.cpp
  SchematicView.cpp
  PropertyPanel.cpp
  PartPalette.cpp
  ModelLibrary.cpp
  ModelBrowser.cpp
  HistogramWidget.cpp)

# The standard model library, built into the program.
qt_add_resources(esim-editor "library" PREFIX "/" FILES library/standard.lib)

target_link_libraries(esim-editor PRIVATE esimcircuit Qt6::Widgets Qt6::Concurrent)
esim_windows_program(esim-editor)
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

# A folder (Windows) or app bundle (macOS) that runs on its own:
#   cmake --install build --prefix dist        (Visual Studio: --config Release)
# with esim next to the editor, the examples, and the Qt libraries
# (windeployqt / macdeployqt).
if(APPLE)
  set(EDITOR_DIR "esim-editor.app/Contents/MacOS")
  set(EXAMPLES_DIR "esim-editor.app/Contents/Resources/examples")
else()
  set(EDITOR_DIR ".")
  set(EXAMPLES_DIR "examples")
endif()
install(TARGETS esim-editor BUNDLE DESTINATION . RUNTIME DESTINATION ${EDITOR_DIR})
install(TARGETS esim RUNTIME DESTINATION ${EDITOR_DIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/ DESTINATION ${EXAMPLES_DIR}
        FILES_MATCHING PATTERN "*.json" PATTERN "*.csv" PATTERN "*.cir")
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
qt_generate_deploy_app_script(TARGET esim-editor OUTPUT_SCRIPT deploy_editor NO_UNSUPPORTED_PLATFORM_ERROR)
# Qt's deploy script needs an absolute prefix (--prefix dist is relative).
# On Windows the Qt libraries and plugins go next to the editor, not into
# bin\ and plugins\.
install(CODE [[
  get_filename_component(QT_DEPLOY_PREFIX "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}" ABSOLUTE)
]])
if(WIN32)
  install(CODE [[
    set(QT_DEPLOY_BIN_DIR ".")
    set(QT_DEPLOY_PLUGINS_DIR ".")
  ]])
endif()
install(SCRIPT ${deploy_editor})

# Tests of the parts without a window (check of the input, curves).
if(ESIM_BUILD_TESTS)
  add_executable(editor_tests tests/editor_tests.cpp Checker.cpp ResultData.cpp Format.cpp Theme.cpp
    JsonSpan.cpp Schematic.cpp Symbols.cpp ModelLibrary.cpp)
  qt_add_resources(editor_tests "library_tests" PREFIX "/" FILES library/standard.lib)
  target_include_directories(editor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  esim_windows_program(editor_tests)
  target_link_libraries(editor_tests PRIVATE esimcircuit Qt6::Widgets Qt6::Concurrent)
  target_compile_options(editor_tests PRIVATE ${ESIM_WARNINGS})
  target_compile_definitions(editor_tests PRIVATE ESIM_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                 ESIM_BINARY="$<TARGET_FILE:esim>")
  add_dependencies(editor_tests esim)
  add_test(NAME editor COMMAND editor_tests)
  set_tests_properties(editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  # Windows: the Qt DLLs of the build are found through the PATH
  function(esim_qt_path test)
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
      set_tests_properties(${test} PROPERTIES ENVIRONMENT_MODIFICATION
                           "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
  endfunction()
  esim_qt_path(editor)

  # The schematic in the main window, driven by mouse and keyboard events.
  find_package(Qt6 COMPONENTS Test QUIET)
  if(Qt6Test_FOUND)
    get_target_property(EDITOR_SOURCES esim-editor SOURCES)
    list(REMOVE_ITEM EDITOR_SOURCES main.cpp)
    add_executable(editor_gui_tests tests/gui_tests.cpp ${EDITOR_SOURCES})
    qt_add_resources(editor_gui_tests "library_gui" PREFIX "/" FILES library/standard.lib)
    target_include_directories(editor_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    esim_windows_program(editor_gui_tests)
    target_link_libraries(editor_gui_tests PRIVATE esimcircuit Qt6::Widgets Qt6::Concurrent Qt6::Test)
    target_compile_options(editor_gui_tests PRIVATE ${ESIM_WARNINGS})
    target_compile_definitions(editor_gui_tests PRIVATE ESIM_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                       ESIM_BINARY="$<TARGET_FILE:esim>")
    add_test(NAME editor_gui COMMAND editor_gui_tests)
    set_tests_properties(editor_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    esim_qt_path(editor_gui)
  endif()
endif()
