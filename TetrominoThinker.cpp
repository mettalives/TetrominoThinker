#include <iostream>
#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <numeric>
#include <map>

// --- Platform-specific includes ------------------------------------------------
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// =============================================================================
// Configuration & Constants (centralized for easy tuning)
// =============================================================================
namespace Config {
    constexpr int W = 10;                     // Board width
    constexpr int H = 20;                     // Board height (visible rows)
    constexpr int PIECE_COUNT = 7;            // Number of Tetromino types
    constexpr int LOOKAHEAD_DEPTH = 3;        // How many upcoming pieces the AI considers

    // Heuristic weights – tuned values from well-known strong Tetris AIs
    struct Weights {
        double HEIGHT_SUM        = -0.510066; // Penalize aggregate column heights
        double HOLES             = -0.76066;  // Strongly penalize internal holes
        double BUMPINESS         = -0.35663;  // Penalize uneven surface
        double WELLS             = -0.05;     // Small penalty for deep wells
        double MAX_HEIGHT_SQUARED = -0.01;    // Quadratic penalty on the highest column
        double LINES_CLEARED     =  0.9;      // Reward for clearing lines
    };
    constexpr Weights HEURISTIC_WEIGHTS{};    // Immutable default instance
}

// =============================================================================
// Tetromino definitions (SRS-compliant kick data omitted for brevity)
// =============================================================================
enum class Piece { I = 0, O, T, S, Z, J, L, Count };

const std::array<std::array<std::array<std::pair<int,int>,4>,4>, Config::PIECE_COUNT> PIECES{{
    // I
    {{{{ {0,0},{1,0},{2,0},{3,0} }}, {{ {1,-1},{1,0},{1,1},{1,2} }}, {{ {0,0},{1,0},{2,0},{3,0} }}, {{ {1,-1},{1,0},{1,1},{1,2} }}}},
    // O (no rotation)
    {{{{ {0,0},{1,0},{0,1},{1,1} }}, {{ {0,0},{1,0},{0,1},{1,1} }}, {{ {0,0},{1,0},{0,1},{1,1} }}, {{ {0,0},{1,0},{0,1},{1,1} }}}},
    // T
    {{{{ {1,0},{0,1},{1,1},{2,1} }}, {{ {1,0},{1,1},{2,1},{1,2} }}, {{ {0,1},{1,1},{2,1},{1,2} }}, {{ {1,0},{0,1},{1,1},{1,2} }}}},
    // S
    {{{{ {1,0},{2,0},{0,1},{1,1} }}, {{ {1,0},{1,1},{2,1},{2,2} }}, {{ {1,1},{2,1},{0,2},{1,2} }}, {{ {0,0},{0,1},{1,1},{1,2} }}}},
    // Z
    {{{{ {0,0},{1,0},{1,1},{2,1} }}, {{ {2,0},{1,1},{2,1},{1,2} }}, {{ {0,1},{1,1},{1,2},{2,2} }}, {{ {1,0},{0,1},{1,1},{0,2} }}}},
    // J
    {{{{ {0,0},{0,1},{1,1},{2,1} }}, {{ {1,0},{2,0},{1,1},{1,2} }}, {{ {0,1},{1,1},{2,1},{2,2} }}, {{ {1,0},{1,1},{0,2},{1,2} }}}},
    // L
    {{{{ {2,0},{0,1},{1,1},{2,1} }}, {{ {1,0},{1,1},{1,2},{2,2} }}, {{ {0,1},{1,1},{2,1},{0,2} }}, {{ {0,0},{1,0},{1,1},{1,2} }}}}
}};

struct Move { int rot = -1, col = -1; double score = -1e12; };

// =============================================================================
// BoardState – compact bitwise representation (10-bit rows)
// =============================================================================
class BoardState {
    std::array<int, Config::H> data;               // Each int holds a row's bitmask

public:
    BoardState() { data.fill(0); }
    BoardState(const BoardState& o) : data(o.data) {}

