#pragma once
/*
 * One axis: the motion primitives, and the bookkeeping that decides whether
 * the position reference can still be trusted. Every method here runs on
 * that axis's worker thread and nowhere else -- see TmcDevice.h.
 */

#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "AxisConfig.h"
#include "TmcDevice.h"

namespace tmc {

struct AxisHealth {
    bool linkUp = false;
    bool simulated = false;
    uint32_t ioin = 0;
    uint32_t gconf = 0;
    uint32_t gstat = 0;
    uint32_t drvStatus = 0;
    uint8_t spiStatus = 0;
    int csActual = 0;
    bool standstill = false;

    // Surveys whose first IOIN read was all-zeros or all-ones and a plain
    // retry read the correct VERSION byte: transient bad reads, counted
    // rather than logged since a working rig sees them in ~0.1% of surveys.
    // A climbing rate is worth chasing (wiring, speedHz).
    uint64_t transientReads = 0;

    std::vector<std::string> problems;

    bool ok() const { return linkUp && problems.empty(); }
};

/*
 * What we believe about where the shaft is, persisted across restarts.
 *
 * XACTUAL lives in the chip and survives between commands, but only while the
 * chip stays powered: a reset silently returns it to 0, which looks exactly
 * like "at home". So the expected XACTUAL is written down and cross-checked.
 */
struct ReferenceState {
    bool homed = false;
    std::string homedAt;
    std::string homeMode;
    std::string lostWhy;
    std::string lostAt;

    int minFullsteps = 0;
    int maxFullsteps = 0;
    double travelDeg = 0.0;

    int positionFullsteps = 0;
    int32_t xactualUsteps = 0;

    // The last stop timed out before the shaft reached standstill, so
    // xactualUsteps is a mid-deceleration reading. The next referenceValid()
    // waits for standstill and adopts the chip's XACTUAL, provided the chip
    // has not reset since.
    bool settling = false;
};

inline int32_t fullstepsToUsteps(double fs) {
    return (int32_t)lround(fs * USTEPS_PER_FULLSTEP);
}
inline double ustepsToFullsteps(int32_t us) {
    return (double)us / USTEPS_PER_FULLSTEP;
}

// A percentage of the commissioned travel, as a position in full steps. Not
// rounded to a whole step: across 31 full steps every 1% is 0.31 of a step,
// and the driver resolves 1/256 of one.
inline double percentToFullsteps(double pct, int minFullsteps, int maxFullsteps) {
    return minFullsteps + (double)(maxFullsteps - minFullsteps) * (pct / 100.0);
}

// What became of a bounded move. Positioning mode is open loop, so none of
// these says anything about the shaft: XACTUAL reaches XTARGET whether or not
// the motor followed.
enum class MoveOutcome {
    Reached,     // position_reached before the timeout
    FellShort,   // the ramp generator never reported position_reached (timeout)
    Interrupted, // abort requested, or the channel value moved on (preempt)
    Fault,       // a DRV_STATUS fault bit; motion was stopped
    OpenLoad,    // both coils open for the whole (slow) move: no motor
    NotStarted,  // device not open, already moving, or a register write failed
};

class TmcAxis {
public:
    TmcAxis(const AxisConfig& cfg, const std::string& dataDir);
    ~TmcAxis();

    void updateTunables(const AxisConfig& cfg);

    bool open(std::string& err);
    void close();

    // Where a stepper's position reference lives: one file per stepper NAME
    // in the plugin's data directory (TmcManager carries it across a rename).
    static std::string statePathFor(const std::string& dataDir, const std::string& name);

    // --- health --------------------------------------------------------
    // Never fatal: the status page needs an answer precisely when something
    // is wrong.
    AxisHealth survey();
    // Same picture, but any problem is an error. Call before anything moves.
    bool gates(std::string& err);

    // --- motion --------------------------------------------------------
    bool configureCurrents(int irun, int ihold, std::string& err);
    bool zeroHere(std::string& err);
    void stopMotion();
    // Blocks until VACTUAL reads 0, so the caller's next XACTUAL read
    // reflects where the shaft actually settled, not a mid-deceleration
    // snapshot. The limit is the deceleration time from the speed the chip
    // reports, on the last programmed DMAX. False if it ran out. Call after
    // stopMotion().
    // False at once while fppd is shutting down: nothing waits on the motor then.
    bool waitUntilStopped();
    // Sleeps ms in short slices; false as soon as an abort is requested, so a
    // Stop or fppd's shutdown never waits out a settle time.
    bool pause(int ms);
    // stopMotion(), waitUntilStopped(), then the settled XACTUAL: how every
    // exit from runTo()'s poll loop records where the shaft ended up. A
    // timed-out wait marks the reference as settling. During shutdown it
    // does not wait at all: a shaft still moving is about to have its coils
    // released mid-ramp, so its reference is marked lost instead.
    int32_t stopAndReadEnd();
    void releaseCoils();
    // Whether the last thing this axis did to the chopper was energise it
    // (configureCurrents) rather than release it. What the hold countdown in
    // AxisWorker consults, so a released axis is not "released" again.
    bool coilsEnergised() const { return m_coilsEnergised; }

