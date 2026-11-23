/*
  Smothered-mate prover for Stockfish.
  Licensed under the GNU GPL v3 (same as Stockfish).
*/

#include "smother.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <sstream>
#include <vector>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"
#include "position.h"

namespace Stockfish::Smother {

namespace {

int sq_distance(Square a, Square b) {
    return std::max(std::abs(int(file_of(a)) - int(file_of(b))),
                    std::abs(int(rank_of(a)) - int(rank_of(b))));
}

// Assumes the side to move is already known to be checkmated.
bool smothered_given_mate(const Position& pos, Style style) {
    const Color    us    = pos.side_to_move();
    const Bitboard ring  = Attacks::attacks_bb(KING, pos.square<KING>(us));
    const Bitboard holes = ring & ~pos.pieces(us);

    if (style == Style::Classic)
        return !holes;
    const Bitboard open = holes & ~pos.checkers();
    return style == Style::Partial ? !more_than_one(open) : !open;
}

// Holes around the king that the attacker's last move may leave: 0 for
// Classic, 1 (the mating piece's square) for AnyPiece, 2 for Partial.
int allowed_holes(Style style) {
    return style == Style::Classic ? 0 : style == Style::AnyPiece ? 1 : 2;
}

// Proof table. Attack and defence nodes share it: their keys differ because
// the side to move differs. proveN = proven with at most proveN attacker
// moves left; failN = disproven with failN attacker moves left.
struct Entry {
    Key    key;
    Move   move;
    int8_t proveN;
    int8_t failN;
};

constexpr std::size_t TableBits = 18;  // 256K entries, 4 MB
constexpr std::size_t TableMask = (std::size_t(1) << TableBits) - 1;

// Goal of a sub-search. Any = plain forced mate, used to check that a
// defender who leaves the smother line gets mated strictly sooner.
enum Kind {
    SMOTHER,
    ANY
};

constexpr Key AnySalt   = 0x9E3779B97F4A7C15ULL;
constexpr Key QuietSalt = 0xC2B2AE3D27D4EB4FULL;

inline Key salted(Key k, Kind kind, int q) {
    return k ^ (kind == ANY ? AnySalt : 0) ^ (QuietSalt * Key(q));
}

// An attacker move on a smother line that brings back a position seen before,
// earlier in the game or on this line, goes round in circles: the defender
// who escaped from it once escapes the same way again, and the third time is
// a draw. Plain-mate (ANY) lines keep the usual draw rule.
inline bool loops(const Position& pos, int ply, Kind kind) {
    return pos.is_draw(ply)
        || (kind == SMOTHER && pos.is_repetition(std::numeric_limits<int>::max()));
}

struct ScoredMove {
    Move m;
    int  score;
};

class Prover {
   public:
    Prover(Position& p, const Limits& l, const std::atomic<bool>& s, std::vector<Entry>& t) :
        pos(p),
        lim(l),
        stop(s),
        table(t) {}

    Result run();

   private:
    bool attack(int n, Kind kind, int q);
    bool defend(int n, Kind kind, int q, const MoveList<ALL>& replies);
    int  min_moves(Kind kind, int limit, int q);
    void extract_pv(int n, std::vector<Move>& pv);
    void poll();

    Entry* probe(Key k) {
        Entry* e = &table[k & TableMask];
        if (e->key != k)
        {
            e->key    = k;
            e->move   = Move::none();
            e->proveN = 0;
            e->failN  = 0;
        }
        return e;
    }

    Position&                pos;
    const Limits&            lim;
    const std::atomic<bool>& stop;
    std::vector<Entry>&      table;
    std::uint64_t            nodes        = 0;
    int                      ply          = 0;
    bool                     aborted      = false;
    bool                     ignoreLimits = false;
    std::vector<Move>        rootExcluded;  // root moves to skip (alternative proofs)

