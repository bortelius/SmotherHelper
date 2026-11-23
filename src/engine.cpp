/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "engine.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "evaluate.h"
#include "misc.h"
#include "nnue/network.h"
#include "nnue/nnue_common.h"
#include "numa.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "shm.h"
#include "syzygy/tbprobe.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

namespace NN = Eval::NNUE;

int MaxThreads = std::max(1024, 4 * int(get_hardware_concurrency()));

// The default configuration will attempt to group L3 domains up to 32 threads.
// This size was found to be a good balance between the Elo gain of increased
// history sharing and the speed loss from more cross-cache accesses (see
// PR#6526). The user can always explicitly override this behavior.
constexpr NumaAutoPolicy DefaultNumaPolicy = BundledL3Policy{32};

Engine::Engine(std::optional<std::filesystem::path> path) :
    binaryDirectory(path ? CommandLine::get_binary_directory(*path) : std::filesystem::path{}),
    numaContext(NumaConfig::from_system(DefaultNumaPolicy)),
    threads(),
    networkFile{std::nullopt, ""},
    network(numaContext, get_default_network()) {

    pos.set(StartFEN, false);

    options.add(  //
      "Debug Log File", Option("", [](const Option& o) {
          start_logger(path_from_utf8(std::string(o)));
          return std::nullopt;
      }));

    options.add(  //
      "NumaPolicy", Option("auto", [this](const Option& o) {
          if (!set_numa_config_from_option(o))
              return "NumaPolicy: invalid value '" + std::string(o) + "', keeping previous config.";
          return numa_config_information_as_string() + "\n"
               + thread_allocation_information_as_string();
      }));

    options.add(  //
      "Threads", Option(1, 1, MaxThreads, [this](const Option&) {
          resize_threads();
          return thread_allocation_information_as_string();
      }));

    options.add(  //
      "Hash", Option(16, 1, MaxHashMB, [this](const Option& o) {
          set_tt_size(o);
          return std::nullopt;
      }));

    options.add(  //
      "Clear Hash", Option([this](const Option&) {
          search_clear();
          return std::nullopt;
      }));

    options.add(  //
      "Ponder", Option(false));

    options.add(  //
      "MultiPV", Option(1, 1, MAX_MOVES));

    options.add("Skill Level", Option(20, 0, 20));

    options.add("Move Overhead", Option(10, 0, 5000));

    options.add("nodestime", Option(0, 0, 10000));

    options.add("UCI_Chess960", Option(false));

    options.add("UCI_LimitStrength", Option(false));

    options.add("UCI_Elo",
                Option(Stockfish::Search::Skill::LowestElo, Stockfish::Search::Skill::LowestElo,
                       Stockfish::Search::Skill::HighestElo));

    // Human mode: with a strength limit (UCI_LimitStrength / Skill Level), the
    // percent of moves picked at random from the top 8 candidates, including
    // real mistakes (Skill alone never loses more than about a pawn per move).
    options.add("HumanBlunder", Option(0, 0, 100));
    // The same in tenths of a percent, for finer steps (the larger applies).
    options.add("HumanBlunderPermille", Option(0, 0, 1000));
    // Play like a human of this rating (1050-2400, human-like Maia-3 scale);
    // sets the strength limit and blunders itself. 0 = off.
    options.add("HumanElo", Option(0, 0, 2400));

    // Analysis strength (see Engine::go): 0 = full strength, else the Elo
    // whose search depth analysis ("go infinite") is capped at. Not named
    // UCI_*, so GUIs that hide those options (DroidFish) show it.
    options.add("Analysis Elo", Option(0, 0, Stockfish::Search::Skill::HighestElo));

    options.add("UCI_ShowWDL", Option(false));

    options.add(  //
      "SyzygyPath", Option("", [](const Option& o) {
          Tablebases::init(o);
          return std::nullopt;
      }));

    options.add("SyzygyProbeDepth", Option(1, 1, 100));

    options.add("Syzygy50MoveRule", Option(true));

    options.add("SyzygyProbeLimit", Option(7, 0, 7));

    // Smothered-mate prover (see smother.h). It only plays a line when it has
    // proved a smothered mate against every defence; otherwise the normal
    // search decides the move.
    options.add("SmotherMate", Option(true));

    options.add("SmotherClassic", Option(false));

    options.add("SmotherStrict", Option(false));

    options.add("SmotherMaxMoves", Option(8, 1, 20));

    options.add("SmotherQuietMoves", Option(1, 0, 3));

    options.add("SmotherTimePercent", Option(25, 0, 100));

    options.add("SmotherParallel", Option(true));

    // Won mode: once the last search scored at least SmotherWinCp (or a
    // mate), the prover gets SmotherWinTimePercent of the move's time.
    options.add("SmotherWinCp", Option(300, 0, 10000));

    options.add("SmotherWinTimePercent", Option(100, 0, 400));

    // Steering in won mode (see search.cpp).
    options.add("SmotherSteerWonOnly", Option(false));

    options.add("SmotherWinMargin", Option(400, 0, 5000));

    options.add("SmotherWinLines", Option(6, 2, 12));

    options.add("SmotherWinTrapDepth", Option(5, 1, 8));

    options.add("SmotherWinTrapNodes", Option(20000, 1000, 10000000));

    options.add("SmotherWinSteerTime", Option(50, 0, 200));

    // Won-mode eval shaping towards smother-friendly positions (0 = off).
    options.add("SmotherShape", Option(0, 0, 64));

    options.add("SmotherShapeCap", Option(100, 0, 1000));

    options.add("SmotherShapeAlways", Option(false));

    // Seek mode: in won mode, plain mates count below smothered ones, for at
    // most SmotherSeek of our moves per game (0 = off), and only while the
    // 50-move counter is below SmotherSeekRule50 half-moves.
    options.add("SmotherSeek", Option(0, 0, 200));

    options.add("SmotherSeekRule50", Option(60, 0, 99));

    // Brutal mode: seek smothers on every move (not only when winning), with
    // a deeper prover (12 moves, 2 quiet) that always gets the won-mode time,
    // and construction mode once the game is decided. On by default since
    // 1.1: full-strength SPRTs found no cost (+2.0 and +1.4 Elo). The bench
    // runs with it off, so the bench signature stays the upstream one.
    options.add("SmotherBrutal", Option(true));

    // After a smother proof, compare up to 4 proven first moves and play the
    // one whose likely replies stay in the smother line. Off by default: it
    // rarely finds alternatives and can cost several times the proof's nodes.
    options.add("SmotherRobust", Option(false));

    // Prover time in brutal mode, in percent of the move's optimum time
    // (capped at 60% of the maximum). Long smothers need ~1-3M prover nodes.
    options.add("SmotherBrutalTimePercent", Option(300, 0, 1000));

    // Brutal mode gives the deep prover its time only from this advantage on
    // (centipawns), or with a mate on the board.
    options.add("SmotherBigCp", Option(1000, 0, 10000));

    // Brutal mode with a forced mate in hand: the prover may use this share
    // of the remaining clock per move (the result is no longer in doubt).
    options.add("SmotherMatePercent", Option(15, 0, 50));
    // After the defender leaves a proven smother: for this many moves the
    // prover may use SmotherEscapePercent of the clock left (0 = off).
    options.add("SmotherEscapeMoves", Option(0, 0, 20));
    options.add("SmotherEscapePercent", Option(20, 0, 50));

    // Brutal construction mode: once the game is decided (SmotherBuildCp,
    // centipawns, or a forced mate), build a smother instead of mating
    // quickly, for at most SmotherBuildMoves moves per game. Positions at or
    // above SmotherBuildFloor are scored by the enemy king's box, at
    // SmotherBuildWeight centipawns per point; extra material counts 1/8.
    options.add("SmotherBuild", Option(true));
    options.add("SmotherBuildCp", Option(800, 0, 10000));
    options.add("SmotherBuildFloor", Option(600, 0, 10000));
    options.add("SmotherBuildMoves", Option(80, 0, 200));
    options.add("SmotherBuildWeight", Option(8, 0, 100));
    // 1 = hand-set build_score weights, 2 = weights fitted to game data.
    options.add("SmotherBuildVersion", Option(1, 1, 2));
    // Also count "all but one" smothers: one square next to the mated king
    // may be empty, covered by the attacker (see Smother::Style::Partial).
    options.add("SmotherPartial", Option(false));
    // Hybrid: "all but one" smothers also count once this many construction
    // moves have been played in the game (0 = never; 80 = after the default
    // construction budget is used up). SmotherPartial counts them always.
    options.add("SmotherPartialAfter", Option(0, 0, 200));
    // Near ("all but one") proof found: look for a true smother at most this
    // many moves longer and prefer it (bounded by the prover's move limit).
    options.add("SmotherFullExtra", Option(3, 0, 20));
    // Near smothers allowed: percent of the prover's budget spent first on a
    // true smother only (0 = off: near first, then a true one if time is left).
    options.add("SmotherFullFirst", Option(0, 0, 100));
    // One switch for the smother goal in brutal mode, for GUIs (DroidFish):
    // Full   = only true smothers count (the options above decide),
    // Hybrid = (default) true smothers first, "all but one" ones after 40 construction
    //          moves (as SmotherPartialAfter 40; an explicit value there wins),
    // Near   = "all but one" smothers always count (as SmotherPartial).
    options.add("SmotherGoal", Option("Hybrid var Full var Hybrid var Near", "Hybrid"));
    // Blocker survivability in the build score, a bit mask: 1 pinned
    // blocker bonus, 2 defended blocker bonus, 4 penalty if our knight wins
    // it (SEE), 8 penalty if a pawn blocker can step forward. 0 = off.
    options.add("SmotherBuildSurvive", Option(0, 0, 15));
    // Cap on the survivability total, in build-score points (+-).
    options.add("SmotherBuildSurviveCap", Option(12, 0, 48));
    // Construction: build-score points per enemy piece kept on the board
    // (blockers for a smother), and the divisor for extra material above the
    // floor (8 = it counts 1/8). Measured: in games where a proven smother
    // escaped, we captured ~7 enemy units after the game was decided and the
    // opponent ended with 3 pieces, against 4 and 6 in delivered smothers.
    options.add("SmotherBuildKeep", Option(11, 0, 50));
    // Extra points for each enemy piece within two squares of its king.
    options.add("SmotherBuildKeepNear", Option(2, 0, 50));
    options.add("SmotherBuildMaterialDiv", Option(8, 1, 64));
    // Blocker reserve guard: build-score points lost per unit of reserve
    // (enemy pieces minus open king-ring squares) below StarveAt. 0 = off.
    options.add("SmotherBuildStarve", Option(0, 0, 50));
    // While building, use at least 1/N of the clock left per move (0 = off).
    options.add("SmotherBuildTime", Option(0, 0, 200));
    options.add("SmotherBuildStarveAt", Option(4, 0, 15));
    // Search control in construction mode (all 0 = off, the search is then
    // unchanged). Lmr: less late-move reduction (1/1024 plies) for our quiet
    // moves that bring a knight closer to a square checking the enemy king or
    // newly cover a free square next to it. DefLmr: the same for the
    // defender's quiet moves that leave a square next to its own king (they
    // can open a flight square). Nmp: no null move for our side while the
    // enemy king has at most that many squares not held by its own pieces.
    options.add("SmotherBuildLmr", Option(0, 0, 4096));
    options.add("SmotherBuildDefLmr", Option(0, 0, 4096));
    options.add("SmotherBuildNmp", Option(0, 0, 8));
    // Build-score terms (points; 0 = off): OwnKeep per queen and knight of
    // ours on the board; Escape per square next to the enemy king that holds
    // none of its pieces and that we do not attack; KnightSafe per knight of
    // ours near the king that is attacked and not defended.
    options.add("SmotherBuildOwnKeep", Option(0, 0, 50));
    // build_score weights, space separated: boxed hole corner edge knight
    // queen, separated by commas (for tuning; the default is the hand-set
    // score).
    options.add("SmotherBuildWeights", Option("13,8,23,8,2,4"));
    options.add("SmotherBuildEscape", Option(4, 0, 50));
    options.add("SmotherBuildKnightSafe", Option(0, 0, 50));

    // While pondering, let the parallel prover search deep and unbounded on
    // the opponent's time.
    options.add("SmotherPonderDeep", Option(true));

    options.add("SmotherSteer", Option(0, 0, 500));

    options.add("SmotherSteerFloor", Option(150, -1000, 1000));

    options.add("SmotherSteerLines", Option(4, 2, 8));

    options.add("SmotherSteerTime", Option(15, 0, 100));

    options.add("SmotherTrapDepth", Option(3, 1, 6));

    options.add("SmotherMateDelays", Option(8, 0, 50));

    options.add("SmotherMateSlack", Option(6, 0, 40));

    options.add("SmotherNodes", Option(5000000, 0, 2000000000));

    options.add(  //
      "EvalFile", Option(EvalFileDefaultName, [this](const Option& o) {
          load_network(path_from_utf8(std::string(o)));
          return std::nullopt;
      }));

    threads.clear();
    threads.ensure_network_replicated();
    resize_threads();
}

