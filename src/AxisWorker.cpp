#include "fpp-pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "AxisWorker.h"
#include "TmcUtil.h"

#include "fpp-json.h"
#include "log.h"

namespace tmc {

// Snapshot cadence (see maybeRefreshSnapshot). The setup page polls every
// 300 ms while visible and every 2 s when hidden, so a 1.5 s window keeps a
// visible page at the fast cadence through four missed polls while a
// hidden tab or an idle box only costs the slow one.
static constexpr int64_t SNAPSHOT_FAST_MS = 200;
static constexpr int64_t SNAPSHOT_SLOW_MS = 2000;
static constexpr int64_t SNAPSHOT_WATCHED_MS = 1500;

const char* Job::kindName(Kind k) {
    switch (k) {
    case Kind::Probe: return "probe";
    case Kind::Jog: return "jog";
    case Kind::SetHome: return "setHome";
    case Kind::SetMax: return "setMax";
    case Kind::Goto: return "goto";
    case Kind::Cycle: return "cycle";
    case Kind::Rehome: return "rehome";
    case Kind::TestTravel: return "testTravel";
    case Kind::RunPreset: return "runPreset";
    case Kind::Release: return "release";
    case Kind::Park: return "park";
    }
    return "unknown";
}

// ------------------------------------------------------------ lifetime --

AxisWorker::AxisWorker(const AxisConfig& cfg, const std::string& dataDir,
                       int idleTimeoutMs) :
    m_cfg(cfg),
    m_idleTimeoutMs(idleTimeoutMs),
    m_axis(cfg, dataDir) {
}

AxisWorker::~AxisWorker() {
    stop();
}

bool AxisWorker::start(std::string& err) {
    if (m_running)
        return true;
    if (!m_axis.open(err)) {
        LogErr(VB_PLUGIN, "%s: %s: %s\n", LOGPFX, m_cfg.name.c_str(), err.c_str());
        return false;
    }

    // A move is cut short when the channel value moves off whatever we are
    // serving. Installed once so no path can forget it.
    m_axis.setPreemptCheck([this]() {
        // runTo() asks this between its polls, on this thread, so it is also
        // where the snapshot keeps up with a move in progress.
        maybeRefreshSnapshot(true);
        if (!m_running)
            return true;
        // The show always wins, even over a manual job in progress.
        if (m_manualMode)
            return desiredPresetValue() != 0;
        return desiredPresetValue() != m_serving;
    });

    m_running = true;
    m_thread = std::thread(&AxisWorker::run, this);
    LogInfo(VB_PLUGIN, "%s: %s: started on %s (%s)\n", LOGPFX,
            m_cfg.name.c_str(), m_cfg.spi.describe().c_str(), m_cfg.driver->name);
    return true;
}

void AxisWorker::requestStop() {
    if (!m_running)
        return;
    m_running = false;
    m_axis.requestShutdown();
    m_wake.notify_all();
}

void AxisWorker::stop() {
    requestStop();
    if (!m_thread.joinable())
        return;
    m_thread.join();
    m_axis.close();
}

void AxisWorker::showStarting() {
    m_wake.notify_all();
}

// Deliberately does not zero the channel. FPP also stops and closes every
// output when it reloads co-other.json (any save on the Channel Outputs page,
// or this plugin's own entry sync), mid-show, and the replacement output's
// frames carry the same values a moment later. Zeroing here sent every
// stepper home and restarted its preset. A real stop is caught by the idle
// timeout instead (desiredPresetValue()).
void AxisWorker::showStopping() {
    m_wake.notify_all();
}

void AxisWorker::applyTunables(const AxisConfig& cfg, int idleTimeoutMs) {
    std::lock_guard<std::mutex> lk(m_lock);
    m_pendingConfig = cfg;
    m_pendingIdleTimeoutMs = idleTimeoutMs;
    m_pendingConfigSet = true;
    m_wake.notify_all();
}

void AxisWorker::takePendingConfig() {
    AxisConfig cfg;
    int idle;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (!m_pendingConfigSet)
            return;
        cfg = m_pendingConfig;
        idle = m_pendingIdleTimeoutMs;
        m_pendingConfigSet = false;
        // Under the lock: config() copies m_cfg from other threads.
        copyTunables(cfg, m_cfg);
    }
    m_axis.updateTunables(cfg);
    m_idleTimeoutMs = idle;
}

// ---------------------------------------------------------------- jobs --