    // Bounded position move in microsteps. Reports the final XACTUAL even on
    // failure, because where it actually stopped is the interesting part.
    //
    // dynamicAccel scales acceleration with rps (see programRamp in the
    // .cpp) so a fast preset actually moves fast on a short-travel axis.
    // rehome()'s own moves pass false to take RehomeConfig::amax instead:
    // the fixed, gentle acceleration homing against a hard stop needs.
    //
    // rampTimeSec <= 0 means "this stepper's MotionConfig::rampTimeSec";
    // a positive value is a preset's own override. Ignored when dynamicAccel
    // is false, since homing is not specified as a time at all.
    // err is set for every outcome except Reached.
    MoveOutcome runTo(int32_t xTarget, double rps, const char* label, int32_t& xEnd,
                      std::string& err, bool dynamicAccel = true,
                      double rampTimeSec = 0.0);

    // Absolute move in full steps from home. Always enforces the soft limits
    // and may recover a lost reference by rehoming first (attemptAutoRecover
    // in the .cpp). The commissioning jog is the one move that ignores
    // limits, and it has its own entry point.
    bool moveToFullsteps(double fullsteps, double rps, int irun, int ihold,
                         std::string& err, double rampTimeSec = 0.0);

    // Absolute-position conversions between raw motor microsteps and the
    // home-relative fullsteps frame (0 at home, increasing away from it).
    // Which raw direction counts as "away from home" depends on which stop
    // rehome/setHomeHere approached -- see dirSign(). Relative/motor-frame
    // deltas (jog's commanded distance, backoff) do NOT go through these;
    // only values compared against minFullsteps/maxFullsteps do.
    double toPositionFullsteps(int32_t usteps) const;
    int32_t toUsteps(double fullsteps) const;

    // The commissioning jog: a relative move of deltaFullsteps in the raw
    // motor frame, clamped to the soft limits once a real span exists. The
    // reference follows the shaft wherever it ends up, even on failure.
    struct JogResult {
        MoveOutcome outcome = MoveOutcome::NotStarted;
        int32_t beforeUsteps = 0;
        int32_t afterUsteps = 0;
        bool clampedToLimit = false;
    };
    JogResult jog(double deltaFullsteps, double rps, int irun, int ihold,
                  std::string& err);

    // Drive gently into the mechanical stop and call that position 0. The
    // commanded distance is travelFs full steps plus overtravelDeg: the shaft
    // slips against the stop for the overtravel, and wherever it ends -- the
    // stop -- becomes 0. The chip cannot tell whether a stop was met (see the
    // .cpp), so the commissioned range has to cover the real travel.
    // definesHome false is rehome()'s coarse pass: it zeroes the counter and
    // notes the approach direction, but the reference stays lost.
    bool seek(bool ccw, int travelFs, double overtravelDeg, double rps,
              int irun, int ihold, bool definesHome, std::string& err);

    // The three-stage anti-bounce rehome.
    bool rehome(std::string& err,
                const std::function<void(const std::string&)>& progress = nullptr);

    // Declare wherever the shaft is right now to be position 0. No motion.
    //
    // approachDir is "", "ccw" or "cw": the raw direction the shaft was
    // driven to reach this spot (AxisWorker works it out from the jogs).
    // Folded into homeMode as seek()/rehome() do, so dirSign() agrees with a
    // later Rehome. Empty leaves homeMode undecorated (dirSign +1).
    bool setHomeHere(int minFullsteps, int maxFullsteps,
                     const std::string& approachDir, std::string& err);
    // Record the current position as the far soft limit.
    //
    // A far limit on the other side of a home at a stop (min 0) means the
    // home was recorded with the wrong approach direction: the travel runs
    // the other way. That is refused with wrongSide set, unless flip is
    // true, in which case the recorded direction is reversed first (so the
    // position becomes positive and a later Rehome drives into the stop
    // home was really set at) and flipped is set.
    bool setMaxHere(bool flip, bool& wrongSide, bool& flipped, std::string& err);
    // Whether the recorded home was approached ccw (dirSign() < 0).
    bool homeApproachedCcw() const { return dirSign() < 0; }
    // Forget the far soft limit (max back to 0) without moving position 0.
    // Probe does this: jog is unclamped again, which is how a mechanism whose
    // commissioned range no longer matches reality gets driven back to its
    // real stop.
    bool clearMaxHere(std::string& err);

