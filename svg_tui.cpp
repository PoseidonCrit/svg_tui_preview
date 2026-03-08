/**
 * Version 0.4 Alpha
 * @file svg_tui.cpp
 * Complie: g++ -O3 svg_tui-v3.cpp -o svg_tui-v3 $(pkg-config --cflags --libs librsvg-2.0 cairo) -lpthread
 * RUN ./svg_tui
 * @brief High-performance SVG TUI Browser with debounced quadrant rendering.
 * * ARCHITECTURE:
 * - PascalCase: Classes (e.g., RenderCache)
 * - snake_case: Functions and variables (e.g., draw_interface)
 * - SCREAMING_SNAKE_CASE: Constants and Global Atomics
 * * UI DESIGN:
 * - Enhanced Sidebar: Increased width for better file visibility.
 * - Compact Preview: Scaled down SVG rendering area.
 * - Note: Optimized for st terminal, goal good perofmace on legacy hardware like CPU i5 760. 
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

// External Rendering Libraries
#include <librsvg/rsvg.h>
#include <cairo/cairo.h>

namespace fs = std::filesystem;
using namespace std::chrono;

// --- GLOBAL CONSTANTS & STATE ---
const int DEBOUNCE_MS = 150;
const int CACHE_SIZE = 100;
const int POLL_TIMEOUT_MS = 55;

std::atomic<bool> G_QUIT_REQUESTED{false};
std::atomic<bool> G_RESIZE_PENDING{false};

/**
 * @brief Global signal handler for terminal events.
 */
void handle_signal(int sig) {
    if (sig == SIGWINCH) G_RESIZE_PENDING = true;
    else G_QUIT_REQUESTED = true;
}

/**
 * @class RenderCache
 * @brief Thread-safe Least Recently Used (LRU) cache for ANSI-encoded image data.
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

    /** @brief Retrieves cached ANSI string if exists. Updates LRU position. */
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

    /** @brief Stores a new ANSI string, evicting oldest if max_entries is reached. */
    void put(const std::string& key, const std::string& data) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache_map.size() >= max_entries && !lru_list.empty()) {
            cache_map.erase(lru_list.back());
            lru_list.pop_back();
        }
        lru_list.push_front(key);
        cache_map[key] = {data, lru_list.begin()};
    }

    /** @brief Flushes all entries. Useful on window resize. */
    void clear() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache_map.clear();
        lru_list.clear();
    }
};

/**
 * @class SvgBrowserApp
 * @brief Main application controller for the TUI browser.
 */
class SvgBrowserApp {
private:
    // --- Terminal State ---
    struct termios orig_termios;
    int term_cols = 80;
    int term_rows = 24;
    const std::string dir_path;
    
    // --- Application State ---
    std::vector<fs::path> file_list;
    int current_index = 0;
    int last_submitted_index = -1;
    steady_clock::time_point last_input_time;

    // --- Threading & Concurrency ---
    RenderCache frame_cache{CACHE_SIZE};
    std::vector<std::thread> worker_threads;
    struct RenderTask { int file_idx; int quadrant; int width; int height; };
    std::deque<RenderTask> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    /** @brief Configures terminal for raw input and alternate buffer. */
    void initialize_terminal() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        // \033[?1049h: Alternate Screen | \033[?25l: Hide Cursor | \033[2J: Clear
        std::cout << "\033[?1049h\033[?25l\033[2J" << std::flush;
    }

    /** @brief Restores terminal to original user settings. */
    void restore_terminal() {
        std::cout << "\033[?25h\033[?1049l" << std::flush;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }

    /** @brief Converts Cairo surface to 24-bit ANSI using half-block characters (▀). */
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
                // BGRA (Cairo) to RGB (ANSI)
                ss << "\033[38;2;" << (int)p1[2] << ";" << (int)p1[1] << ";" << (int)p1[0] << "m"
                   << "\033[48;2;" << (int)p2[2] << ";" << (int)p2[1] << ";" << (int)p2[0] << "m▀";
            }
            ss << "\033[0m\n";
        }
        return ss.str();
    }

    /** @brief Renders a quadrant of an SVG to a specific pixel dimension. */
    void render_quadrant(const std::string& path, int w, int h, int quadrant) {
        std::string key = path + "_q" + std::to_string(quadrant) + "_" + std::to_string(w) + "x" + std::to_string(h);
        std::string existing;
        if (frame_cache.try_get(key, existing)) return;

        GError* error = nullptr;
        RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &error);
        if (!handle) return;

        // Pixel dimensions: Height is doubled for half-block resolution
        int px_w = w, px_h = h * 2;
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px_w, px_h);
        cairo_t* cr = cairo_create(surface);

        // Background
        cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
        cairo_paint(cr);

        // Offset viewport to target specific quadrant
        RsvgRectangle viewport = {0, 0, (double)px_w * 2, (double)px_h * 2};
        double tx = (quadrant == 2 || quadrant == 4) ? -(double)px_w : 0;
        double ty = (quadrant == 3 || quadrant == 4) ? -(double)px_h : 0;
        cairo_translate(cr, tx, ty);

        rsvg_handle_render_document(handle, cr, &viewport, &error);
        frame_cache.put(key, rasterize_to_ansi(surface));

        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        g_object_unref(handle);
    }

    /** @brief Background thread task consumer. */
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
            render_quadrant(file_list[task.file_idx].string(), task.width, task.height, task.quadrant);
        }
    }

    /** @brief Polls terminal dimensions and invalidates layout. */
    void update_dimensions() {
        struct winsize ws;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        term_cols = ws.ws_col; 
        term_rows = ws.ws_row;
        frame_cache.clear();
        std::cout << "\033[2J" << std::flush;
        G_RESIZE_PENDING = false;
    }