// Arguments from outside (the page, the API, FPP commands) checked before a
// job is queued. A speed of 0 or less, or a negative current, means "use the
// configured value", as the job code reads them.
static bool validJobArgs(const Job& job, std::string& err) {
    if (std::isnan(job.rps) || job.rps > MAX_RPS) {
        err = "speed must be at most 4 rev/s";
        return false;
    }
    if (job.irun > 31 || job.ihold > 31) {
        err = "irun and ihold must be 0..31";
        return false;
    }
    auto inRange = [](double v) { return std::isfinite(v) && std::fabs(v) <= MAX_MANUAL_FULLSTEPS; };
    if (!inRange(job.delta) || !inRange(job.target) || !inRange(job.minFullsteps) ||
        !inRange(job.maxFullsteps)) {
        err = "a distance or position is not a number, or beyond 1000000 full steps";
        return false;
    }
    return true;
}

uint64_t AxisWorker::submit(const Job& job, std::string& err) {
    if (!validJobArgs(job, err))
        return 0;
    // The show always wins: a non-zero channel value means a sequence is
    // driving this axis right now. desiredPresetValue(), not the raw value:
    // a show that stopped on a non-zero value has timed out to 0 by now.
    const int showValue = desiredPresetValue();
    if (showValue != 0) {
        err = "a sequence is currently driving this stepper (channel value " +
              std::to_string(showValue) +
              "). Stop the sequence, or wait for the channel to return to 0.";
        return 0;
    }
    if (!m_running) {
        err = "this stepper is not running";
        return 0;
    }

    std::lock_guard<std::mutex> lk(m_lock);
    if (m_job.state == "running") {
        err = "this stepper is busy with '" + m_job.kind + "'";
        return 0;
    }
    if (m_queue.size() > 4) {
        err = "too many queued commands";
        return 0;
    }
    uint64_t id = m_nextJobId++;
    m_queue.push_back(job);
    m_queueDepth.store((int)m_queue.size(), std::memory_order_relaxed);
    m_job = JobResult();
    m_job.id = id;
    m_job.kind = Job::kindName(job.kind);
    m_job.state = "running";
    // Visible from the moment the job is accepted: the setup page turns
    // that preset's "Run now" into "Stop" while the job runs.
    if (job.kind == Job::Kind::RunPreset)
        m_job.data["presetValue"] = job.value;
    m_manualMode = true;
    m_axis.clearAbort();
    m_wake.notify_all();
    return id;
}

void AxisWorker::abortNow() {
    m_axis.requestAbort();
    m_wake.notify_all();
}

JobResult AxisWorker::lastJob() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_job;
}

AxisSnapshot AxisWorker::snapshot() {
    m_lastStatusRequestMs.store(monotonicMs(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_lock);
    return m_snapshot;
}

bool AxisWorker::takeJob(Job& out) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_queue.empty())
        return false;
    out = m_queue.front();
    m_queue.pop_front();
    m_queueDepth.store((int)m_queue.size(), std::memory_order_relaxed);
    return true;
}

void AxisWorker::jobLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_job.log.size() < 64)
        m_job.log.push_back(s);
}

void AxisWorker::jobDone(bool ok, const std::string& message) {
    std::lock_guard<std::mutex> lk(m_lock);
    // Stop aborts the move in flight, which surfaces from moveToFullsteps()
    // /rehome() as an ordinary failure. The axis did what Stop asked, so it
    // is not reported as a failed job; recognised here, where every job
    // kind funnels through.
    if (!ok && m_axis.abortRequested()) {
        m_job.state = "stopped";
        m_job.message = "stopped";
        return;
    }
    m_job.state = ok ? "done" : "failed";
    m_job.message = message;
}

void AxisWorker::setActivity(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_lock);
    m_activity = s;
}

// ----------------------------------------------------------- show side --

int AxisWorker::desiredPresetValue() const {
    int v = (int)m_channelValue.load(std::memory_order_relaxed);
    if (v == 0)
        return 0;
    // Frames stopping is the other way a show says "done with this axis"
    // (a sequence ending without a final 0, output shutting down).
    int64_t last = m_lastFrameMs.load(std::memory_order_relaxed);
    if (last != 0 && (monotonicMs() - last) > m_idleTimeoutMs)
        return 0;
    return v;
}

bool AxisWorker::presetChanged(int servingValue) const {
    if (!m_running)
        return true;
    if (m_axis.abortRequested())
        return true;
    if (m_queueDepth.load(std::memory_order_relaxed) > 0)
        return true;
    // A manual run has no channel value holding it up; it ends when it
    // finishes, Stop is pressed, or a sequence writes a non-zero value (the
    // show always wins).
    if (m_manualMode)
        return desiredPresetValue() != 0;
    return desiredPresetValue() != servingValue;
}

// What a preset asks for, as a job. The -1 sentinels carry through;
// runJob() resolves them.
static Job jobFromPreset(int value, const Preset& p, Job::Source source) {
    Job j;
    j.source = source;
    j.value = value;
    j.rps = p.rps;
    j.irun = p.irun;
    j.ihold = p.ihold;
    j.rampTimeSec = p.rampTimeSec;
    switch (p.type) {
    case Preset::Type::Rehome:
        j.kind = Job::Kind::Rehome;
        break;
    case Preset::Type::Goto:
        j.kind = Job::Kind::Goto;
        j.targetIsPercent = p.usePercent;
        j.target = p.usePercent ? p.positionPercent : p.positionFullsteps;
        break;
    case Preset::Type::Cycle:
        j.kind = Job::Kind::Cycle;
        j.cycle = &p;
        break;
    case Preset::Type::Release:
        j.kind = Job::Kind::Release;
        break;
    }
    return j;
}