std::variant<u64, PositionSetError>
Engine::perft(const std::string& fen, Depth depth, bool isChess960) {
    verify_network();

    return Benchmark::perft(fen, depth, isChess960);
}

void Engine::go(Search::LimitsType& limits) {
    assert(limits.perft == 0);
    verify_network();

    // "Analysis Elo": GUIs such as DroidFish analyse with "go infinite" at
    // full strength and hide the UCI_* options. A value above 0 caps the
    // analysis depth at the depth the UCI_Elo limiter picks its move at for
    // that Elo (about 2 at 1500, 5 at 2000, 8 at 2500), so the lines shown
    // are what a player of that strength would see. 0 = full strength.
    const int analysisElo = int(options["Analysis Elo"]);
    if (limits.infinite && analysisElo > 0 && !limits.depth)
    {
        const Search::Skill skill(20, std::max(analysisElo, int(Search::Skill::LowestElo)));
        limits.depth = 1 + int(skill.level);
    }

    threads.start_thinking(options, pos, limits);
}
void Engine::stop() { threads.stop = true; }

void Engine::search_clear() {
    wait_for_search_finished();

    tt.clear(threads);
    threads.clear();

    // TODO: does not work with multiple instances
    Tablebases::init(options["SyzygyPath"]);  // Free mapped files
}