public:
    explicit SvgBrowserApp(const char* path) : dir_path(path ? path : ".") {
        signal(SIGWINCH, handle_signal);
        signal(SIGINT, handle_signal);
        
        initialize_terminal();
        scan_directory();
        update_dimensions();
        
        last_input_time = steady_clock::now();
        unsigned int core_count = std::thread::hardware_concurrency();
        int threads = core_count > 0 ? core_count : 4;
        for (int i = 0; i < threads; i++) worker_threads.emplace_back(&SvgBrowserApp::worker_loop, this);
    }

    ~SvgBrowserApp() {
        G_QUIT_REQUESTED = true;
        queue_cv.notify_all();
        for (auto &t : worker_threads) if (t.joinable()) t.join();
        restore_terminal();
    }

    /** @brief Loads SVG files from disk. */
    void scan_directory() {
        file_list.clear();
        if (fs::exists(dir_path)) {
            for (const auto &entry : fs::directory_iterator(dir_path)) {
                if (entry.path().extension() == ".svg") file_list.push_back(entry.path());
            }
            std::sort(file_list.begin(), file_list.end());
        }
    }

    /** @brief Draws the TUI layout. Modified to have a larger list and smaller SVG. */
    void draw_interface() {
        if (G_RESIZE_PENDING) update_dimensions();

        // --- LAYOUT LOGIC ---
        // Increase list width to ~35% of terminal, max 40 chars
        const int list_w = std::min(40, (int)(term_cols * 0.35));
        const int preview_w = term_cols - list_w - 6;
        const int preview_h = term_rows - 5; 

        // 1. HEADER
        std::cout << "\033[H\033[1;36m SVG EXPLORER \033[0m [" << current_index + 1 << "/" << file_list.size() << "]\033[K\n";

        // 2. SIDEBAR (LARGER LIST)
        for (int i = 0; i < term_rows - 3; i++) {
            std::cout << "\033[" << i + 2 << ";1H\033[K";
            if (i < (int)file_list.size()) {
                std::string name = file_list[i].filename().string();
                if (name.length() > (size_t)list_w - 3) name = name.substr(0, list_w - 5) + "..";
                
                if (i == current_index) std::cout << "\033[7;33m " << std::left << std::setw(list_w - 1) << name << " \033[0m";
                else std::cout << " " << std::left << std::setw(list_w - 1) << name;
            }
            // Vertical Separator
            std::cout << "\033[" << i + 2 << ";" << list_w + 2 << "H\033[1;30m│\033[0m";
        }

        // 3. MAIN PREVIEW (SMALLER SVG)
        if (!file_list.empty()) {
            int qw = preview_w / 2, qh = preview_h / 2;
            int start_x = list_w + 4, start_y = 2;
            std::string path = file_list[current_index].string();
            
            auto now = steady_clock::now();
            auto idle_ms = duration_cast<milliseconds>(now - last_input_time).count();

            std::string quadrant_data[4];
            bool ready = true;
            for (int q = 1; q <= 4; q++) {
                if (!frame_cache.try_get(path + "_q" + std::to_string(q) + "_" + std::to_string(qw) + "x" + std::to_string(qh), quadrant_data[q-1])) 
                    ready = false;
            }

            if (ready) {
                for (int q = 0; q < 4; q++) {
                    int off_x = (q % 2 == 1) ? qw : 0;
                    int off_y = (q >= 2) ? qh : 0;
                    std::stringstream ss(quadrant_data[q]);
                    std::string line;
                    int y_idx = 0;
                    while (std::getline(ss, line)) {
                        std::cout << "\033[" << (start_y + off_y + y_idx++) << ";" << (start_x + off_x) << "H" << line;
                    }
                }
            } else if (idle_ms > DEBOUNCE_MS) { 
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (last_submitted_index != current_index) {
                    task_queue.clear(); 
                    for (int q = 1; q <= 4; q++) task_queue.push_back({current_index, q, qw, qh});
                    last_submitted_index = current_index;
                    queue_cv.notify_all();
                }
                std::cout << "\033[" << term_rows/2 << ";" << (start_x + preview_w/3) << "H\033[1;34mRendering...\033[0m";
            } else {
                std::cout << "\033[" << term_rows/2 << ";" << (start_x + preview_w/3) << "H\033[1;30mWait...\033[0m";
            }
        }

        // 4. FOOTER
        std::cout << "\033[" << term_rows << ";1H\033[48;2;30;30;30m\033[38;2;180;180;180m "
                  << "↑/↓: j/k │ Quit: q │ File: " << file_list[current_index].filename().string()
                  << "\033[K\033[0m" << std::flush;
    }

    /** @brief Logic loop for handling user interaction. */
    void start() {
        while (!G_QUIT_REQUESTED) {
            draw_interface();
            struct pollfd pfd = { STDIN_FILENO, POLLIN, POLL_TIMEOUT_MS };
            if (poll(&pfd, 1, POLL_TIMEOUT_MS) > 0) {
                char c;
                if (read(STDIN_FILENO, &c, 1) > 0) {
                    last_input_time = steady_clock::now();
                    if (c == 'q') break;
                    if (c == 'j') current_index = (current_index + 1) % file_list.size();
                    if (c == 'k') current_index = (current_index - 1 + (int)file_list.size()) % file_list.size();
                }
            }
        }
    }
};

/**
 * @brief Application entry point.
 */
int main(int argc, char** argv) {
    try {
        SvgBrowserApp app(argc > 1 ? argv[1] : ".");
        app.start();
    } catch (const std::exception& e) {
        std::cerr << "\nRuntime Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