void AxisWorker::returnHome(int servingValue) {
    // The "already home" shortcut only applies to a trusted reference.
    std::string why;
    if (m_axis.referenceValid(why)) {
        // xactualUsteps, not the rounded positionFullsteps: a preset parked
        // 0.3 of a step from home is not at home.
        if (m_axis.reference().xactualUsteps == 0) {
            releaseIfNoHold();
            setActivity("at home");
            return;
        }
    } else if (!m_showDroveAxis) {
        // Going home now would be a rehome nobody requested.
        setActivity("idle -- " + why);
        return;
    }

    setActivity("returning home");
    std::string err;
    // A plain move on the stored reference when it is good, with the same
    // chance to auto-recover a lost one as any other move -- not a forced
    // rehome every time a sequence ends.
    if (!m_axis.moveToFullsteps(0, DEFAULT_MANUAL_RPS, m_cfg.motor.irun, m_cfg.motor.ihold,
                                err)) {
        if (m_axis.abortRequested()) {
            // Stop was pressed: the axis stays where Stop left it until the
            // channel value next changes.
            setActivity("idle -- " + err);
            LogWarn(VB_PLUGIN, "%s: %s: return home: %s\n", LOGPFX,
                    m_cfg.name.c_str(), err.c_str());
            releaseIfNoHold();
            return;
        }
        if (presetChanged(servingValue))
            return; // the value moved on: that is the normal end of a move
        // A real failure while the channel still asks for home. Retry as a
        // repeat-forever cycle retries a failed leg: pause, then forget the
        // serving value so the next iteration tries again. Logged on the
        // first attempt and every tenth after that.
        m_returnHomeFailures++;
        if (m_returnHomeFailures == 1 || m_returnHomeFailures % 10 == 0)
            LogWarn(VB_PLUGIN,
                    "%s: %s: return home: %s (attempt %d; retrying while the "
                    "channel asks for home)\n",
                    LOGPFX, m_cfg.name.c_str(), err.c_str(), m_returnHomeFailures);
        setActivity("idle -- " + err);
        releaseIfNoHold();
        for (int t = 0; t < 10 && !presetChanged(servingValue); t++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        m_serving = -1;
        return;
    }
    m_returnHomeFailures = 0;
    releaseIfNoHold();
    setActivity("at home");
}

bool AxisWorker::executeCycle(const Job& job, std::string& err) {
    const int value = job.value;
    const Preset& p = *job.cycle;
    const double rps = job.rps > 0 ? job.rps : DEFAULT_MANUAL_RPS;
    const int irun = job.irun >= 0 ? job.irun : m_cfg.motor.irun;
    const int ihold = job.ihold >= 0 ? job.ihold : m_cfg.motor.ihold;
    // repeat 0 means "until something stops us": the channel letting go of
    // this value, or Stop for a manual run (presetChanged() covers both).
    // A show-driven repeat-forever cycle also retries a failed leg after a
    // pause, since the channel is still asking for motion; a manual run
    // reports the failure instead.
    const int limit = (p.repeat > 0) ? p.repeat : 0;
    const bool retryOnFailure = limit == 0 && !m_manualMode;
    int iteration = 0;
    while (!presetChanged(value)) {
        for (size_t i = 0; i < p.waypoints.size(); i++) {
            // Checked between legs, not during one.
            if (presetChanged(value))
                return true;

            const Waypoint& w = p.waypoints[i];
            setActivity("cycle: leg " + std::to_string(i + 1) + "/" +
                        std::to_string(p.waypoints.size()));

            if (!m_axis.moveToFullsteps(w.positionFullsteps, rps, irun, ihold, err,
                                        job.rampTimeSec)) {
                if (presetChanged(value))
                    return true;
                LogWarn(VB_PLUGIN, "%s: %s: cycle leg failed: %s\n", LOGPFX,
                        m_cfg.name.c_str(), err.c_str());
                if (!retryOnFailure)
                    return false;
                setActivity("cycle: leg failed, retrying -- " + err);
                for (int t = 0; t < 10 && !presetChanged(value); t++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                break; // restart the waypoint list from the top
            }

            // Dwell in short slices so a value change is noticed promptly.
            int remaining = w.dwellMs;
            while (remaining > 0 && !presetChanged(value)) {
                int slice = remaining > 50 ? 50 : remaining;
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                remaining -= slice;
            }
        }
        iteration++;
        if (limit > 0 && iteration >= limit) {
            setActivity("cycle complete");
            return true;
        }
    }
    return true;
}

bool AxisWorker::executePreset(int value, std::string& err) {
    if (value == 0) {
        returnHome(value);
        return true;
    }

    auto it = m_cfg.presets.find(value);
    if (it == m_cfg.presets.end()) {
        // Usually a sequence still being built; not worth a log line per
        // frame.
        setActivity("channel " + std::to_string(value) + ": no preset mapped");
        err = "no preset mapped to value " + std::to_string(value);
        return false;
    }
    Json::Value data; // nothing show-side reads it
    return runJob(jobFromPreset(value, it->second, Job::Source::Show), err, data);
}

// ------------------------------------------------------------ execution --

void AxisWorker::noteHomingDirection(bool ccw, Json::Value& data) {
    data["configUpdated"] = false;
    if (ccw == m_cfg.rehome.ccw)
        return;
    // The live reference already approaches this way (TmcAxis::rehome()
    // follows it); the configuration is what a rehome falls back on once
    // the reference is lost, so it has to say the same.
    {
        // config() copies m_cfg under m_lock on HTTP threads.
        std::lock_guard<std::mutex> lk(m_lock);
        m_cfg.rehome.ccw = ccw;
    }
    if (m_onHomingDirection) {
        Json::Value saved = m_onHomingDirection(ccw);
        if (saved.isObject()) {
            data["configUpdated"] = true;
            data["revisionBefore"] = saved["revisionBefore"];
            data["revision"] = saved["revision"];
        }
    }
}

bool AxisWorker::runJob(const Job& job, std::string& err, Json::Value& data) {
    bool ok = false;

    const int irun = job.irun >= 0 ? job.irun : m_cfg.motor.irun;
    const int ihold = job.ihold >= 0 ? job.ihold : m_cfg.motor.ihold;
    const double rps = job.rps > 0 ? job.rps : DEFAULT_MANUAL_RPS;
    const bool show = job.source == Job::Source::Show;

    // Any other job moves the shaft, or re-reads the chip, without being
    // part of an approach to a stop: the jog span starts again after it.
    if (job.kind != Job::Kind::Jog && job.kind != Job::Kind::SetHome)
        m_jogSpan = false;

    switch (job.kind) {
    case Job::Kind::Probe: {
        setActivity("probing");
        AxisHealth h = m_axis.survey();
        data["linkUp"] = h.linkUp;
        data["simulated"] = h.simulated;
        data["driver"] = m_cfg.driver->name;
        data["versionByteExpected"] = (int)m_cfg.driver->versionByte;
        data["versionByteRead"] = (int)((h.ioin >> 24) & 0xFF);
        data["ioin"] = (Json::UInt)h.ioin;
        data["gconf"] = (Json::UInt)h.gconf;
        data["gstat"] = (Json::UInt)h.gstat;
        data["drvStatus"] = (Json::UInt)h.drvStatus;
        data["drvEnnHigh"] = (h.ioin & IOIN_DRV_ENN) != 0;
        data["sdModeHigh"] = (h.ioin & IOIN_SD_MODE) != 0;
        data["csActual"] = h.csActual;
        data["irun"] = irun;
        data["rmsCurrentAmps"] = m_cfg.rmsCurrentAmps(irun);
        data["holdCurrentAmps"] = m_cfg.rmsCurrentAmps(ihold);
        for (const auto& p : h.problems)
            data["problems"].append(p);
        ok = h.ok();
        err = ok ? "" : (h.problems.empty() ? "link down" : h.problems[0]);
        // Probe is step 1 of commissioning, which re-measures the range: a
        // driver that checks out has its far limit forgotten so jog is
        // unclamped for the steps that follow. Position 0 is kept.
        if (ok) {
            std::string cerr;
            if (m_axis.clearMaxHere(cerr)) {
                data["farLimitCleared"] = true;
            } else {
                data["farLimitCleared"] = false;
                data["farLimitError"] = cerr;
            }
        }
        break;
    }

    case Job::Kind::Jog: {
        setActivity("jogging");
        std::string moveErr;
        const TmcAxis::JogResult jr = m_axis.jog(job.delta, rps, irun, ihold, moveErr);
        const bool reached = jr.outcome == MoveOutcome::Reached;
        if (!m_jogSpan) {
            m_jogSpan = true;
            m_jogLo = m_jogHi = jr.beforeUsteps;
        }
        m_jogLo = std::min({ m_jogLo, jr.beforeUsteps, jr.afterUsteps });
        m_jogHi = std::max({ m_jogHi, jr.beforeUsteps, jr.afterUsteps });

        double commanded = job.delta;
        double actual = ustepsToFullsteps(jr.afterUsteps - jr.beforeUsteps);
        data["beforeFullsteps"] = ustepsToFullsteps(jr.beforeUsteps);
        data["afterFullsteps"] = ustepsToFullsteps(jr.afterUsteps);
        data["commandedFullsteps"] = commanded;
        data["actualFullsteps"] = actual;
        // No stop detector: positioning mode is open loop and a held shaft
        // slips without the chip noticing. Whether the jog met the stop is
        // for the operator to judge.
        data["hitSoftLimit"] = jr.clampedToLimit;

        if (jr.clampedToLimit && reached) {
            ok = true;
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "moved %.2f of %.1f full steps and stopped at the soft limit. "
                     "To jog past it, press Probe (or Set Home) first.",
                     actual, commanded);
            err = buf;
        } else if (!reached) {
            ok = false;
            err = moveErr;
        } else {
            ok = true;
        }
        break;
    }

    case Job::Kind::SetHome: {
        setActivity("setting home");
        // The direction home was approached in: stated by the caller, else
        // from the jogs that led here, else the configured one -- never
        // left out, so the reference and the configuration always name one.
        std::string dir = job.approachDir;
        std::string from = "request";
        if (dir != "ccw" && dir != "cw") {
            dir.clear();
            if (m_jogSpan && m_jogHi > m_jogLo) {
                // The stop is at the end of the span the shaft is nearest.
                // Positive raw microsteps drive ccw (TmcAxis::seek).
                const int32_t x = m_axis.readXactual();
                const int64_t toHi = (int64_t)m_jogHi - x;
                const int64_t toLo = (int64_t)x - m_jogLo;
                if (toHi != toLo) {
                    dir = toHi < toLo ? "ccw" : "cw";
                    from = "jogs";
                }
            }
            if (dir.empty()) {
                dir = m_cfg.rehome.ccw ? "ccw" : "cw";
                from = "config";
            }
        }
        ok = m_axis.setHomeHere(job.minFullsteps, job.maxFullsteps, dir, err);
        m_jogSpan = false;
        if (ok) {
            data["approachDir"] = dir;
            data["approachFrom"] = from;
            noteHomingDirection(dir == "ccw", data);
        }
        break;
    }

    case Job::Kind::SetMax: {
        setActivity("setting far limit");
        bool wrongSide = false, flipped = false;
        ok = m_axis.setMaxHere(job.flipDir, wrongSide, flipped, err);
        if (wrongSide)
            data["wrongSide"] = true;
        if (flipped) {
            // The flip is already in the saved reference even if the far
            // limit itself was then refused.
            data["flipped"] = true;
            data["approachDir"] = m_axis.homeApproachedCcw() ? "ccw" : "cw";
            noteHomingDirection(m_axis.homeApproachedCcw(), data);
        }
        if (ok) {
            const ReferenceState& r = m_axis.reference();
            data["minFullsteps"] = r.minFullsteps;
            data["maxFullsteps"] = r.maxFullsteps;
            data["travelFullsteps"] = r.maxFullsteps - r.minFullsteps;
            data["travelDegrees"] = r.travelDeg;
        }
        break;
    }

    case Job::Kind::Goto: {
        // A percentage is resolved against the current soft limits at run
        // time (see Job::target).
        double target = job.target;
        if (job.targetIsPercent) {
            const ReferenceState& r = m_axis.reference();
            target = percentToFullsteps(job.target, r.minFullsteps, r.maxFullsteps);
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "goto %.2f", target);
        setActivity(buf);
        ok = m_axis.moveToFullsteps(target, rps, irun, ihold, err, job.rampTimeSec);
        if (!ok) {
            // A show-driven failure goes to the log -- unless the value
            // moved on, the normal way a show ends a move.
            if (show && !presetChanged(job.value))
                LogWarn(VB_PLUGIN, "%s: %s: goto %.2f: %s\n", LOGPFX,
                        m_cfg.name.c_str(), target, err.c_str());
            break;
        }
        snprintf(buf, sizeof(buf), "holding at %.2f", target);
        setActivity(buf);
        break;
    }

    case Job::Kind::Rehome:
        setActivity("rehoming");
        // Progress lines belong to the manual job that asked; a show-driven
        // rehome has no job open (rehome() logs each stage either way).
        if (show)
            ok = m_axis.rehome(err, nullptr);
        else
            ok = m_axis.rehome(err, [this](const std::string& s) { jobLog(s); });
        if (!ok) {
            if (show)
                LogWarn(VB_PLUGIN, "%s: %s: rehome: %s\n", LOGPFX,
                        m_cfg.name.c_str(), err.c_str());
            break;
        }
        setActivity("rehomed");
        {
            const ReferenceState& r = m_axis.reference();
            data["maxFullsteps"] = r.maxFullsteps;
            data["travelDegrees"] = r.travelDeg;
            data["travelFullsteps"] = r.maxFullsteps - r.minFullsteps;
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "home re-established at the stop; range 0..%d full steps "
                     "(%.1f deg)",
                     r.maxFullsteps, r.travelDeg);
            err = buf;
        }
        break;

    case Job::Kind::Cycle:
        ok = executeCycle(job, err);
        break;

    case Job::Kind::TestTravel: {
        // Run the full travel N times for someone to watch. XACTUAL is the
        // commanded counter, so what is reported here catches a move that
        // failed to finish its ramp, never lost steps (a motor stalling every
        // cycle still reports 0.000), and a rehome cannot measure them either:
        // it zeroes wherever it ends. Nothing here may be worded as if it did.
        const ReferenceState& r = m_axis.reference();
        // "Has this axis ever been commissioned": min/maxFullsteps survive a
        // lost reference. Whether it is currently valid is for the first
        // move to find out, with the chance to auto-recover.
        if (r.maxFullsteps <= r.minFullsteps) {
            err = "no travel range set yet -- find home and the far limit first";
            break;
        }
        int cycles = job.cycles > 0 ? job.cycles : 1;

        // Start from home, then take the baseline.
        setActivity("test travel: moving to home");
        if (!m_axis.moveToFullsteps(r.minFullsteps, rps, irun, ihold, err))
            break;
        int32_t startX = m_axis.readXactual();
        ok = true;
        double worst = 0.0;
        for (int c = 0; c < cycles && ok; c++) {
            if (m_axis.abortRequested()) {
                err = "aborted";
                ok = false;
                break;
            }
            setActivity("test travel " + std::to_string(c + 1) + "/" +
                        std::to_string(cycles));
            ok = m_axis.moveToFullsteps(r.maxFullsteps, rps, irun, ihold, err);
            if (!ok)
                break;
            int32_t atFar = m_axis.readXactual();
            double farErr =
                m_axis.toPositionFullsteps(atFar) - (double)r.maxFullsteps;
            ok = m_axis.moveToFullsteps(r.minFullsteps, rps, irun, ihold, err);
            if (!ok)
                break;
            int32_t atHome = m_axis.readXactual();
            double homeErr =
                m_axis.toPositionFullsteps(atHome) - (double)r.minFullsteps;
            worst = std::max(worst, std::max(std::fabs(farErr), std::fabs(homeErr)));
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "cycle %d: far off by %+.3f, home off by %+.3f full steps",
                     c + 1, farErr, homeErr);
            jobLog(buf);
        }
        int32_t endX = m_axis.readXactual();
        double counterErr = m_axis.toPositionFullsteps(endX) - m_axis.toPositionFullsteps(startX);
        data["cycles"] = cycles;
        data["counterErrorFullsteps"] = counterErr;
        data["counterErrorDegrees"] = counterErr * DEG_PER_FULLSTEP;
        data["worstEndpointErrorFullsteps"] = worst;
        data["irun"] = irun;
        data["ihold"] = ihold;
        data["rps"] = rps;
        data["rmsCurrentAmps"] = m_cfg.rmsCurrentAmps(irun);
        if (ok) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "%d cycles at IRUN %d (%.2f A RMS), %.3f rev/s: step counter off "
                     "by %+.3f full steps (%+.2f deg)",
                     cycles, irun, m_cfg.rmsCurrentAmps(irun), rps, counterErr,
                     counterErr * DEG_PER_FULLSTEP);
            err = buf;
        }
        break;
    }

    case Job::Kind::RunPreset: {
        auto it = m_cfg.presets.find(job.value);
        if (it == m_cfg.presets.end()) {
            err = "no preset mapped to value " + std::to_string(job.value);
            break;
        }
        jobLog("running preset " + std::to_string(job.value) + ": " +
               it->second.describe());
        // The UI identifies the row whose "Run now" is now "Stop" from here;
        // a manual run sets no serving value.
        data["presetValue"] = job.value;
        // The preset's own result data stays out of this job's: Run now
        // reports whether the preset ran.
        Json::Value inner;
        ok = runJob(jobFromPreset(job.value, it->second, Job::Source::Manual), err, inner);
        // No channel drop will trigger returnHome()'s release, so honour
        // holdSeconds here, as Park does.
        releaseIfNoHold();
        break;
    }

    case Job::Kind::Release:
        m_axis.releaseCoils();
        setActivity("released");
        ok = true;
        err = "coils de-energised; the position reference is kept";
        break;

    case Job::Kind::Park:
        setActivity("parking");
        ok = m_axis.moveToFullsteps(0, rps, irun, ihold, err);
        if (ok)
            releaseIfNoHold();
        break;
    }
    return ok;
}

