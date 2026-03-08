/**
 * @project     Epistemic Filter AI - SVG TUI Framework
 * @file        svg_tui.cpp
 * @version     v1.2.0 "Gallery Edition"
 * @brief       Ultra-high-performance SVG explorer for modern terminals.
 * * ============================================================================
 * SYSTEM ARCHITECTURE
 * ============================================================================
 * * [1] CONCURRENCY ENGINE (Producer-Consumer)
 * ----------------------------------------------------------------------------
 * - MAIN THREAD:   Event-driven I/O loop. Manages ANSI escape sequences, 
 * terminal polling (poll.h), and UI layout calculations.
 * - WORKER POOL:   A fixed-size deque-based thread pool (C++11 threads). 
 * Decouples expensive Cairo/Rsvg rasterization from the UI.
 * * [2] RENDERING STRATEGIES (Dual-Pipeline)
 * ----------------------------------------------------------------------------
 * - SINGLE VIEW:   Quadrant-based fragmentation. DIVS an SVG into 4 tiles
 * to allow for partial updates and high-fidelity rendering
 * beyond standard terminal buffer limits.
 * - GALLERY VIEW:  Mass-thumbnail grid. Utilizes "Half-Block" Unicode encoding
 * (U+2580) to achieve 2:1 pixel density within text cells,
 * mapped to 24-bit TrueColor (RGB).
 * * [3] PERFORMANCE & STABILITY
 * ----------------------------------------------------------------------------
 * - ANSI LRU CACHE: Thread-safe storage of pre-encoded terminal strings.
 * Evicts least recently used frames to cap RAM overhead.
 * - DEBOUNCING:     Hysteresis logic (DEBOUNCE_MS) suppresses rendering
 * storms during high-frequency input (scrolling).
 * - SIGNAL SAFETY:  Atomic flag orchestration for SIGWINCH (resizing) 
 * and SIGINT (interruption) ensures zero-flicker stability.
 * * [4] TECHNICAL STACK
 * ----------------------------------------------------------------------------
 * - CORE:          C++17, Pthreads, POSIX Termios.
 * - GRAPHICS:      Librsvg-2.0 (Vector parse), Cairo (Raster backend).
 * - CONVENTIONS:   PascalCase [Types], snake_case [Logic], SCREAMING [Config].
 * * ============================================================================
 * Copyright (c) 2024 Epistemic Filter AI Sintax Clean up. Released under MIT License.
 * ============================================================================
 */

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <iomanip>
#include <signal.h>
#include <poll.h>
#include <atomic>
#include <unordered_map>
#include <list>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <sstream>
#include <chrono>

#include <librsvg/rsvg.h>
#include <cairo/cairo.h>

namespace fs = std::filesystem;
using namespace std::chrono;

// --- GLOBAL CONSTANTS ---
const int DEBOUNCE_MS = 150;
const int CACHE_SIZE = 500; // Increased for thumbnails
const int POLL_TIMEOUT_MS = 30;
const int THUMB_W = 20;     // Width of gallery thumbnails
const int THUMB_H = 10;     // Height of gallery thumbnails (half-blocks)

std::atomic<bool> G_QUIT_REQUESTED{false};
std::atomic<bool> G_RESIZE_PENDING{false};

void handle_signal(int sig) {
    if (sig == SIGWINCH) G_RESIZE_PENDING = true;
    else G_QUIT_REQUESTED = true;
}

/**
 * @class RenderCache
 * @brief Thread-safe LRU cache for ANSI strings.
 */
class RenderCache {
    struct CacheEntry {
        std::string ansi_data;
        std::list<std::string>::iterator lru_iterator;
    };
    std::unordered_map<std::string, CacheEntry> cache_map;
    std::list<std::string> lru_list;
    size_t max_entries;
    std::mutex cache_mutex;

public:
    explicit RenderCache(size_t size) : max_entries(size) {}

    bool try_get(const std::string& key, std::string& out_data) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache_map.find(key);
        if (it == cache_map.end()) return false;
        lru_list.erase(it->second.lru_iterator);
        lru_list.push_front(key);
        it->second.lru_iterator = lru_list.begin();
        out_data = it->second.ansi_data;
        return true;
    }

    void put(const std::string& key, const std::string& data) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache_map.size() >= max_entries && !lru_list.empty()) {
            cache_map.erase(lru_list.back());
            lru_list.pop_back();
        }
        lru_list.push_front(key);
        cache_map[key] = {data, lru_list.begin()};
    }

    void clear() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache_map.clear();
        lru_list.clear();
    }
};

/**
 * @class SvgBrowserApp
 * @brief Main TUI Controller. Handles Single View and Gallery Mode.
 */
class SvgBrowserApp {
    enum class ViewMode { SINGLE, GALLERY };

private:
    struct termios orig_termios;
    int term_cols = 80;
    int term_rows = 24;
    const std::string dir_path;
    
