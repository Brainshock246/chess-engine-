#include "uci.h"
#include "weights.h"
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

namespace Aurora {

using namespace std::chrono;

namespace {

constexpr int MAX_MOVES_UCI = 256;
int64_t now_ms() {
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

void UCI::stop_ponder() {
    if (ponderThread_) {
        search_.set_node_limit(1);  // force the ponder search to bail out
        // The ponder thread observes this through check_time()
    }
}

void UCI::position(std::istringstream& is) {
    std::string token, fen;

    // NOTE: the caller has already consumed the leading "position" token, so
    // this must NOT skip another word. Skipping one here made the *placement*
    // arrive where "startpos"/"fen" was expected, the position silently fell
    // through to set(""), and the engine searched a board with no pieces on it -
    // or crashed, depending on which other defect the empty board then hit.
    Move startPos = MOVE_NONE;
    is >> token;
    if (token == "startpos") {
        fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        is >> token;   // next should be "moves" or eof
    } else if (token == "fen") {
        // read the next 6 fields
        std::string f;
        for (int i = 0; i < 6 && (is >> f); ++i) {
            fen += f;
            fen += " ";
        }
        is >> token;
    } else {
        // Neither "startpos" nor "fen". Falling through here used to install an
        // *empty* board, which then crashed in set_check_info: king_square() is
        // lsb(pieces(c, KING)), lsb(0) is 64, and every attacker lookup indexed
        // one past the end of the tables. A malformed command must be reported,
        // not applied.
        std::cerr << "position: expected 'startpos' or 'fen', got '" << token << "'" << std::endl;
        return;
    }

    stateList_.assign(1024, StateInfo{});
    states_ = stateList_.data();
    pos_.set(fen, states_);

    if (token == "moves") {
        std::string ms;
        while (is >> ms) {
            Move m = to_move(pos_, ms);
            if (m == MOVE_NONE || m == MOVE_NULL) break;
            pos_.do_move(m, stateList_[pos_.game_ply()]);
        }
    }
    (void)startPos;
}

void UCI::setoption(std::istringstream& is) {
    std::string token, name, value;
    is >> token;                       // "setoption"
    is >> token;                       // "name"
    std::string n;
    while (is >> n) {
        if (n == "value") break;
        name += (name.empty() ? "" : " ") + n;
    }
    std::string v;
    while (is >> v) value += (value.empty() ? "" : " ") + v;

    if (name == "Hash")        search_.set_hash_size(std::stoul(value));
    else if (name == "Threads") { /* single-threaded by design */ }
    else if (name == "MultiPV") search_.set_multi_pv(std::stoi(value));
    else if (name == "Clear Hash") search_.clear_hash();
    else if (name == "Move Overhead") moveOverhead_ = std::stoi(value);
    else if (name == "EvalWeights") {
        // Debug aid: dump or set evaluation weights.  Not part of UCI.
    }
}

void UCI::go(std::istringstream& is) {
    LimitsType limits;
    limits.startTime = now_ms();

    std::string token;
    Color us = pos_.side_to_move();
    while (is >> token) {
        if (token == "searchmoves") {
            std::string ms;
            while (is >> ms) {
                Move m = to_move(pos_, ms);
                if (m != MOVE_NONE && limits.numSearchmoves < MAX_MOVES_UCI)
                    limits.searchmoves[limits.numSearchmoves++] = m;
                else break;
            }
        }
        else if (token == "wtime")     is >> limits.time[WHITE];
        else if (token == "btime")     is >> limits.time[BLACK];
        else if (token == "winc")      is >> limits.inc[WHITE];
        else if (token == "binc")      is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movesToGo;
        else if (token == "depth")     is >> limits.depth;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "mate")      is >> limits.mate;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "infinite")  limits.infinite = true;
        else if (token == "ponder")    limits.ponder = true;
    }