// --------------------------------------------------------- manual side --

void AxisWorker::executeJob(const Job& job) {
    std::string err;
    Json::Value data;
    const bool ok = runJob(job, err, data);
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_job.data = data;
    }
    jobDone(ok, ok ? (err.empty() ? "ok" : err) : err);
    setActivity("idle");
}

// ----------------------------------------------------------- main loop --

// Startup homing is opt-in and runs at most once, so unlike
// attemptAutoRecover() it does not consult autoRecoverMaxAttempts.
void AxisWorker::maybeAutoHomeAtStartup() {
    if (!m_cfg.rehome.autoRecoverAtStartup)
        return;

    std::string why;
    if (m_axis.referenceValid(why))
        return; // a persisted reference survived the restart; nothing to do

    std::string gateErr;
    if (!m_axis.gates(gateErr)) {
        LogWarn(VB_PLUGIN,
                "%s: %s: startup auto-home skipped -- driver is not healthy: "
                "%s\n",
                LOGPFX, m_cfg.name.c_str(), gateErr.c_str());
        return;
    }

    LogInfo(VB_PLUGIN,
            "%s: %s: no valid position reference at startup (%s) -- rehoming "
            "automatically\n",
            LOGPFX, m_cfg.name.c_str(), why.c_str());
    setActivity("rehoming (startup)");
    std::string err;
    // No manual job is open for progress to belong to; rehome() logs each
    // stage itself.
    if (!m_axis.rehome(err, nullptr)) {
        LogWarn(VB_PLUGIN, "%s: %s: startup auto-home failed: %s\n", LOGPFX,
                m_cfg.name.c_str(), err.c_str());
    }
    setActivity("idle");
}