    std::vector<fs::path> file_list;
    int current_index = 0;
    int last_submitted_index = -1;
    steady_clock::time_point last_input_time;
    ViewMode current_mode = ViewMode::SINGLE;

    RenderCache frame_cache{CACHE_SIZE};
    std::vector<std::thread> worker_threads;
    struct RenderTask { int file_idx; int quadrant; int width; int height; bool is_thumb; };
    std::deque<RenderTask> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    void initialize_terminal() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        std::cout << "\033[?1049h\033[?25l\033[2J" << std::flush;
    }

    void restore_terminal() {
        std::cout << "\033[?25h\033[?1049l" << std::flush;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }

    std::string rasterize_to_ansi(cairo_surface_t* surface) {
        std::stringstream ss;
        unsigned char* data = cairo_image_surface_get_data(surface);
        int stride = cairo_image_surface_get_stride(surface);
        int width = cairo_image_surface_get_width(surface);
        int height = cairo_image_surface_get_height(surface);

        for (int y = 0; y < height - 1; y += 2) {
            for (int x = 0; x < width; ++x) {
                unsigned char* p1 = data + (y * stride) + (x * 4);
                unsigned char* p2 = data + ((y + 1) * stride) + (x * 4);
                ss << "\033[38;2;" << (int)p1[2] << ";" << (int)p1[1] << ";" << (int)p1[0] << "m"
                   << "\033[48;2;" << (int)p2[2] << ";" << (int)p2[1] << ";" << (int)p2[0] << "m▀";
            }
            ss << "\033[0m\n";
        }
        return ss.str();
    }

    void render_image(int idx, int w, int h, int quadrant, bool is_thumb) {
        std::string path = file_list[idx].string();
        std::string key = is_thumb ? "thumb_" + path : path + "_q" + std::to_string(quadrant) + "_" + std::to_string(w) + "x" + std::to_string(h);
        
        std::string dummy;
        if (frame_cache.try_get(key, dummy)) return;

        GError* error = nullptr;
        RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &error);
        if (!handle) return;

        int px_w = w, px_h = h * 2;
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
        cairo_t* cr = cairo_create(surface);

        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_paint(cr);

        if (is_thumb) {
            RsvgRectangle viewport = {0, 0, (double)px_w, (double)px_h};
            rsvg_handle_render_document(handle, cr, &viewport, &error);
        } else {
            RsvgRectangle viewport = {0, 0, (double)px_w * 2, (double)px_h * 2};
            double tx = (quadrant == 2 || quadrant == 4) ? -(double)px_w : 0;
            double ty = (quadrant == 3 || quadrant == 4) ? -(double)px_h : 0;
            cairo_translate(cr, tx, ty);
            rsvg_handle_render_document(handle, cr, &viewport, &error);
        }

        frame_cache.put(key, rasterize_to_ansi(surface));
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        g_object_unref(handle);
    }

    void worker_loop() {
        while (!G_QUIT_REQUESTED) {
            RenderTask task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this] { return !task_queue.empty() || G_QUIT_REQUESTED; });
                if (G_QUIT_REQUESTED) break;
                task = task_queue.front();
                task_queue.pop_front();
            }
            render_image(task.file_idx, task.width, task.height, task.quadrant, task.is_thumb);
        }
    }

    void update_dimensions() {
        struct winsize ws;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        term_cols = ws.ws_col; term_rows = ws.ws_row;
        frame_cache.clear();
        std::cout << "\033[2J" << std::flush;
        G_RESIZE_PENDING = false;
    }

    void draw_single_view() {
        const int list_w = std::min(35, (int)(term_cols * 0.3));
        const int preview_w = term_cols - list_w - 6;
        const int preview_h = term_rows - 5;

        // Draw List
        for (int i = 0; i < term_rows - 3; i++) {
            std::cout << "\033[" << i + 2 << ";1H\033[K";
            if (i < (int)file_list.size()) {
                std::string name = file_list[i].filename().string();
                if (name.length() > (size_t)list_w - 2) name = name.substr(0, list_w - 4) + "..";
                if (i == current_index) std::cout << "\033[7;33m " << std::left << std::setw(list_w-1) << name << "\033[0m";
                else std::cout << " " << std::left << std::setw(list_w-1) << name;
            }
            std::cout << "\033[" << i + 2 << ";" << list_w + 2 << "H\033[1;30m│\033[0m";
        }

        // Preview Logic
        if (!file_list.empty()) {
            int qw = preview_w / 2, qh = preview_h / 2;
            std::string path = file_list[current_index].string();
            std::string q_data[4];
            bool ready = true;
            for (int q=1; q<=4; q++) if (!frame_cache.try_get(path + "_q" + std::to_string(q) + "_" + std::to_string(qw) + "x" + std::to_string(qh), q_data[q-1])) ready = false;

            if (ready) {
                for (int q=0; q<4; q++) {
                    int ox = (q % 2 == 1) ? qw : 0, oy = (q >= 2) ? qh : 0;
                    std::stringstream ss(q_data[q]); std::string line; int yi = 0;
                    while (std::getline(ss, line)) std::cout << "\033[" << (2+oy+yi++) << ";" << (list_w+4+ox) << "H" << line;
                }
            } else if (duration_cast<milliseconds>(steady_clock::now() - last_input_time).count() > DEBOUNCE_MS) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (last_submitted_index != current_index) {
                    task_queue.clear();
                    for(int q=1; q<=4; q++) task_queue.push_back({current_index, q, qw, qh, false});
                    last_submitted_index = current_index;
                    queue_cv.notify_all();
                }
            }
        }
    }

    void draw_gallery_view() {
        const int spacing_x = 4, spacing_y = 2;
        const int cell_w = THUMB_W + spacing_x;
        const int cell_h = THUMB_H + spacing_y;
        int cols = std::max(1, (term_cols - 4) / cell_w);
        
        std::cout << "\033[2;1H";
        for (int i = 0; i < (int)file_list.size(); i++) {
            int r = i / cols, c = i % cols;
            int x = 2 + (c * cell_w), y = 3 + (r * cell_h);
            if (y + cell_h > term_rows) break;

            std::string thumb_data;
            if (frame_cache.try_get("thumb_" + file_list[i].string(), thumb_data)) {
                std::stringstream ss(thumb_data); std::string line; int yi = 0;
                while (std::getline(ss, line)) std::cout << "\033[" << (y + yi++) << ";" << x << "H" << line;
            } else {
                std::cout << "\033[" << y << ";" << x << "H\033[1;30m[PENDING]\033[0m";
                std::lock_guard<std::mutex> lock(queue_mutex);
                task_queue.push_back({i, 0, THUMB_W, THUMB_H, true});
                queue_cv.notify_all();
            }

            // Highlight Cursor
            if (i == current_index) {
                std::cout << "\033[" << y-1 << ";" << x << "H\033[1;33m▶ SELECTING\033[0m";
                std::cout << "\033[" << y+THUMB_H << ";" << x << "H\033[1;33m" << file_list[i].filename().string().substr(0, THUMB_W) << "\033[0m";
            }
        }
    }

