#!/bin/bash
# Fetch the GUI's vendored deps into deps/ (gitignored; per-host like ffmpeg and lwIP):
#   - Dear ImGui (pinned tag) -- immediate-mode UI over the SDL2 renderer we already use
#   - nlohmann/json single header -- parsing the 9876 API's replies in the GUI
# Usage: scripts/fetch-gui-deps.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p deps

IMGUI_TAG=v1.91.8
if [ ! -d deps/imgui ]; then
    git clone --branch "$IMGUI_TAG" --depth 1 https://github.com/ocornut/imgui.git deps/imgui
fi

JSON_TAG=v3.11.3
if [ ! -f deps/json/nlohmann/json.hpp ]; then
    mkdir -p deps/json/nlohmann
    curl -fsL -o deps/json/nlohmann/json.hpp \
        "https://github.com/nlohmann/json/releases/download/$JSON_TAG/json.hpp"
fi

echo "gui deps ready: deps/imgui ($IMGUI_TAG), deps/json ($JSON_TAG)"