void AxisWorker::maybeReleaseIdleCoils() {
    if (m_cfg.holdSeconds < 0 || !m_axis.coilsEnergised() || m_holdingSinceMs == 0)
        return;
    if (monotonicMs() - m_holdingSinceMs < (int64_t)m_cfg.holdSeconds * 1000)
        return;
    m_axis.releaseCoils();
    setActivity("released (idle)");
}

void AxisWorker::noteActivity() {
    m_holdingSinceMs = monotonicMs();
}

void AxisWorker::releaseIfNoHold() {
    if (m_cfg.holdSeconds == 0)
        m_axis.releaseCoils();
}

void AxisWorker::run() {
    maybeAutoHomeAtStartup();
    noteActivity();

    while (m_running) {
        takePendingConfig();
        maybeReleaseIdleCoils();

        Job job;
        if (takeJob(job)) {
            m_axis.clearAbort();
            // requestStop() clears m_running before it raises the abort, so
            // one landing just before the clear above is not lost.
            if (!m_running)
                m_axis.requestAbort();
            executeJob(job);
            m_axis.clearAbort();
            noteActivity();
            // Manual mode persists after a job so the operator can jog again;
            // it is dropped the moment a sequence writes a non-zero value.
            refreshSnapshot();
            continue;
        }

        int desired = desiredPresetValue();

        if (m_manualMode) {
            // The show always wins: a non-zero channel value revokes manual
            // control immediately, even mid-jog.
            if (desired != 0) {
                LogInfo(VB_PLUGIN,
                        "%s: %s: sequence took over (channel value %d), leaving "
                        "manual mode\n",
                        LOGPFX, m_cfg.name.c_str(), desired);
                m_manualMode = false;
                m_axis.requestAbort();
                m_serving = -1;
            } else {
                std::unique_lock<std::mutex> lk(m_lock);
                m_wake.wait_for(lk, std::chrono::milliseconds(100));
                lk.unlock();
                maybeRefreshSnapshot(false);
                continue;
            }
        }

        if (desired != m_serving) {
            m_serving = desired;
            if (desired > 0)
                m_showDroveAxis = true;
            m_axis.clearAbort();
            if (!m_running) // see the job path above
                m_axis.requestAbort();
            std::string err; // already logged where it happened
            (void)executePreset(desired, err);
            // A return-home that re-armed itself (m_serving back to -1) is a
            // retry, not new activity: the hold countdown is left running.
            if (m_serving == desired)
                noteActivity();
        } else {
            std::unique_lock<std::mutex> lk(m_lock);
            m_wake.wait_for(lk, std::chrono::milliseconds(50));
        }

        maybeRefreshSnapshot(false);
    }

    // Leave the chip quiet, but keep the reference: XACTUAL is untouched.
    m_axis.stopMotion();
    // Released whatever the hold policy: with fppd down nobody watches the
    // driver, so current is never left flowing. A sprung or back-drivable
    // mechanism set to hold indefinitely can move while fppd is stopped.
    if (m_axis.coilsEnergised()) {
        m_axis.releaseCoils();
        LogInfo(VB_PLUGIN, "%s: %s: coils released at shutdown\n", LOGPFX,
                m_cfg.name.c_str());
    }
}