    // --- reference -----------------------------------------------------
    const ReferenceState& reference() const { return m_ref; }

    // Follow the shaft after a commanded move made outside moveToFullsteps()
    // (the commissioning jog). The reference has to track every move we make,
    // including ones that ignore the soft limits -- otherwise the next
    // cross-check sees XACTUAL disagreeing with the stored value and reports a
    // reset that never happened.
    void noteCommandedPosition(int32_t xactual);

    // The real check: a fresh GSTAT read and an XACTUAL cross-check against
    // hardware, which invalidates and persists the loss if either disagrees.
    // Run it before anything commits to a move, never from a status refresh:
    // a single marginal read would trip a real invalidation (and, with
    // on-demand auto-recovery, a real rehome). referenceStatus() is for
    // display.
    bool referenceValid(std::string& why);

    // What we currently believe, with no hardware access and no side
    // effects. Reflects the last referenceValid() (or rehome); a reset since
    // then is only noticed when something tries to move.
    bool referenceStatus(std::string& why) const;

    void invalidateReference(const std::string& why);

    // --- interruption --------------------------------------------------
    // Set from any thread to break a move out of its polling loop. The abort
    // itself (the SPI writes that halt the ramp) always happens on the worker.
    void requestAbort() { m_abortRequested = true; }
    // fppd is stopping: abort, and never wait on the motor from here on.
    // A job starting during shutdown must not clear it: the abort stays up.
    void requestShutdown() { m_shuttingDown = true; m_abortRequested = true; }
    void clearAbort() { m_abortRequested = m_shuttingDown.load(); }
    bool abortRequested() const { return m_abortRequested || m_shuttingDown; }

    // An extra predicate the preset engine installs so a long move can be cut
    // short when the channel value changes.
    void setPreemptCheck(std::function<bool()> p) { m_preempt = std::move(p); }

    // consensus=false is a single read, for a display refresh taken while
    // the shaft is moving: three consecutive reads of a changing XACTUAL
    // never agree, so the consensus loop would spend its 15 attempts and
    // return the last read anyway. Everything that records a position keeps
    // the consensus read.
    int32_t readXactual(bool consensus = true);
    // Datagrams this axis has put on the bus since it opened.
    uint64_t spiDatagrams() const;

private:
    bool loadReference();
    bool saveReference();
    void recordReference(const std::string& mode, int minFs, int maxFs,
                         int32_t xactual);
    uint32_t latchGstat();
    void acknowledgeReset();
    std::string statePath() const;

    // DRV_STATUS with a fault bit believed only when the read straight after
    // shows it too (see the definition), and VACTUAL with a non-zero value
    // believed only when a second read agrees it is moving. Both count a
    // reading the second read contradicts in m_transientReads.
    uint32_t readDrvStatusConfirmed(const char* where);
    bool movingConfirmed(int32_t& vactual);

    // Backs moveToFullsteps(): referenceValid(), and when that fails, a real
    // rehome() if RehomeConfig::autoRecoverOnDemand is set, survey() is
    // clean and fewer than autoRecoverMaxAttempts have failed since the
    // reference was last good. why is a complete message a caller can
    // surface directly.
    bool attemptAutoRecover(std::string& why);

    // Sign to apply when converting between the home-relative fullsteps
    // frame (0 at home, increasing away from it) and raw motor microsteps.
    // seek()/rehome() approach home in the raw direction the "ccw" flag
    // selects; moving to larger positions must go the OTHER raw direction,
    // so dirSign is -1 when home was approached ccw, +1 when cw. Derived
    // from homeMode rather than stored separately so there is no extra
    // persisted field to get out of sync with it.
    int dirSign() const;

    AxisConfig m_cfg;
    std::string m_dataDir;
    TmcDevice* m_dev = nullptr;
    ReferenceState m_ref;

    // GSTAT is read-and-clear, so whichever read happens first consumes the
    // reset flag. Latched here so a status poll cannot eat the evidence a
    // move is about to check for.
    uint32_t m_gstatSticky = 0;

    // Consecutive automatic rehome attempts since the reference was last
    // good; reset by recordReference().
    int m_autoRecoverAttempts = 0;

    // Surveys whose first IOIN read was rubbish but whose retry was fine with
    // no pin touched. Only ever touched by this axis's own worker thread.
    uint64_t m_transientReads = 0;

    std::atomic<bool> m_abortRequested { false };
    std::atomic<bool> m_shuttingDown { false };
    // DMAX of the last programmed ramp: bounds waitUntilStopped().
    uint32_t m_rampAmax = 0;
    std::function<bool()> m_preempt;
    bool m_coilsEnergised = false;
};

} // namespace tmc