    // Simple hash for transposition table
    size_t hash() const {
        size_t h = 0;
        for (int r : data) h = h * 31 + r;
        return h;
    }

    // Collision test using bitwise operations
    bool collides(int px, int py, int p, int r) const {
        for (auto [dx, dy] : PIECES[p][r]) {
            int x = px + dx;
            int y = py + dy;
            if (x < 0 || x >= Config::W || y >= Config::H) return true;
            if (y >= 0 && (data[y] & (1 << x))) return true;
        }
        return false;
    }

    // Place piece (sets bits)
    void place(int px, int py, int p, int r) {
        for (auto [dx, dy] : PIECES[p][r]) {
            int x = px + dx;
            int y = py + dy;
            if (y >= 0) data[y] |= (1 << x);
        }
    }

    // Line clearing with in-place compaction
    int clear_lines() {
        int lines = 0;
        int dst = Config::H - 1;
        for (int src = Config::H - 1; src >= 0; --src) {
            if (data[src] == (1 << Config::W) - 1) { ++lines; }
            else { if (src != dst) data[dst] = data[src]; --dst; }
        }
        for (int i = dst; i >= 0; --i) data[i] = 0;
        return lines;
    }

    const std::array<int, Config::H>& raw() const { return data; }
};

// =============================================================================
// Heuristic evaluation (polymorphic interface for future extensions)
// =============================================================================
class AbstractHeuristic {
public:
    virtual double evaluate(const BoardState&, int lines_cleared) const = 0;
    virtual ~AbstractHeuristic() = default;
};

class TetrisHeuristic final : public AbstractHeuristic {
    const Config::Weights w;

public:
    explicit TetrisHeuristic(Config::Weights weights = Config::HEURISTIC_WEIGHTS)
        : w(weights) {}

    double evaluate(const BoardState& b, int lines_cleared) const override {
        const auto& rows = b.raw();
        std::array<int, Config::W> col_height{};
        int holes = 0, bumpiness = 0, height_sum = 0, max_h = 0, wells = 0;

        // Compute column heights and count holes
        for (int x = 0; x < Config::W; ++x) {
            bool found = false;
            for (int y = 0; y < Config::H; ++y) {
                if (rows[y] & (1 << x)) {
                    if (!found) { col_height[x] = Config::H - y; found = true; }
                } else if (found) ++holes;
            }
            height_sum += col_height[x];
            max_h = std::max(max_h, col_height[x]);
        }

        // Bumpiness & wells
        for (int x = 0; x < Config::W; ++x) {
            if (x < Config::W-1) bumpiness += std::abs(col_height[x] - col_height[x+1]);

            int left  = (x == 0)        ? Config::H : col_height[x-1];
            int right = (x == Config::W-1) ? Config::H : col_height[x+1];
            if (col_height[x] < left && col_height[x] < right)
                wells += std::min(left, right) - col_height[x];
        }

        return w.HEIGHT_SUM        * height_sum
             + w.HOLES             * holes
             + w.BUMPINESS         * bumpiness
             + w.WELLS             * wells
             + w.MAX_HEIGHT_SQUARED * max_h * max_h
             + w.LINES_CLEARED     * lines_cleared;
    }
};

// =============================================================================
// AI Engine – depth-limited minimax with transposition table
// =============================================================================
class AIEngine {
    const AbstractHeuristic& heuristic;
    mutable std::map<size_t, double> transposition;

    double lookahead(const BoardState& board, const std::vector<int>& queue, int depth) const {
        if (depth >= static_cast<int>(queue.size())) return 0.0;

        size_t h = board.hash();
        if (transposition.count(h)) return transposition.at(h);

        double best = -1e12;
        bool valid_move = false;
        int piece = queue[depth];

        for (int r = 0; r < 4; ++r) {
            for (int c = -3; c < Config::W; ++c) {
                BoardState sim = board;
                int y = 0;
                if (sim.collides(c, y, piece, r)) continue;
                while (!sim.collides(c, y+1, piece, r)) ++y;

                sim.place(c, y, piece, r);
                int lines = sim.clear_lines();
                double score = heuristic.evaluate(sim, lines)
                             + lookahead(sim, queue, depth + 1);

                best = std::max(best, score);
                valid_move = true;
            }
        }

        if (!valid_move) best = -1e12;
        transposition[h] = best;
        return best;
    }

public:
    explicit AIEngine(const AbstractHeuristic& h) : heuristic(h) {}