void AxisWorker::maybeRefreshSnapshot(bool moving) {
    const int64_t now = monotonicMs();
    const bool watched =
        now - m_lastStatusRequestMs.load(std::memory_order_relaxed) <= SNAPSHOT_WATCHED_MS;
    if (now - m_lastSnapshotMs <= (watched ? SNAPSHOT_FAST_MS : SNAPSHOT_SLOW_MS))
        return;
    // A move that nobody is watching is left alone entirely: the snapshot
    // is refreshed as soon as the job ends (run()).
    if (moving && !watched)
        return;
    refreshSnapshot(moving);
}

void AxisWorker::refreshSnapshot(bool moving) {
    AxisSnapshot s;
    s.valid = true;
    s.simulated = m_cfg.spi.fake;
    s.manualMode = m_manualMode;
    s.channelValue = (int)m_channelValue.load(std::memory_order_relaxed);
    s.activePreset = m_serving > 0 ? m_serving : 0;

    AxisHealth h = m_axis.survey();
    s.linkUp = h.linkUp;
    s.ioin = h.ioin;
    s.gstat = h.gstat;
    s.drvStatus = h.drvStatus;
    s.spiStatus = h.spiStatus;
    s.csActual = h.csActual;
    s.standstill = h.standstill;
    s.transientReads = h.transientReads;
    s.problems = h.problems;

    if (h.linkUp) {
        s.xactualUsteps = m_axis.readXactual(/*consensus=*/!moving);
        s.positionFullsteps = m_axis.toPositionFullsteps(s.xactualUsteps);
        s.positionDegrees = s.positionFullsteps * DEG_PER_FULLSTEP;
    }

    // referenceStatus(), not referenceValid(): a display refresh must not
    // invalidate a good reference on one marginal read (and, with on-demand
    // auto-recovery, launch a rehome). The real check happens before every
    // move.
    std::string why;
    s.referenceValid = m_axis.referenceStatus(why);
    s.referenceWhy = why;
    s.reference = m_axis.reference();

    s.spiDatagrams = m_axis.spiDatagrams();

    std::lock_guard<std::mutex> lk(m_lock);
    s.activityText = m_activity;
    m_snapshot = s;
    m_lastSnapshotMs = monotonicMs();
}