void Engine::set_on_update_no_moves(std::function<void(const Engine::InfoShort&)>&& f) {
    updateContext.onUpdateNoMoves = std::move(f);
}

void Engine::set_on_update_full(std::function<void(const Engine::InfoFull&)>&& f) {
    updateContext.onUpdateFull = std::move(f);
}

void Engine::set_on_iter(std::function<void(const Engine::InfoIter&)>&& f) {
    updateContext.onIter = std::move(f);
}

void Engine::set_on_bestmove(std::function<void(std::string_view, std::string_view)>&& f) {
    updateContext.onBestmove = std::move(f);
}

void Engine::set_on_start(std::function<void()>&& f) { updateContext.onStart = std::move(f); }

void Engine::set_on_verify_network(std::function<void(std::string_view)>&& f) {
    onVerifyNetwork = std::move(f);
}

void Engine::wait_for_search_finished() { threads.main_thread()->wait_for_search_finished(); }

std::optional<PositionSetError> Engine::set_position(const std::string&              fen,
                                                     const std::vector<std::string>& moves) {
    auto err = pos.set(fen, options["UCI_Chess960"]);
    if (err.has_value())
        return err;

    for (const auto& move : moves)
    {
        auto m = UCIEngine::to_move(pos, move);

        if (m == Move::none())
            return PositionSetError("Illegal move: " + move);

        pos.do_move(m);
    }

    return std::nullopt;
}

