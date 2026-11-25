#pragma once
/*
 * One worker thread per axis.
 *
 * This is the single serialisation point for a driver chip. Show-driven
 * presets and UI-driven commissioning commands both run here, one at a time,
 * so they can never issue overlapping SPI transactions or fight over the ramp
 * generator. Nothing outside this thread touches a TmcDevice.
 *
 * The fppd output thread only ever stores a byte and a timestamp (atomics);
 * the HTTP handlers only ever enqueue a job and read a cached snapshot.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fpp-json.h"

#include "AxisConfig.h"
#include "TmcAxis.h"

namespace tmc {

// One unit of work for the worker thread, whichever side asked for it.
//
// Manual: a commissioning operation from the web UI or an FPP command. Long
// ones run asynchronously: submit returns a job id and the UI watches it
// through the status endpoint.
//
// Show: what a channel value's preset asks for, built by jobFromPreset() on
// the worker when the value is served. The motions are the same code either
// way; only who is told about a failure differs (job result vs fppd log).
struct Job {
    enum class Kind {
        Probe,
        Jog,
        SetHome,
        SetMax,
        Goto,
        Cycle,
        Rehome,
        TestTravel,
        RunPreset,
        Release,
        Park,
    };
    enum class Source {
        Manual,
        Show,
    };

    Kind kind = Kind::Probe;
    Source source = Source::Manual;

    // Goto. A percentage is resolved against the soft limits when the job
    // runs, so re-commissioning the travel range does not move a preset.
    double target = 0;
    bool targetIsPercent = false;
    double delta = 0;         // Jog, full steps
    int minFullsteps = 0;     // SetHome
    int maxFullsteps = 0;     // SetHome
    // "", "ccw" or "cw": the direction home was approached in, when the
    // caller states it. Empty (what the setup page sends) lets the worker
    // work it out from the jogs that led here (see m_jogLo/m_jogHi).
    std::string approachDir;  // SetHome
    // Reverse the recorded homing direction if the far limit turns out to
    // be on the wrong side of home (TmcAxis::setMaxHere).
    bool flipDir = false;     // SetMax
    int cycles = 1;           // TestTravel
    // RunPreset: the preset to run. Show-side jobs: the channel value being
    // served, which is what presetChanged() compares against.
    int value = 0;
    // Cycle: the preset whose waypoints to walk. Points into the worker's
    // own presets map, which is only ever swapped between jobs.
    const Preset* cycle = nullptr;

    // Motion parameters; -1 means "inherit": rps 0.10, currents from the
    // motor config, ramp from motion.rampTimeSec (TmcAxis::runTo).
    double rps = -1.0;
    int irun = -1;
    int ihold = -1;
    double rampTimeSec = -1.0;

    static const char* kindName(Kind k);
};

struct JobResult {
    uint64_t id = 0;
    std::string kind;
    // "idle", "running", "done", "failed"
    std::string state = "idle";
    std::string message;
    std::vector<std::string> log;
    Json::Value data;
};

// Everything the UI shows about an axis, refreshed by the worker so an HTTP
// handler never has to touch SPI.
struct AxisSnapshot {
    bool valid = false;
    bool simulated = false;
    bool linkUp = false;
    bool manualMode = false;

    int32_t xactualUsteps = 0;
    double positionFullsteps = 0.0;
    double positionDegrees = 0.0;

    uint32_t ioin = 0;
    uint32_t gstat = 0;
    uint32_t drvStatus = 0;
    uint8_t spiStatus = 0;
    int csActual = 0;
    bool standstill = false;
    // Bad IOIN reads that a plain retry fixed, with no pin touched. See
    // AxisHealth::transientReads.
    uint64_t transientReads = 0;
    // Datagrams this axis has sent since it opened (TmcDevice::datagrams).
    uint64_t spiDatagrams = 0;
    std::vector<std::string> problems;

    bool referenceValid = false;
    std::string referenceWhy;
    ReferenceState reference;

    int channelValue = 0;
    int activePreset = 0;
    std::string activityText = "idle";
};

class AxisWorker {
public:
    AxisWorker(const AxisConfig& cfg, const std::string& dataDir, int idleTimeoutMs);
    ~AxisWorker();

    const std::string& name() const { return m_cfg.name; }
    // A copy, taken under m_lock: a live tunables save rewrites m_cfg on the
    // worker thread while an HTTP thread asks for it. Worker thread: read
    // m_cfg directly.
    AxisConfig config() const {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_cfg;
    }

    bool start(std::string& err);
    // Asks the worker to finish: the current move ramps down, no new job
    // starts. Returns at once; stop() then joins. Requesting every worker
    // first lets their motors decelerate together.
    void requestStop();
    void stop();

    // Called from the fppd output thread. Must stay allocation- and lock-free.
    void setChannelValue(uint8_t v) { m_channelValue.store(v, std::memory_order_relaxed); }
    void noteFrame(int64_t monotonicMs) {
        m_lastFrameMs.store(monotonicMs, std::memory_order_relaxed);
    }

    // Show is starting or stopping: wake the worker. Ending the show is left
    // to the idle timeout (see showStopping()).
    void showStarting();
    void showStopping();

    // Enqueue a manual job. Refuses while the show is actively driving this
    // axis -- the show always wins. Returns 0 and sets err on refusal.
    uint64_t submit(const Job& job, std::string& err);

    // Emergency stop: breaks whatever move is running out of its poll loop.
    // Always accepted, in any mode.
    void abortNow();

    JobResult lastJob();
    // The cached snapshot. Calling this is what makes the worker keep it
    // fresh: see maybeRefreshSnapshot().
    AxisSnapshot snapshot();
    // Hand the worker new tunables (and the plugin-wide idle timeout). Not
    // applied here: the worker reads its config lock-free (executeCycle()
    // holds a reference into the presets map for a whole cycle), so the copy
    // is parked under m_lock and swapped in by the worker between jobs.
    void applyTunables(const AxisConfig& cfg, int idleTimeoutMs);

    // Called on the worker thread when Set Home or a Set Max flip leaves the
    // live reference approaching home in a direction the configuration does
    // not name, so the owner can save it (TmcManager). Set before start().
    // It returns what to tell the page about the save (null: none).
    void setHomingDirectionListener(std::function<Json::Value(bool ccw)> fn) {
        m_onHomingDirection = std::move(fn);
    }

private:
    void run();
    // Swaps in whatever applyTunables() parked, if anything. Worker thread
    // only, and only at the top of the loop -- never inside a move.
    void takePendingConfig();
    void refreshSnapshot(bool moving = false);
    // The snapshot costs a survey and an XACTUAL read, so it is refreshed
    // at SNAPSHOT_FAST_MS only while something has asked for it within
    // SNAPSHOT_WATCHED_MS, else at SNAPSHOT_SLOW_MS (which keeps the
    // survey's side effects -- the GSTAT latch -- running
    // unattended). Called between jobs and, through the preempt predicate,
    // between the polls of a move. `moving` takes the single XACTUAL read.
    // Worker thread only.
    void maybeRefreshSnapshot(bool moving);
    void setActivity(const std::string& s);
    // Runs once at the top of run(): rehomes unattended if
    // RehomeConfig::autoRecoverAtStartup is set and there is no valid
    // reference. TmcAxis::attemptAutoRecover is the on-demand counterpart.
    void maybeAutoHomeAtStartup();

    // The hold countdown (AxisConfig::holdSeconds): releases the coils in
    // place once nothing has driven this axis for holdSeconds, wherever the
    // shaft is (IHOLD flowing indefinitely is what cooks a bare driver
    // board). Its own timer: the plugin-wide idleTimeoutMs says when quiet
    // frames mean the show is done with an axis, a different question.
    // Checked between jobs and presets, never mid-cycle.
    void maybeReleaseIdleCoils();
    // Called right after a job or preset ran to a conclusion. Restarts the
    // hold countdown, so the next lull starts a fresh one. Not called for a
    // re-armed return-home (returnHome), so an axis that cannot get home
    // still releases when its hold runs out.
    void noteActivity();
    // For a move that ended somewhere it is fine to stay (home, Park, the
    // end of a manual preset run): release at once when holdSeconds is 0,
    // otherwise leave the coils powered for the countdown or for good.
    void releaseIfNoHold();

    bool takeJob(Job& out);
    // The one execution path for manual jobs and show-side presets. False,
    // with err, when the job could not do what it was asked; data receives
    // whatever the kind reports.
    bool runJob(const Job& job, std::string& err, Json::Value& data);
    // Manual wrapper: runJob(), then record the result and go idle.
    void executeJob(const Job& job);
    void jobLog(const std::string& s);
    void jobDone(bool ok, const std::string& message);

    // Show-side execution.
    int desiredPresetValue() const;
    bool presetChanged(int servingValue) const;
    // False, with err, when the preset could not do what it was asked: no
    // preset mapped, a refused or failed move. A manual Run now reports it;
    // a show-driven run only logs it.
    bool executePreset(int value, std::string& err);
    bool executeCycle(const Job& job, std::string& err);
    void returnHome(int servingValue);

    // Worker-thread-owned. Only the worker writes m_cfg, and only under
    // m_lock (takePendingConfig()), so it reads it freely while any other
    // thread goes through config(). name() reads a field copyTunables()
    // never touches.
    AxisConfig m_cfg;
    int m_idleTimeoutMs;
    TmcAxis m_axis;

    // Written by applyTunables() under m_lock, consumed by takePendingConfig().
    bool m_pendingConfigSet = false;
    AxisConfig m_pendingConfig;
    int m_pendingIdleTimeoutMs = 0;

    std::thread m_thread;
    std::atomic<bool> m_running { false };

    std::atomic<uint8_t> m_channelValue { 0 };
    std::atomic<int64_t> m_lastFrameMs { 0 };
    std::atomic<bool> m_manualMode { false };

    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    // Read without the lock by presetChanged(), which runs deep inside a move
    // loop; an atomic depth avoids taking m_lock on every poll.
    std::atomic<int> m_queueDepth { 0 };
    uint64_t m_nextJobId = 1;
    JobResult m_job;
    AxisSnapshot m_snapshot;
    std::string m_activity = "idle";
    // Monotonic ms of the last snapshot() call, i.e. the last status request
    // for this axis. Written by the HTTP thread, read by the worker.
    std::atomic<int64_t> m_lastStatusRequestMs { 0 };
    int64_t m_lastSnapshotMs = 0;

    // See maybeReleaseIdleCoils(). 0 means "no countdown running" (nothing
    // has happened yet this run).
    int64_t m_holdingSinceMs = 0;

    // The preset value currently being served, as seen by the worker.
    int m_serving = -1;
    // Consecutive failed return-home attempts (see returnHome); only for
    // rate-limiting the log line. Reset when the axis gets home.
    int m_returnHomeFailures = 0;

    // Whether a sequence has driven this axis since fppd started. Until it
    // has, returnHome() will not rehome a lost reference on its own: the
    // first loop iteration always "returns home", and an unattended rehome
    // at every start is what RehomeConfig::autoRecoverAtStartup opts in to.
    bool m_showDroveAxis = false;

    // The raw XACTUAL span the jogs since the last other job covered. Set
    // Home takes the approach direction from it: the stop is at whichever
    // end of the span the shaft is nearest, so backing off a step after
    // meeting the stop does not reverse it. Worker thread only.
    bool m_jogSpan = false;
    int32_t m_jogLo = 0;
    int32_t m_jogHi = 0;

    std::function<Json::Value(bool ccw)> m_onHomingDirection;
    // Tell the listener, and keep this worker's copy in step, when the live
    // reference's direction differs from the configured one.
    void noteHomingDirection(bool ccw, Json::Value& data);
};

Json::Value snapshotToJson(const AxisSnapshot& s, const AxisConfig& cfg);
Json::Value jobToJson(const JobResult& j);

} // namespace tmc