// ----------------------------------------------------------------- json --

Json::Value snapshotToJson(const AxisSnapshot& s, const AxisConfig& cfg) {
    Json::Value v;
    v["name"] = cfg.name;
    v["driver"] = cfg.driver->name;
    v["simulated"] = s.simulated;
    v["valid"] = s.valid;
    v["linkUp"] = s.linkUp;
    v["manualMode"] = s.manualMode;
    v["channelValue"] = s.channelValue;
    v["activePreset"] = s.activePreset;
    v["activity"] = s.activityText;

    v["xactualUsteps"] = s.xactualUsteps;
    v["positionFullsteps"] = s.positionFullsteps;
    v["positionDegrees"] = s.positionDegrees;
    v["transientReads"] = (Json::UInt64)s.transientReads;
    v["spiDatagrams"] = (Json::UInt64)s.spiDatagrams;

    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", s.ioin);
    v["ioin"] = buf;
    snprintf(buf, sizeof(buf), "0x%08X", s.drvStatus);
    v["drvStatus"] = buf;
    snprintf(buf, sizeof(buf), "0x%02X", s.gstat);
    v["gstat"] = buf;
    snprintf(buf, sizeof(buf), "0x%02X", s.spiStatus);
    v["spiStatus"] = buf;
    v["csActual"] = s.csActual;
    v["standstill"] = s.standstill;
    v["drvEnnHigh"] = (s.ioin & IOIN_DRV_ENN) != 0;
    v["sdModeHigh"] = (s.ioin & IOIN_SD_MODE) != 0;

    for (const auto& p : s.problems)
        v["problems"].append(p);

    v["referenceValid"] = s.referenceValid;
    v["referenceWhy"] = s.referenceWhy;

    Json::Value r;
    r["homed"] = s.reference.homed;
    r["homedAt"] = s.reference.homedAt;
    r["homeMode"] = s.reference.homeMode;
    r["lostWhy"] = s.reference.lostWhy;
    r["lostAt"] = s.reference.lostAt;
    r["minFullsteps"] = s.reference.minFullsteps;
    r["maxFullsteps"] = s.reference.maxFullsteps;
    r["travelDegrees"] = s.reference.travelDeg;
    r["positionFullsteps"] = s.reference.positionFullsteps;
    v["reference"] = r;

    v["runCurrentAmps"] = cfg.rmsCurrentAmps(cfg.motor.irun);
    v["holdCurrentAmps"] = cfg.rmsCurrentAmps(cfg.motor.ihold);
    return v;
}

Json::Value jobToJson(const JobResult& j) {
    Json::Value v;
    v["id"] = (Json::UInt64)j.id;
    v["kind"] = j.kind;
    v["state"] = j.state;
    v["message"] = j.message;
    v["data"] = j.data;
    for (const auto& l : j.log)
        v["log"].append(l);
    return v;
}

} // namespace tmc