// modifiers

bool Engine::set_numa_config_from_option(const std::string& o) {
    if (o == "auto" || o == "system")
        numaContext.set_numa_config(NumaConfig::from_system(DefaultNumaPolicy));
    else if (o == "hardware")
        // Don't respect affinity set in the system.
        numaContext.set_numa_config(NumaConfig::from_system(DefaultNumaPolicy, false));
    else if (o == "none")
        numaContext.set_numa_config(NumaConfig{});
    else
    {
        auto parsed = NumaConfig::from_string(o);
        if (!parsed.has_value())
            return false;
        numaContext.set_numa_config(std::move(*parsed));
    }

    // Force reallocation of threads in case affinities need to change.
    resize_threads();
    threads.ensure_network_replicated();
    return true;
}

void Engine::resize_threads() {
    threads.wait_for_search_finished();
    threads.set(numaContext.get_numa_config(), {options, threads, tt, sharedHists, network},
                updateContext);

    // Reallocate the hash with the new threadpool size
    set_tt_size(options["Hash"]);
    threads.ensure_network_replicated();
}

void Engine::set_tt_size(usize mb) {
    wait_for_search_finished();
    tt.resize(mb, threads);
}

void Engine::set_ponderhit(bool b) { threads.main_manager()->ponder = b; }

