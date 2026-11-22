/*
  Smothered-mate prover for Stockfish.
  Licensed under the GNU GPL v3 (same as Stockfish).

  Proves, by exhaustive AND/OR search, that the side to move can force a
  smothered mate against every defence. The attacker may play any checking
  move (including sacrifices); the defender may play every legal reply.
  A line only succeeds if it ends in a smothered mate: an ordinary mate is
  treated as a failure, so the result is a guarantee, not a preference.
*/

#ifndef SMOTHER_H_INCLUDED
#define SMOTHER_H_INCLUDED

#include <atomic>
#include <cstdint>
#include <vector>

#include "misc.h"
#include "types.h"

namespace Stockfish {

class Position;

namespace Smother {

// Classic: every square next to the mated king holds one of its own pieces
//          (only a knight, or a pawn promoting to one, can deliver this).
// AnyPiece: every square next to the king holds one of its own pieces,
//           except the square of the checking piece itself.
// Partial:  "all but one": as AnyPiece, but at most one more square may be
//           empty or hold an enemy piece (it is covered by the attacker,
//           since the position is mate). SmotherPartial.
enum class Style {
    Classic,
    AnyPiece,
    Partial
};

// True if the side to move is checkmated and the mate is smothered.
bool is_smothered_mate(const Position& pos, Style style);

// Same, for a position already known to be checkmate (cheap: no movegen).
bool smothered_if_mate(const Position& pos, Style style);

struct Limits {
    int           maxMoves   = 8;  // attacker moves (mate in N)
    Style         style      = Style::AnyPiece;
    bool          strict     = false;    // every defence must be smothered
    int           quietMoves = 1;        // non-checking attacker moves per line
    std::uint64_t maxNodes   = 5000000;  // 0 = unlimited
    TimePoint     deadline   = 0;        // absolute now() value, 0 = none
    bool          keepTable  = false;    // reuse the proof table (no clear)
    bool          robust     = false;    // prefer the line hardest to escape (costly)
};

struct Result {
    bool              found   = false;
    bool              aborted = false;
    int               mateIn  = 0;  // attacker moves
    std::uint64_t     nodes   = 0;
    std::vector<Move> pv;                 // principal (longest-resistance) line
    int               alternatives = 0;   // proven first moves compared
    double            robustness   = -1;  // share of likely replies kept in the line
};

// Heuristic "smother potential" of pos for side us (higher = the enemy king
// is more boxed in by its own pieces and our knights/queen are closer to
// delivering a smother). Used only by the optional steering.
int potential(const Position& pos, Color us);

// Smother-friendliness of pos for side us, used as a won-mode eval bonus:
// the enemy king boxed in by its own pieces, near an edge or corner, with
// our knights and queen close. Cheap (a few bitboard operations).
int shape(const Position& pos, Color us);

// Construction-mode target: how ready the enemy king is for a corner smother
// (its own pieces around it, a corner or edge, its pieces kept alive, our
// queen and knights close). Measured: 77% of delivered smothers were in a
// corner, blocked mostly by the defender's own pawns, rooks and bishops.
int build_score(const Position& pos, Color us, int survive = 0, int cap = 12, int version = 1, int keep = 8, int keepNear = 3, int starve = 0, int starveAt = 4, int ownKeep = 0, int escape = 0, int knightSafe = 0);

// Smother traps: pos has the DEFENDER to move (we just played our candidate).
// For every legal reply, try to prove a forced smother for us with perReply
// limits. hit/total is the plausibility-weighted share of replies that walk
// into a forced smother.
struct TrapResult {
    double            hit      = 0;
    double            total    = 0;
    int               replies  = 0;
    int               shortest = 0;  // shortest proven smother after a hit
    std::uint64_t     nodes    = 0;
    bool              aborted  = false;
    std::vector<Move> hitMoves;
    double            share() const { return total > 0 ? hit / total : 0; }
};
TrapResult
traps(Position& pos, const Limits& perReply, TimePoint deadline, const std::atomic<bool>& stop);

// Search from pos (side to move = attacker). pos is restored on return.
// stop is polled so a GUI "stop" interrupts the proof.
Result prove(Position& pos, const Limits& limits, const std::atomic<bool>& stop);

}  // namespace Smother
}  // namespace Stockfish

#endif  // #ifndef SMOTHER_H_INCLUDED
