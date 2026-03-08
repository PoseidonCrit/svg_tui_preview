SVG TUI Visualizer (svg_tui_preview)

svg_tui_preview is a high-performance Terminal User Interface (TUI) framework designed for ultra-fast exploration of SVG files. This prototype focuses on providing a snappy, responsive experience even when dealing with complex vector graphics.

🚀 Key Features

Fast Rendering: Leverages librsvg and cairo for high-quality, efficient rasterization directly to the terminal.

Threaded Architecture: Utilizes background worker threads to handle rendering tasks, ensuring the UI remains fluid and input is never blocked.

Quadrant Debouncing: Implements an intelligent "settle" logic that only triggers high-resolution rendering when navigation pauses, significantly saving CPU cycles.

LRU Caching: Stores previously rendered frames in an ANSI-encoded Least Recently Used (LRU) cache for instantaneous back-and-forth navigation.




🛠 Installation

0. Dependencies

Ensure your system has the following libraries and tools installed:
_____________________________________________________________
librsvg-2.0

cairo

pkg-config

g++ (C++17 support required)
_____________________________________________________________
1. Local Project Setup

Create your workspace and organize the source files:
_____________________________________________________________
mkdir svg-tui

cd svg-tui
_____________________________________________________________

Project Structure:

src/: Place the svg_tui.cpp source file here.

include/: (Optional) Reserved for future header file separation.

CMakeLists.txt: Build configuration file for standardized compilation.

README.md: Project documentation.

2. Building the Project

You can compile the project using the raw compiler command or the preferred CMake build system.

Option A: Manual Compilation (Quick Start)
_____________________________________________________________
g++ -O3 src/svg_tui.cpp -o svg_tui $(pkg-config --cflags --libs librsvg-2.0 cairo) -lpthread
_____________________________________________________________

Option B: Using CMake (Recommended)
_____________________________________________________________
cmake .
make
_____________________________________________________________

🌍 Global Access (Recommended)

To run the visualizer from any directory, move the binary to your local bin and set up an alias.


_____________________________________________________________
Step 1: Move to Local Bin

mkdir -p ~/.local/bin

cp svg_tui ~/.local/bin/
_____________________________________________________________

Step 2: Create an Alias

Add the following to your ~/.bashrc or ~/.zshrc:

# SVG TUI Alias
alias svgt='~/.local/bin/svg_tui'


Then refresh your shell: source ~/.bashrc (or ~/.zshrc).

_____________________________________________________________


📖 Usage

Now you can simply type svgt followed by the directory path to browse SVGs:

_____________________________________________________________
# Usage with alias
svgt /path/to/your/svg/folder

# Usage from current directory
svgt .
_____________________________________________________________


_____________________________________________________________
alternative call :

./svg_tui /path/to/your/svg/folder
_____________________________________________________________

Controls:
_____________________________________________________________

Key | Action

j    Move Down / Next File

k    Move Up / Previous File

q    Exit Application

_____________________________________________________________

📝 License

This project is open-source.