public:
    explicit SvgBrowserApp(const char* path) : dir_path(path ? path : ".") {
        signal(SIGWINCH, handle_signal);
        signal(SIGINT, handle_signal);
        initialize_terminal();
        scan_directory();
        update_dimensions();
        for (int i = 0; i < 4; i++) worker_threads.emplace_back(&SvgBrowserApp::worker_loop, this);
    }

    ~SvgBrowserApp() {
        G_QUIT_REQUESTED = true;
        queue_cv.notify_all();
        for (auto &t : worker_threads) if (t.joinable()) t.join();
        restore_terminal();
    }

    void scan_directory() {
        file_list.clear();
        if (fs::exists(dir_path)) {
            for (const auto &entry : fs::directory_iterator(dir_path)) 
                if (entry.path().extension() == ".svg") file_list.push_back(entry.path());
            std::sort(file_list.begin(), file_list.end());
        }
    }

    void start() {
        while (!G_QUIT_REQUESTED) {
            if (G_RESIZE_PENDING) update_dimensions();
            std::cout << "\033[H\033[1;36m SVG EXPLORER \033[0m [" << (current_mode == ViewMode::SINGLE ? "SINGLE" : "GALLERY") << "]\033[K";
            
            if (current_mode == ViewMode::SINGLE) draw_single_view();
            else draw_gallery_view();

            std::cout << "\033[" << term_rows << ";1H\033[48;2;30;30;30m j/k: Nav │ p: Toggle Mode │ q: Quit\033[K\033[0m" << std::flush;

            struct pollfd pfd = { STDIN_FILENO, POLLIN, POLL_TIMEOUT_MS };
            if (poll(&pfd, 1, POLL_TIMEOUT_MS) > 0) {
                char c;
                if (read(STDIN_FILENO, &c, 1) > 0) {
                    last_input_time = steady_clock::now();
                    if (c == 'q') break;
                    if (c == 'p') { current_mode = (current_mode == ViewMode::SINGLE) ? ViewMode::GALLERY : ViewMode::SINGLE; std::cout << "\033[2J"; }
                    if (c == 'j') current_index = (current_index + 1) % file_list.size();
                    if (c == 'k') current_index = (current_index - 1 + (int)file_list.size()) % file_list.size();
                }
            }
        }
    }
};

int main(int argc, char** argv) {
    try { SvgBrowserApp(argc > 1 ? argv[1] : ".").start(); }
    catch (const std::exception& e) { std::cerr << "Error: " << e.what() << std::endl; return 1; }
    return 0;
}
