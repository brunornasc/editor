cmake_minimum_required(VERSION 3.22)
project(editor VERSION 0.1.0 LANGUAGES C CXX) # Adicionado C pois o tree-sitter e puro C

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# --- 1. QScintilla ---
find_library(QSCINTILLA_LIBRARY
        NAMES qscintilla2_qt6 qscintilla2
        HINTS ${QSCINTILLA_PREFIX}/lib)
find_path(QSCINTILLA_INCLUDE_DIR
        NAMES Qsci/qsciscintilla.h
        HINTS ${QSCINTILLA_PREFIX}/include ${QSCINTILLA_PREFIX}/include/qt6
        /usr/include/x86_64-linux-gnu/qt6)

if(NOT QSCINTILLA_LIBRARY OR NOT QSCINTILLA_INCLUDE_DIR)
    message(FATAL_ERROR "QScintilla nao encontrado. Instale libqscintilla2-qt6-dev.")
endif()

# --- 2. Tree-Sitter Core (Do pacote apt libtree-sitter-dev) ---
find_library(TREESITTER_LIBRARY NAMES tree-sitter)
find_path(TREESITTER_INCLUDE_DIR NAMES tree_sitter/api.h)

if(NOT TREESITTER_LIBRARY OR NOT TREESITTER_INCLUDE_DIR)
    message(FATAL_ERROR "Tree-Sitter nao encontrado. Execute: sudo apt install libtree-sitter-dev")
endif()

# --- 3. Tree-Sitter C++ Grammar (Vendor) ---
set(TREESITTER_CPP_SRC vendor/tree-sitter-cpp/src/parser.c)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter-cpp/src/scanner.c)
    list(APPEND TREESITTER_CPP_SRC vendor/tree-sitter-cpp/src/scanner.c)
endif()

# --- 4. Target Executável ---
add_executable(editor
        src/main.cpp
        src/ui/MainWindow.cpp
        src/ui/MainWindow.hpp
        src/ui/EditorTabWidget.cpp
        src/ui/EditorTabWidget.hpp
        src/ui/ProjectExplorer.cpp
        src/ui/ProjectExplorer.hpp
        src/ui/OutputWindow.cpp
        src/ui/OutputWindow.hpp
        src/ui/CodeEditorWidget.cpp
        src/ui/CodeEditorWidget.hpp
        src/ui/DebugDock.cpp
        src/ui/DebugDock.hpp
        src/ui/ProjectTreeModel.cpp
        src/ui/ProjectTreeModel.hpp
        src/project/Project.cpp
        src/project/Project.hpp
        src/build/MakefileGenerator.cpp
        src/build/MakefileGenerator.hpp
        src/build/ConanManager.cpp
        src/build/ConanManager.hpp
        src/build/BuildManager.cpp
        src/build/BuildManager.hpp
        src/debug/MiParser.cpp
        src/debug/MiParser.hpp
        src/debug/GdbDebugger.cpp
        src/debug/GdbDebugger.hpp
        src/debug/DebugController.cpp
        src/debug/DebugController.hpp
        src/editor/CodeOutlineParser.cpp
        src/editor/CodeOutlineParser.hpp
        src/editor/TreeSitterQueries.hpp
        src/editor/TreeSitterOutlineParser.hpp
        src/editor/TreeSitterOutlineParser.cpp
        src/editor/SymbolTree.hpp
        src/editor/SymbolTree.cpp
        src/core/ThemeManager.cpp
        src/core/ThemeManager.hpp
        src/core/Keywords.cpp
        src/core/Keywords.hpp
        src/core/keywords/cpp.txt
        src/core/keywords/sql.txt
        src/core/keywords/asm.txt
        resources/resources.qrc
        ${TREESITTER_CPP_SRC}
)

# --- 5. Includes ---
target_include_directories(editor PRIVATE
        src
        vendor/tree-sitter-cpp/src
        ${QSCINTILLA_INCLUDE_DIR}
        ${TREESITTER_INCLUDE_DIR}
)

# --- 6. Links ---
target_link_libraries(editor PRIVATE
        Qt6::Widgets
        ${QSCINTILLA_LIBRARY}
        ${TREESITTER_LIBRARY}
)