    // ---- time management -------------------------------------------------
    //
    // Two defects lived here and both were invisible in a single-engine test.
    //
    // 1. `movestogo` was parsed into a throwaway variable. A match runner that
    //    supplies it got nothing, and the division below assumed 30 whatever the
    //    real number was.
    //
    // 2. The allocation was `time / 30 + inc / 2`, hard-coded. On `go wtime 4000`
    //    that is 133 + 25 = 158ms - Aurora thought for 4% of the time it was
    //    given, on every move, for the whole game. The cap below it (45% of the
    //    remaining time) was never the binding constraint, so it did not look
    //    wrong.
    //
    // The third defect was in check_time(), not here: it read limits_.time[0],
    // which is White's slot, so the stop condition belonged to White whoever was
    // on move. `time` is now a single budget passed to the search directly.
    //
    // What follows is a conventional two-bound scheme:
    //   optTime - aim for this; the search may finish early and use less
    //   maxTime - never exceed this, so the side is not flagging at the end
    int64_t optTime = 0, maxTime = 0;
    if (limits.movetime > 0) {
        optTime = maxTime = limits.movetime - moveOverhead_;
    } else if (limits.time[us] > 0) {
        const int64_t base = limits.time[us];
        const int64_t inc  = limits.inc[us];

        // Hard ceiling: leave a margin so the next `go` is not issued late.
        maxTime = std::max<int64_t>(1, std::min(base * 7 / 10, base - moveOverhead_));

        // How many moves we think are left. Prefer what the GUI told us; failing
        // that estimate from the material on the board, which is crude but far
        // better than a constant.
        int moves = limits.movesToGo;
        if (moves <= 0) {
            const int64_t npm = pos_.non_pawn_material(WHITE)
                              + pos_.non_pawn_material(BLACK);
            moves = npm > 4000 ? 32 : npm > 1500 ? 22 : 12;
        }
        moves = std::clamp(moves, 1, 60);

        optTime = base / moves + inc * 3 / 4;
        optTime = std::min(optTime, maxTime);
        optTime = std::max<int64_t>(optTime, 1);
    }

    if (optTime > 0) {
        limits.useTime = true;
        limits.time[us] = optTime;
    }
    if (limits.nodes > 0) search_.set_node_limit(U64(limits.nodes));

    searching_ = true;
    search_.think(pos_, states_, limits, std::cout);
    searching_ = false;
}

void UCI::loop(int argc, char** argv) {
    stateList_.assign(4096, StateInfo{});
    states_ = stateList_.data();

    pos_.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", states_);

    std::string line, token;

    if (argc > 1 && std::string(argv[1]) == "bench") { return; }

    std::cout << "Aurora 1.0 by the Aurora project" << std::endl;
    std::cout << "Features: NOSTOE" << std::endl;

    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        is >> token;
        if (token.empty()) continue;

        if (token == "uci") {
            std::cout << "id name Aurora 1.0" << std::endl;
            std::cout << "id author Aurora project" << std::endl;
            std::cout << "option name Hash type spin default 64 min 1 max 65536" << std::endl;
            std::cout << "option name Threads type spin default 1 min 1 max 512" << std::endl;
            std::cout << "option name MultiPV type spin default 1 min 1 max 256" << std::endl;
            std::cout << "option name Move Overhead type spin default 10 min 0 max 5000" << std::endl;
            std::cout << "option name Clear Hash type button" << std::endl;
            std::cout << "uciok" << std::endl;
        }
        else if (token == "setoption")  setoption(is);
        else if (token == "position")   position(is);
        else if (token == "go")         go(is);
        else if (token == "stop")       stop_ponder();
        else if (token == "ponderhit")  ponderHit_ = true;
        else if (token == "ucinewgame")  search_.new_game();
        else if (token == "isready")    std::cout << "readyok" << std::endl;
        else if (token == "quit")      { quit_ = true; break; }
        else if (token == "eval")      {
            std::cout << "eval: " << int(Eval::evaluate(pos_)) << std::endl;
        }
        else if (token == "d") {
            std::cout << pos_.fen() << std::endl;
        }
        else if (token == "weights") {
            // Announced count first: this stream has no other terminator, so a
            // driver reading it over stdin would otherwise have to guess where
            // it ends.
            int n = Eval::weight_count();
            std::cout << "# " << n << std::endl;
            for (int i = 0; i < n; ++i)
                std::cout << i << "\t" << Eval::weight_name(i) << "\t" << Eval::get_weight(i) << "\n";
        }
        else if (token == "setweight") {
            std::string nm; int v;
            is >> nm >> v;
            int n = Eval::weight_count();
            for (int i = 0; i < n; ++i)
                if (nm == Eval::weight_name(i)) { Eval::set_weight(i, v); break; }
        }
    }
    (void)quit_;
}

} // namespace Aurora
