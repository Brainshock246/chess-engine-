// Aurora - UCI protocol
#pragma once
#include "search.h"
#include <iostream>
#include <string>
#include <vector>

namespace Aurora {

class UCI {
public:
    explicit UCI(Search& s) : search_(s) {}

    void loop(int argc, char** argv);

private:
    void position(std::istringstream& is);
    void go(std::istringstream& is);
    void setoption(std::istringstream& is);
    void stop_ponder();

    Search& search_;
    Position pos_;
    StateInfo* states_ = nullptr;
    std::vector<StateInfo> stateList_;

    bool ponder_ = false;
    int64_t time_[COLOR_NB] = {0, 0};
    int64_t inc_[COLOR_NB] = {0, 0};
    bool searching_ = false;
    int64_t startTime_ = 0;
    bool quit_ = false;
    bool ponderHit_ = false;
    int moveOverhead_ = 10;
    bool infinite_ = false;
    std::string ponderMove_;

    // ponder thread
    void* ponderThread_ = nullptr;
};

} // namespace Aurora