// network related

void Engine::verify_network() const {
    const auto file = path_from_utf8(std::string(options["EvalFile"]));
    network->verify(onVerifyNetwork, networkFile, file);

    auto statuses = network.get_status_and_errors();
    for (usize i = 0; i < statuses.size(); ++i)
    {
        const auto [status, error] = statuses[i];
        std::string message        = "Network replica " + std::to_string(i + 1) + ": ";
        if (status == SystemWideSharedConstantAllocationStatus::NoAllocation)
            message += "No allocation.";
        else if (status == SystemWideSharedConstantAllocationStatus::LocalMemory)
            message += "Local memory.";
        else if (status == SystemWideSharedConstantAllocationStatus::SharedMemory)
            message += "Shared memory.";
        else
            message += "Unknown status.";

        if (error.has_value())
            message += " " + *error;

        onVerifyNetwork(message);
    }
}

std::unique_ptr<Eval::NNUE::Network> Engine::get_default_network() {

    auto network_ = std::make_unique<NN::Network>();

    network_->load(binaryDirectory, std::filesystem::path{}, networkFile);

    return network_;
}

void Engine::load_network(const std::filesystem::path& file) {
    network.modify_and_replicate(
      [this, &file](NN::Network& network_) { network_.load(binaryDirectory, file, networkFile); });
    threads.clear();
    threads.ensure_network_replicated();
}

void Engine::save_network(const std::optional<std::filesystem::path>& file) {
    network.modify_and_replicate(
      [&file, this](NN::Network& network_) { network_.save(networkFile, file); });
}

// utility functions

void Engine::trace_eval() const {
    std::unique_ptr<Position> p = std::make_unique<Position>();
    p->set(pos.fen(), options["UCI_Chess960"]);

    verify_network();

    sync_cout << "\n" << Eval::trace(*p, *network) << sync_endl;
}

const OptionsMap& Engine::get_options() const { return options; }
OptionsMap&       Engine::get_options() { return options; }

std::string Engine::fen() const { return pos.fen(); }

std::optional<PositionSetError> Engine::flip() { return pos.flip(); }

std::string Engine::visualize() const {
    std::stringstream ss;
    ss << pos;
    return ss.str();
}

int Engine::get_hashfull(int maxAge) const { return tt.hashfull(maxAge); }

std::vector<std::pair<usize, usize>> Engine::get_bound_thread_count_by_numa_node() const {
    auto                                 counts = threads.get_bound_thread_count_by_numa_node();
    const NumaConfig&                    cfg    = numaContext.get_numa_config();
    std::vector<std::pair<usize, usize>> ratios;
    NumaIndex                            n = 0;
    for (; n < counts.size(); ++n)
        ratios.emplace_back(counts[n], cfg.num_cpus_in_numa_node(n));
    if (!counts.empty())
        for (; n < cfg.num_numa_nodes(); ++n)
            ratios.emplace_back(0, cfg.num_cpus_in_numa_node(n));
    return ratios;
}

std::string Engine::get_numa_config_as_string() const {
    return numaContext.get_numa_config().to_string();
}

std::string Engine::numa_config_information_as_string() const {
    auto cfgStr = get_numa_config_as_string();
    return "Available processors: " + cfgStr;
}

std::string Engine::thread_binding_information_as_string() const {
    auto              boundThreadsByNode = get_bound_thread_count_by_numa_node();
    std::stringstream ss;
    if (boundThreadsByNode.empty())
        return ss.str();

    bool isFirst = true;

    for (auto&& [current, total] : boundThreadsByNode)
    {
        if (!isFirst)
            ss << ":";
        ss << current << "/" << total;
        isFirst = false;
    }

    return ss.str();
}

std::string Engine::thread_allocation_information_as_string() const {
    std::stringstream ss;

    usize threadsSize = threads.size();
    ss << "Using " << threadsSize << (threadsSize > 1 ? " threads" : " thread");

    auto boundThreadsByNodeStr = thread_binding_information_as_string();
    if (boundThreadsByNodeStr.empty())
        return ss.str();

    ss << " with NUMA node thread binding: ";
    ss << boundThreadsByNodeStr;

    return ss.str();
}
}
