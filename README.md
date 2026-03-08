SVG TUI Visualizer (svg_tui_preview)

svg_tui_preview is a high-performance Terminal User Interface (TUI) framework designed for ultra-fast exploration of SVG files. This prototype focuses on providing a snappy, responsive experience even when dealing with complex vector graphics.

Key Features

Fast Rendering: Leverages librsvg and cairo for high-quality, efficient rasterization directly to the terminal.

Threaded Architecture: Utilizes background worker threads to handle rendering tasks, ensuring the UI remains fluid and input is never blocked.

Quadrant Debouncing: Implements an intelligent "settle" logic that only triggers high-resolution rendering when navigation pauses, significantly saving CPU cycles.

LRU Caching: Stores previously rendered frames in an ANSI-encoded Least Recently Used (LRU) cache for instantaneous back-and-forth navigation.

Installation

0. Dependencies

Ensure your system has the following libraries and tools installed:

librsvg-2.0

cairo

pkg-config

g++ (C++17 support required)

1. Local Project Setup

Create your workspace and organize the source files:

mkdir svg-tui
cd svg-tui


Project Structure:

src/: Place the svg_tui.cpp source file here.

include/: (Optional) Reserved for future header file separation.

CMakeLists.txt: Build configuration file for standardized compilation.

README.md: Project documentation.

2. Building the Project

You can compile the project using the raw compiler command or the preferred CMake build system.

Option A: Manual Compilation (Quick Start)

g++ -O3 src/svg_tui.cpp -o svg_tui $(pkg-config --cflags --libs librsvg-2.0 cairo) -lpthread


Option B: Using CMake (Recommended)

cmake .
make


Usage

Run the visualizer by passing the path to the directory containing your SVG files:

./svg_tui /path/to/your/svg/folder


Controls

j: Move down / Next file

k: Move up / Previous file

q: Exit application