    double robustness(Move m, int n);
};

void Prover::poll() {
    if (nodes & 2047)
        return;
    // While the line of a found proof is rebuilt (ignoreLimits), the node and
    // time budgets no longer apply, but "stop" still does: a deep proof found
    // while pondering can take seconds to rebuild, and the engine waited for
    // it on its own clock (ponder2: 3 games lost on time on the proof's move).
    if (stop.load(std::memory_order_relaxed)
        || (!ignoreLimits
            && ((lim.maxNodes && nodes >= lim.maxNodes) || (lim.deadline && now() >= lim.deadline))))
        aborted = true;
}

// Attacker to move: is there a check after which every defence still ends in
// a smothered mate within n attacker moves (this one included)?
// q = quiet (non-checking) attacker moves still allowed on this line.
bool Prover::attack(int n, Kind kind, int q) {
    ++nodes;
    poll();
    if (aborted)
        return false;

    if (kind == ANY)
        q = 0;
    const Key  k        = salted(pos.key(), kind, q);
    Entry*     e        = probe(k);
    const bool altRoots = ply == 0 && !rootExcluded.empty();
    if (!altRoots && e->proveN && e->proveN <= n)
        return true;
    if (!altRoots && e->failN >= n)
        return false;

    const Color    them  = ~pos.side_to_move();
    const Square   ksq   = pos.square<KING>(them);
    const Bitboard holes = Attacks::attacks_bb(KING, ksq) & ~pos.pieces(them);

    // Last attacker move: the defender cannot fill any square, so the king's
    // current neighbourhood must already be closed, except possibly the one
    // square the mating piece lands on.
    if (kind == SMOTHER && n == 1 && popcount(holes) > allowed_holes(lim.style))
    {
        e->failN = 1;
        return false;
    }

    // Quiet set-up moves are allowed only with budget left and at least one
    // more attacker move to follow.
    const bool allowQuiet = kind == SMOTHER && q > 0 && n >= 2;

    ScoredMove moves[MAX_MOVES];
    int        cnt = 0;
    for (const Move m : MoveList<ALL>(pos))
    {
        if (kind == SMOTHER && n == 1
            && popcount(holes & ~square_bb(m.to_sq())) > allowed_holes(lim.style) - 1)
            continue;
        if (altRoots
            && std::find(rootExcluded.begin(), rootExcluded.end(), m) != rootExcluded.end())
            continue;
        const bool check = pos.gives_check(m);
        if (!check && !allowQuiet)
            continue;

        PieceType pt = m.type_of() == PROMOTION ? m.promotion_type() : type_of(pos.moved_piece(m));
        int       score = pt == KNIGHT ? 600
                        : pt == QUEEN  ? 500
                        : pt == ROOK   ? 300
                        : pt == BISHOP ? 200
                                       : 100;
        score += pos.capture(m) ? 50 : 0;
        score -= 10 * sq_distance(m.to_sq(), ksq);
        if (!check)
            score -= 10000;  // forcing moves first
        if (m == e->move)
            score += 100000;
        moves[cnt++] = {m, score};
    }
    std::sort(moves, moves + cnt,
              [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });

    for (int i = 0; i < cnt; ++i)
    {
        const Move m     = moves[i].m;
        const bool quiet = moves[i].score < -5000 && !pos.gives_check(m);
        pos.do_move(m);
        ++ply;

        bool ok = false;
        {
            MoveList<ALL> replies(pos);
            if (!quiet)
            {
                if (replies.size() == 0)  // in check with no moves: mate
                    ok = kind == ANY || smothered_given_mate(pos, lim.style);
                else
                    ok = n > 1 && !loops(pos, ply, kind) && defend(n - 1, kind, q, replies);
            }
            else if (replies.size() && !loops(pos, ply, kind))  // no replies here = stalemate
            {
                // Threat test: if the defender could pass, would a smother
                // follow? Only moves that pass are searched in full.
                pos.do_null_move();
                const bool threat = attack(n - 1, SMOTHER, 0);
                pos.undo_null_move();
                ok = !aborted && threat && defend(n - 1, SMOTHER, q - 1, replies);
            }
        }

        --ply;
        pos.undo_move(m);

        if (aborted)
            return false;
        if (ok)
        {
            e         = probe(k);  // may have been reused by a transposition
            e->move   = m;
            e->proveN = int8_t(n);
            return true;
        }
    }

    if (altRoots)  // a failure with exclusions says nothing about the position
        return false;
    e        = probe(k);
    e->failN = int8_t(std::max<int>(e->failN, n));
    return false;
}

// Defender to move (in check): does every reply still lose to a smothered
// mate within n attacker moves?
bool Prover::defend(int n, Kind kind, int q, const MoveList<ALL>& replies) {
    ++nodes;

    if (kind == ANY)
        q = 0;
    const Key k = salted(pos.key(), kind, q);
    Entry*    e = probe(k);
    if (e->proveN && e->proveN <= n)
        return true;
    if (e->failN >= n)
        return false;

    // Try the most likely refutations first: the known refutation, captures
    // of the checker, then king moves, then interpositions.
    const Square ksq = pos.square<KING>(pos.side_to_move());
    ScoredMove   moves[MAX_MOVES];
    int          cnt = 0;
    for (const Move r : replies)
    {
        int score = r == e->move ? 100000 : 0;
        score += pos.capture(r) ? 1000 : 0;
        score += r.from_sq() == ksq ? 500 : 0;
        moves[cnt++] = {r, score};
    }
    std::sort(moves, moves + cnt,
              [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });

    auto fail = [&](Move r) {
        Entry* f = probe(k);
        f->move  = r;
        f->failN = int8_t(std::max<int>(f->failN, n));
        return false;
    };

    if (kind == ANY || lim.strict)
    {
        // Every reply must lose to the same kind of mate within n moves.
        for (int i = 0; i < cnt; ++i)
        {
            const Move r = moves[i].m;
            pos.do_move(r);
            ++ply;
            const bool ok = !pos.is_draw(ply) && attack(n, kind, q);
            --ply;
            pos.undo_move(r);

            if (aborted)
                return false;
            if (!ok)
                return fail(r);
        }
    }
    else
    {
        // Default rule. Let S be the longest smother any reply runs into.
        // Every reply must either be smothered, or be mated by any means in
        // fewer than S moves. Then a defender who resists longest is always
        // smothered, and at least one smother line really exists.
        int sLen[MAX_MOVES];
        int S = 0;

        for (int i = 0; i < cnt; ++i)
        {
            const Move r = moves[i].m;
            pos.do_move(r);
            ++ply;

            int  len  = 0;
            bool dead = pos.is_draw(ply);
            if (!dead)
            {
                for (int j = 1; j <= n && !len && !aborted; ++j)
                    if (attack(j, SMOTHER, q))
                        len = j;
                // No smother: it must at least be a quicker plain mate.
                if (!len && !aborted && !(n > 1 && attack(n - 1, ANY, 0)))
                    dead = true;
            }

            --ply;
            pos.undo_move(r);

            if (aborted)
                return false;
            if (dead)
                return fail(r);
            sLen[i] = len;
            S       = std::max(S, len);
        }

        if (!S)
            return fail(moves[0].m);

        for (int i = 0; i < cnt; ++i)
        {
            if (sLen[i])
                continue;
            const Move r = moves[i].m;
            pos.do_move(r);
            ++ply;
            const bool ok = S > 1 && attack(S - 1, ANY, 0);
            --ply;
            pos.undo_move(r);

            if (aborted)
                return false;
            if (!ok)
                return fail(r);
        }
    }

    e         = probe(k);
    e->proveN = int8_t(n);
    return true;
}

// Smallest k <= limit such that attack(k, kind) holds, or 0.
int Prover::min_moves(Kind kind, int limit, int q) {
    for (int k = 1; k <= limit; ++k)
        if (attack(k, kind, q))
            return k;
    return 0;
}

// Follow the proof: the attacker plays the proven move, the defender picks
// the reply that holds out longest.
void Prover::extract_pv(int n, std::vector<Move>& pv) {
    std::vector<Move>     done;
    Kind                  kind = SMOTHER;
    int                   q    = lim.quietMoves;

    while (n > 0 && attack(n, kind, q))
    {
        const Move m = probe(salted(pos.key(), kind, kind == ANY ? 0 : q))->move;
        if (m == Move::none())
            break;
        if (!pos.gives_check(m))
            --q;
        pv.push_back(m);
        done.push_back(m);
        pos.do_move(m);
        ++ply;

        MoveList<ALL> replies(pos);
        if (replies.size() == 0)
            break;

        Move best     = Move::none();
        int  bestN    = 0;
        Kind bestKind = SMOTHER;
        for (const Move r : replies)
        {
            pos.do_move(r);
            ++ply;
            // Prefer showing the smother line; a reply only falls back to a
            // plain (strictly shorter) mate when it has no smother.
            Kind rk  = kind;
            int  len = min_moves(kind, n - 1, q);
            if (!len && kind == SMOTHER && !lim.strict && n > 2)
            {
                len = min_moves(ANY, n - 2, 0);
                rk  = ANY;
            }
            --ply;
            pos.undo_move(r);
            if (len > bestN || (len == bestN && rk == SMOTHER))
            {
                bestN    = len;
                best     = r;
                bestKind = rk;
            }
        }
        if (best == Move::none())
            break;
        pv.push_back(best);
        done.push_back(best);
        pos.do_move(best);
        ++ply;
        n    = bestN;
        kind = bestKind;
    }

    for (auto it = done.rbegin(); it != done.rend(); ++it)
    {
        pos.undo_move(*it);
        --ply;
    }
}

// Share of the defender's replies to our first move m that stay inside the
// smother (instead of escaping into a faster plain mate), weighted by how
// natural the reply is for a practical opponent: captures that do not lose
// material are the typical escapes, so they weigh most.
double Prover::robustness(Move m, int n) {
    const int q = pos.gives_check(m) ? lim.quietMoves : lim.quietMoves - 1;
    pos.do_move(m);
    ++ply;
    double hit = 0, total = 0;
    for (const Move r : MoveList<ALL>(pos))
    {
        const bool   safe = pos.see_ge(r, Value(0));
        const double w    = pos.capture(r) && safe ? 1.0 : safe ? 0.6 : 0.2;
        pos.do_move(r);
        ++ply;
        const bool smothered = n > 1 && min_moves(SMOTHER, n - 1, q) > 0;
        --ply;
        pos.undo_move(r);
        total += w;
        hit += smothered ? w : 0;
    }
    --ply;
    pos.undo_move(m);
    return total > 0 ? hit / total : 1.0;
}

Result Prover::run() {
    Result res;
    if (!lim.keepTable)
        std::fill(table.begin(), table.end(), Entry{0, Move::none(), 0, 0});

    for (int n = 1; n <= lim.maxMoves; ++n)
    {
        if (attack(n, SMOTHER, lim.quietMoves))
        {
            res.found  = true;
            res.mateIn = n;

            // Among first moves that prove a smother in n, play the one the
            // opponent is least likely to escape from. Alternatives are found
            // by excluding the moves already proven; the search stays within
            // the limits, and whatever was found by then is used.
            const Key         rootKey = salted(pos.key(), SMOTHER, lim.quietMoves);
            std::vector<Move> cands{probe(rootKey)->move};
            if (lim.robust && n > 1)
            {
                while (cands.size() < 4 && !aborted)
                {
                    rootExcluded = cands;
                    if (!attack(n, SMOTHER, lim.quietMoves) || aborted)
                        break;
                    cands.push_back(probe(rootKey)->move);
                }
                rootExcluded.clear();
                aborted = false;

                Move   best  = cands[0];
                double bestR = -1;
                for (Move m : cands)
                {
                    const double r = robustness(m, n);
                    if (aborted)
                        break;
                    if (r > bestR)
                        bestR = r, best = m;
                }
                aborted          = false;
                Entry* e         = probe(rootKey);
                e->move          = best;
                e->proveN        = int8_t(n);
                res.alternatives = int(cands.size());
                res.robustness   = bestR;
            }
            // The proof exists. Rebuild its line; if a stop cuts that short,
            // the proven first move from the table is enough to play.
            const Move first = probe(rootKey)->move;
            ignoreLimits     = true;
            extract_pv(n, res.pv);
            if (res.pv.empty() && first != Move::none())
                res.pv.push_back(first);
            break;
        }
        if (aborted)
            break;
    }

    res.aborted = aborted;
    res.nodes   = nodes;
    return res;
}

}  // namespace

int potential(const Position& pos, Color us) {
    const Color    them  = ~us;
    const Square   ksq   = pos.square<KING>(them);
    const Bitboard ring  = Attacks::attacks_bb(KING, ksq);
    const Bitboard own   = pos.pieces(them);
    const int      boxed = popcount(ring & own);
    const int      holes = popcount(ring & ~own);

    // A king with many open squares cannot be smothered soon.
    if (holes > 3)
        return 0;

    int score = 4 * boxed - 3 * holes;

    // Knight distance (in knight moves) to a square that checks the king.
    const Bitboard checkSqs = Attacks::attacks_bb(KNIGHT, ksq) & ~pos.pieces(us);
    Bitboard       knights  = pos.pieces(us, KNIGHT);
    while (knights)
    {
        const Square s   = pop_lsb(knights);
        Bitboard     one = Attacks::attacks_bb(KNIGHT, s);
        if (one & checkSqs)
            score += 6;
        else
        {
            Bitboard two = 0;
            while (one)
                two |= Attacks::attacks_bb(KNIGHT, pop_lsb(one));
            if (two & checkSqs)
                score += 3;
        }
    }

    // A queen near the king supports the typical queen sacrifice.
    Bitboard queens = pos.pieces(us, QUEEN);
    while (queens)
        if (sq_distance(pop_lsb(queens), ksq) <= 3)
            score += 2;

    return std::max(score, 0);
}

int shape(const Position& pos, Color us) {
    const Color    them  = ~us;
    const Square   ksq   = pos.square<KING>(them);
    const Bitboard ring  = Attacks::attacks_bb(KING, ksq);
    const int      boxed = popcount(ring & pos.pieces(them));
    const int      holes = popcount(ring & ~pos.pieces(them));

    const int f = file_of(ksq), r = rank_of(ksq);
    const int edge = std::min(std::min(f, 7 - f), std::min(r, 7 - r));  // 0 = on an edge

    int score = 5 * boxed - 3 * holes + 6 * (3 - edge);

    Bitboard knights = pos.pieces(us, KNIGHT);
    while (knights)
        score += 3 * std::max(0, 5 - sq_distance(pop_lsb(knights), ksq));

    Bitboard queens = pos.pieces(us, QUEEN);
    while (queens)
        score += 2 * std::max(0, 5 - sq_distance(pop_lsb(queens), ksq));

    return std::max(score, 0);
}

// Version 2: weights fitted (logistic regression, notes/fit) to "a proven
// smother follows within 20 plies" over ~150k decided positions from games
// played without construction mode: a stronger corner, no plain-edge bonus,
// the queen weighted more, and open holes (not covered by us) costing more
// than covered ones.
static int build_score_v2(const Position& pos, Color us) {
    const Color    them  = ~us;
    const Square   ksq   = pos.square<KING>(them);
    const Bitboard ring  = Attacks::attacks_bb(KING, ksq);
    const int      boxed = popcount(ring & pos.pieces(them));
    Bitboard       hb    = ring & ~pos.pieces(them);
    const int      holes = popcount(hb);
    int            open  = 0;
    while (hb)
        open += !(pos.attackers_to(pop_lsb(hb)) & pos.pieces(us));

    const int  f = file_of(ksq), r = rank_of(ksq);
    const bool corner = (f == 0 || f == 7) && (r == 0 || r == 7);

    int score = 12 * boxed - 4 * holes - 6 * open + (corner ? 45 : 0);

    Bitboard theirs = pos.pieces(them) & ~pos.pieces(KING);
    score += 4 * popcount(theirs);
    while (theirs)
        score += sq_distance(pop_lsb(theirs), ksq) <= 2 ? 4 : 0;

    // Knights near the king, and knights one move from a checking square.
    const Bitboard checkSqs = Attacks::attacks_bb(KNIGHT, ksq) & ~pos.pieces(us);
    Bitboard       knights  = pos.pieces(us, KNIGHT);
    while (knights)
    {
        const Square s = pop_lsb(knights);
        score += 5 * std::max(0, 4 - sq_distance(s, ksq));
        score += (Attacks::attacks_bb(KNIGHT, s) & checkSqs) ? 5 : 0;
    }
    Bitboard queens = pos.pieces(us, QUEEN);
    while (queens)
        score += 15 * std::max(0, 4 - sq_distance(pop_lsb(queens), ksq));

    return score;
}

BuildWeights buildWeights;

void set_build_weights(const std::string& s) {
    BuildWeights w;
    std::string  t = s;  // commas or spaces between the numbers
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream is(t);
    int*               f[] = {&w.boxed, &w.hole, &w.corner, &w.edge, &w.knight, &w.queen};
    for (int* p : f)
        if (!(is >> *p))
            break;
    buildWeights = w;
}

int build_score(const Position& pos, Color us, int survive, int cap, int version, int keep, int keepNear, int starve, int starveAt, int ownKeep, int escape, int knightSafe) {
    if (version == 2)
        return build_score_v2(pos, us);

    const Color    them  = ~us;
    const Square   ksq   = pos.square<KING>(them);
    const Bitboard ring  = Attacks::attacks_bb(KING, ksq);
    const int      boxed = popcount(ring & pos.pieces(them));
    const int      holes = popcount(ring & ~pos.pieces(them));

    const int  f = file_of(ksq), r = rank_of(ksq);
    const bool corner = (f == 0 || f == 7) && (r == 0 || r == 7);
    const bool edge   = f == 0 || f == 7 || r == 0 || r == 7;

    const BuildWeights& W = buildWeights;
    int score = W.boxed * boxed - W.hole * holes + (corner ? W.corner : edge ? W.edge : 0);

    // Every enemy piece is a possible blocker: keep them, especially near
    // their king.
    Bitboard theirs = pos.pieces(them) & ~pos.pieces(KING);
    score += keep * popcount(theirs);  // SmotherBuildKeep (11)
    while (theirs)
        score += sq_distance(pop_lsb(theirs), ksq) <= 2 ? keepNear : 0;  // SmotherBuildKeepNear (2)

    // Blocker reserve: enemy pieces minus open squares around their king.
    // Escaped smothers had a reserve of 2 at the first proof, delivered ones
    // 4, so penalise each unit below SmotherBuildStarveAt (off by default).
    if (starve > 0)
    {
        const int reserve = popcount(pos.pieces(them) & ~pos.pieces(KING)) - holes;
        if (reserve < starveAt)
            score -= starve * (starveAt - reserve);
    }

    // Our mating material kept (SmotherBuildOwnKeep): points per queen and
    // knight of ours still on the board, so they are not traded off.
    if (ownKeep > 0)
        score += ownKeep * popcount(pos.pieces(us, QUEEN, KNIGHT));

    // Escape squares (SmotherBuildEscape): squares next to the enemy king
    // that hold none of its pieces and that we do not attack are where a
    // checked king steps out of the box; penalise each.
    if (escape > 0)
    {
        Bitboard open = ring & ~pos.pieces(them);
        int      n    = 0;
        while (open)
            n += !(pos.attackers_to(pop_lsb(open)) & pos.pieces(us));
        score -= escape * n;
    }

    // Knight safety (SmotherBuildKnightSafe): our knights near the king that
    // the defender attacks and we do not defend can be taken or chased.
    if (knightSafe > 0)
    {
        Bitboard nk = pos.pieces(us, KNIGHT);
        while (nk)
        {
            const Square   s   = pop_lsb(nk);
            const Bitboard atk = pos.attackers_to(s);
            if (sq_distance(s, ksq) <= 3 && (atk & pos.pieces(them)) && !(atk & pos.pieces(us)))
                score -= knightSafe;
        }
    }

    // The finishers: knights and the queen near the king.
    Bitboard knights = pos.pieces(us, KNIGHT);
    while (knights)
        score += W.knight * std::max(0, 4 - sq_distance(pop_lsb(knights), ksq));
    Bitboard queens = pos.pieces(us, QUEEN);
    while (queens)
        score += W.queen * std::max(0, 4 - sq_distance(pop_lsb(queens), ksq));

    // Blocker survivability, a bit mask (SmotherBuildSurvive): 1 pinned
    // bonus, 2 defended bonus, 4 knight-wins-it penalty, 8 pawn-push penalty.
    // Only cheap, already-known facts: pins, defenders, a static exchange
    // for our knight, an open pawn push. Capped so that positions with many
    // blockers do not dominate.
    if (survive > 0)
    {
        const Bitboard pinned = pos.blockers_for_king(them) & pos.pieces(them);
        int            s      = 0;
        Bitboard       bl     = ring & pos.pieces(them);
        while (bl)
        {
            const Square   b   = pop_lsb(bl);
            const Bitboard atk = pos.attackers_to(b);
            if ((survive & 1) && (pinned & b))
                s += 4;  // cannot leave its square
            if ((survive & 2) && (atk & pos.pieces(them) & ~pos.pieces(KING)))
                s += 2;  // defended: a capture there costs us
            if (survive & 4)
            {
                // Our knight can take it without losing material (SEE).
                Bitboard nk = atk & pos.pieces(us, KNIGHT);
                if (nk && pos.see_ge(Move(lsb(nk), b), 0))
                    s -= 4;
            }
            if ((survive & 8) && type_of(pos.piece_on(b)) == PAWN)
            {
                // A pawn that can step forward reopens the square.
                const Square to = b + pawn_push(them);
                if (is_ok(to) && pos.empty(to))
                    s -= 3;
            }
        }
        score += std::clamp(s, -cap, cap);
    }

    return score;
}

bool smothered_if_mate(const Position& pos, Style style) {
    return smothered_given_mate(pos, style);
}

bool is_smothered_mate(const Position& pos, Style style) {
    return pos.checkers() && MoveList<ALL>(pos).size() == 0 && smothered_given_mate(pos, style);
}

namespace {
std::vector<Entry>& shared_table() {
    static std::vector<Entry> table(std::size_t(1) << TableBits);
    return table;
}
}

TrapResult
traps(Position& pos, const Limits& perReply, TimePoint deadline, const std::atomic<bool>& stop) {
    TrapResult     tr;
    const Color    them = pos.side_to_move();  // the defender replies now
    const Square   ksq  = pos.square<KING>(them);
    const Bitboard ring = Attacks::attacks_bb(KING, ksq);

    Limits lim    = perReply;
    lim.keepTable = true;   // entries stay valid across sibling positions
    lim.robust    = false;  // only "is there a proof" matters here
    lim.deadline  = deadline;

    for (const Move r : MoveList<ALL>(pos))
    {
        // Plausibility of the reply for a shallow, human-like opponent: a
        // reply that loses material by SEE is rarely played.
        const double w = pos.see_ge(r, Value(0)) ? 1.0 : 0.3;
        tr.total += w;
        ++tr.replies;

        if (stop.load(std::memory_order_relaxed) || (deadline && now() >= deadline))
        {
            tr.aborted = true;
            continue;
        }

        pos.do_move(r);

        // Cheap filter: a king with many open neighbours cannot be smothered
        // within a few moves.
        const Square   k2    = r.from_sq() == ksq ? r.to_sq() : ksq;
        const Bitboard ring2 = r.from_sq() == ksq ? Attacks::attacks_bb(KING, k2) : ring;
        const int      holes = popcount(ring2 & ~pos.pieces(them));
        if (holes <= lim.maxMoves + 1 && MoveList<ALL>(pos).size())
        {
            Prover p(pos, lim, stop, shared_table());
            Result res = p.run();
            tr.nodes += res.nodes;
            if (res.found)
            {
                tr.hit += w;
                if (!tr.shortest || res.mateIn < tr.shortest)
                    tr.shortest = res.mateIn;
                tr.hitMoves.push_back(r);
            }
        }
        pos.undo_move(r);
    }
    return tr;
}

Result prove(Position& pos, const Limits& limits, const std::atomic<bool>& stop) {
    auto& table = shared_table();

    if (limits.maxMoves <= 0)
        return {};

    Prover p(pos, limits, stop, table);
    return p.run();
}

}  // namespace Stockfish::Smother