    Move find_best_move(BoardState board, const std::vector<int>& queue) {
        transposition.clear();
        Move best;
        int current = queue[0];

        for (int r = 0; r < 4; ++r) {
            for (int c = -3; c < Config::W; ++c) {
                BoardState sim = board;
                int y = 0;
                if (sim.collides(c, y, current, r)) continue;
                while (!sim.collides(c, y+1, current, r)) ++y;

                sim.place(c, y, current, r);
                int lines = sim.clear_lines();
                double score = heuristic.evaluate(sim, lines)
                             + lookahead(sim, queue, 1);

                if (score > best.score) best = {r, c, score};
            }
        }
        return best;
    }
};

// =============================================================================
// 7-bag randomizer
// =============================================================================
class PieceGenerator {
    std::vector<int> bag;
    std::mt19937 rng;

    void refill() {
        bag.clear();
        for (int i = 0; i < Config::PIECE_COUNT; ++i) bag.push_back(i);
        std::shuffle(bag.begin(), bag.end(), rng);
    }

public:
    PieceGenerator() : rng(std::random_device{}()) { refill(); }

    int next() {
        if (bag.empty()) refill();
        int p = bag.front();
        bag.erase(bag.begin());
        return p;
    }
};

// =============================================================================
// Rendering helpers
// =============================================================================
#ifdef _WIN32
void setup_console() { _setmode(_fileno(stdout), _O_U16TEXT); }
#else
void setup_console() {}
#endif

std::vector<std::vector<int>> visual(const BoardState& b) {
    std::vector<std::vector<int>> v(Config::H, std::vector<int>(Config::W, 0));
    for (int y = 0; y < Config::H; ++y)
        for (int x = 0; x < Config::W; ++x)
            if (b.raw()[y] & (1 << x)) v[y][x] = 1;
    return v;
}

void draw(const BoardState& b, int score) {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    auto v = visual(b);
    std::wcout << L"╔══════════╗\n";
    for (const auto& row : v) {
        std::wcout << L"║";
        for (int cell : row) std::wcout << (cell ? L'█' : L'·');
        std::wcout << L"║\n";
    }
    std::wcout << L"╚══════════╝\nScore: " << score << L'\n';
}

// =============================================================================
// Main game loop (AI vs AI demo)
// =============================================================================
int main() {
    setup_console();

    BoardState board;
    PieceGenerator gen;
    TetrisHeuristic heuristic;
    AIEngine ai(heuristic);

    int score = 0;
    std::vector<int> queue(Config::LOOKAHEAD_DEPTH);
    for (int& p : queue) p = gen.next();

    while (true) {
        Move m = ai.find_best_move(board, queue);
        if (m.score <= -1e12) break;               // No legal move → game over

        int piece = queue[0];
        int drop_y = 0;
        while (!board.collides(m.col, drop_y + 1, piece, m.rot)) ++drop_y;
        board.place(m.col, drop_y, piece, m.rot);

        int lines = board.clear_lines();
        static constexpr std::array<int,5> bonus{0,100,300,500,800};
        score += bonus[lines];

        // Shift queue and fetch next piece
        std::rotate(queue.begin(), queue.begin() + 1, queue.end());
        queue.back() = gen.next();

        draw(board, score);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    draw(board, score);
    std::wcout << L"\n========== GAME OVER ==========\n";
    std::wcout << L"Final Score: " << score << L'\n';
#ifdef _WIN32
    system("pause");
#endif
    return 0;
}
