#!/bin/bash

OUTPUT_DLL="DailyTracker.dll"

echo "Cross‑compiling DailyTracker -> ${OUTPUT_DLL}"

CXX="x86_64-w64-mingw32-g++"

# 1. ADDED -DCURL_STATICLIB so curl knows it's being linked statically
CXXFLAGS="-shared -O2 -std=c++17 -static -DCURL_STATICLIB"
CXXFLAGS="$CXXFLAGS -DUNICODE -D_UNICODE -DWIN32 -D_WIN32"
CXXFLAGS="$CXXFLAGS -mwindows -Wl,--subsystem,windows"

INCLUDES="-I./src"
INCLUDES="$INCLUDES -I./src/nexus"
INCLUDES="$INCLUDES -I./src/core"
INCLUDES="$INCLUDES -I./src/api"
INCLUDES="$INCLUDES -I./src/store"
INCLUDES="$INCLUDES -I./src/imgui"
INCLUDES="$INCLUDES -I/usr/x86_64-w64-mingw32/include"

LIBDIRS="-L/usr/x86_64-w64-mingw32/lib"

# ImGui source files
IMGUI_SOURCES="
    src/imgui/imgui.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_widgets.cpp
    src/imgui/imgui_tables.cpp
"

# Added the missing closing quote down here below $IMGUI_SOURCES
SOURCES="
    src/entry.cpp
    src/core/DataTypes.cpp
    src/core/Cache.cpp
    src/core/Settings.cpp
    src/api/APIManager.cpp
    src/api/ResponseParser.cpp
    src/store/DataStore.cpp
    $IMGUI_SOURCES
"

LIBS="-Wl,-Bstatic -Wl,--start-group -lcurl -lssl -lcrypto -lz -Wl,--end-group -Wl,-Bdynamic -lws2_32 -lwinmm -lwldap32 -lbcrypt -lsecur32 -lcrypt32 -lpathcch -liphlpapi -Wl,-Bstatic -static-libgcc -static-libstdc++"
$CXX $CXXFLAGS $INCLUDES $LIBDIRS -o "$OUTPUT_DLL" $SOURCES $LIBS

if [ $? -eq 0 ]; then
    echo "Success: $OUTPUT_DLL built as fully static self-contained binary."
else
    echo "Build failed."
fi
