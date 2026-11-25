#include "fpp-pch.h"

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <thread>

#include "TmcAxis.h"

#include "common.h"
#include "fpp-json.h"
#include "log.h"

#include "TmcUtil.h"

namespace tmc {

// The position file's layout, written as "format" (absent means 1).
static constexpr int REFERENCE_FORMAT = 1;


static std::string timestampNow() {
    char buf[32];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// Axis names come from the UI, so they reach the filesystem only through this.
static std::string sanitise(const std::string& s) {
    std::string out = stepperFileStem(s);
    if (out.empty())
        out = "axis";
    return out;
}

// What the reference file belongs to. A reference is only good for the
// hardware it was measured on, so a file written under the same name for
// another driver or chip select is not adopted.
static std::string wiringOf(const AxisConfig& c) {
    std::string w = std::string(c.driver ? c.driver->name : "?") + " ";
    if (c.spi.fake)
        return w + "simulator";
    return w + "SPI" + std::to_string(c.spi.bus) + " " + c.spi.cs;
}

// ------------------------------------------------------------ lifetime --

TmcAxis::TmcAxis(const AxisConfig& cfg, const std::string& dataDir) :
    m_cfg(cfg),
    m_dataDir(dataDir) {
    loadReference();
}

TmcAxis::~TmcAxis() {
    close();
}

bool TmcAxis::open(std::string& err) {
    if (m_dev)
        return true;
    if (m_cfg.spi.fake)
        m_dev = new FakeTmcDevice(*m_cfg.driver);
    else
        m_dev = new SpiTmcDevice(m_cfg.spi.bus, m_cfg.spi.cs, m_cfg.spi.speedHz, m_cfg.name);

    if (!m_dev->open(err)) {
        delete m_dev;
        m_dev = nullptr;
        return false;
    }
    return true;
}

void TmcAxis::close() {
    if (m_dev) {
        m_dev->close();
        delete m_dev;
        m_dev = nullptr;
    }
}

void TmcAxis::updateTunables(const AxisConfig& cfg) {
    copyTunables(cfg, m_cfg);
}

std::string TmcAxis::statePathFor(const std::string& dataDir, const std::string& name) {
    return dataDir + "/axis-" + sanitise(name) + ".json";
}

std::string TmcAxis::statePath() const {
    return statePathFor(m_dataDir, m_cfg.name);
}

// -------------------------------------------------------------- health --

uint32_t TmcAxis::latchGstat() {
    if (!m_dev)
        return m_gstatSticky;
    // Read-and-clear in the chip; sticky here until acknowledged. RESET
    // invalidates a saved reference (persisted to disk), so a reset bit that
    // only one of the read's two datagrams saw is treated as a corrupted
    // byte and dropped.
    bool resetConfirmed = false;
    uint32_t v = m_dev->readGstat(&resetConfirmed);
    if ((v & GSTAT_RESET) && !resetConfirmed) {
        LogWarn(VB_PLUGIN,
                "%s: %s: GSTAT reset flag seen on the data byte but not "
                "corroborated by the address-latch status byte -- treating "
                "as a corrupted read, not a driver reset\n",
                LOGPFX, m_cfg.name.c_str());
        v &= ~GSTAT_RESET;
    }
    // Only the defined flags, so a bit flipped on the wire cannot sit in the
    // sticky value forever. UV_CP is not latched by the chip (it is the
    // current charge-pump state, raised for a few ms at every VS power-up),
    // so it tracks the latest read instead of sticking.
    m_gstatSticky |= v & (GSTAT_ALL_FLAGS & ~GSTAT_UV_CP);
    m_gstatSticky = (m_gstatSticky & ~GSTAT_UV_CP) | (v & GSTAT_UV_CP);
    return m_gstatSticky;
}

void TmcAxis::acknowledgeReset() {
    m_gstatSticky &= ~GSTAT_RESET;
}

// A corrupted DRV_STATUS read is common enough on a noisy rig (a weak motor
// supply under fast moves) to end a show cycle on its own: flipped bits land
// as short-to-ground, short-to-supply or overtemperature flags in
// combinations no real fault produces. Every one of those flags is latched
// by the chip until the driver is disabled or the condition clears, so a real
// fault is still there on the read straight after; a corrupted one is not.
// Only bits seen on BOTH reads are kept, so two corrupted reads in a row
// still have to agree on the same flag.
uint32_t TmcAxis::readDrvStatusConfirmed(const char* where) {
    const uint32_t mask = m_cfg.driver->faultMask;
    const uint32_t first = m_dev->read(REG_DRV_STATUS);
    if (!(first & mask))
        return first;
    const uint32_t second = m_dev->read(REG_DRV_STATUS);
    const uint32_t confirmed = (second & ~mask) | (first & second & mask);
    if (!(confirmed & mask)) {
        m_transientReads++;
        LogDebug(VB_PLUGIN,
                 "%s: %s: DRV_STATUS=0x%08X during %s not confirmed by the next read "
                 "(0x%08X) -- treating as a corrupted read (%llu so far)\n",
                 LOGPFX, m_cfg.name.c_str(), first, where, second,
                 (unsigned long long)m_transientReads);
    }
    return confirmed;
}

// The same for "is it still moving": a corrupted VACTUAL must not refuse a
// move the shaft is standing still for.
bool TmcAxis::movingConfirmed(int32_t& vactual) {
    vactual = m_dev->readSigned(REG_VACTUAL, 24);
    if (vactual == 0)
        return false;
    const int32_t again = m_dev->readSigned(REG_VACTUAL, 24);
    if (again == 0) {
        m_transientReads++;
        LogDebug(VB_PLUGIN,
                 "%s: %s: VACTUAL=%d not confirmed by the next read (0) -- treating "
                 "as a corrupted read (%llu so far)\n",
                 LOGPFX, m_cfg.name.c_str(), vactual,
                 (unsigned long long)m_transientReads);
        vactual = 0;
        return false;
    }
    vactual = again;
    return true;
}

// Every problem survey() reports is a fixed text with the register word(s)
// it was judged from; this is the formatting they share.
static void problem(AxisHealth& h, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void problem(AxisHealth& h, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    h.problems.push_back(buf);
}

AxisHealth TmcAxis::survey() {
    AxisHealth h;
    if (!m_dev) {
        h.problems.push_back("SPI device is not open");
        return h;
    }
    h.simulated = m_dev->isSimulated();

    h.ioin = m_dev->read(REG_IOIN);
    h.spiStatus = m_dev->lastStatus();
    uint8_t version = (uint8_t)((h.ioin >> 24) & 0xFF);

    // A wrong version byte is re-read once before it is reported: a bit
    // flipped on the wire is transient, a driver mismatch or a chip with VS
    // off survives the retry. The pins are taken once, when the device opens
    // (the chip select through FPP's pin API, SPI1's data lines with
    // configPin("spi")), and never re-muxed behind FPP's back.
    if (version != m_cfg.driver->versionByte) {
        h.ioin = m_dev->read(REG_IOIN);
        h.spiStatus = m_dev->lastStatus();
        const uint8_t retried = (uint8_t)((h.ioin >> 24) & 0xFF);
        if (retried == m_cfg.driver->versionByte) {
            m_transientReads++;
            LogDebug(VB_PLUGIN,
                     "%s: %s: transient bad IOIN read, recovered on retry (%llu so far)\n",
                     LOGPFX, m_cfg.name.c_str(),
                     (unsigned long long)m_transientReads);
        }
        version = retried;
    }
    h.transientReads = m_transientReads;

    if (version != m_cfg.driver->versionByte) {
        problem(h,
                "IOIN=0x%08X: VERSION byte is 0x%02X, expected 0x%02X for %s. "
                "All-zero reads usually mean VS is off -- the chip's VCC comes "
                "from its internal regulator off VS. A different non-zero value "
                "means this stepper is configured for the wrong driver.",
                h.ioin, version, m_cfg.driver->versionByte, m_cfg.driver->name);
        return h; // nothing else read from this chip is meaningful
    }
    h.linkUp = true;

    if (h.ioin & IOIN_SD_MODE)
        problem(h,
                "IOIN=0x%08X: SD_MODE is high -- the ramp generator is inert and "
                "the chip is in step/dir mode. Tie SD_MODE low.",
                h.ioin);
    if (h.ioin & IOIN_DRV_ENN)
        problem(h,
                "IOIN=0x%08X: DRV_ENN is high -- the power stage is off, so the "
                "motor will not move however correct everything else is. This "
                "plugin does not drive DRV_ENN: wire it low.",
                h.ioin);

    h.gconf = m_dev->read(REG_GCONF);
    if (m_cfg.driver->gconfIllegalMask &&
        (h.gconf & m_cfg.driver->gconfIllegalMask))
        problem(h,
                "GCONF=0x%08X: analog or internal-Rsense current scaling is "
                "enabled, so IRUN no longer means what the current calculation "
                "assumes. Clear GCONF bits 0 and 1.",
                h.gconf);

    h.gstat = latchGstat();
    if (h.gstat & GSTAT_UV_CP)
        problem(h,
                "GSTAT=0x%02X: charge pump undervoltage -- VS is off or browning "
                "out under load.",
                h.gstat);

    h.drvStatus = readDrvStatusConfirmed("a health check");
    h.transientReads = m_transientReads;
    h.csActual = (int)((h.drvStatus >> 16) & 0x1F);
    h.standstill = (h.drvStatus & DRV_STATUS_STST) != 0;

    if (h.drvStatus & DRV_STATUS_OT)
        problem(h,
                "DRV_STATUS=0x%08X: overtemperature shutdown (150C). Let it cool, "
                "then lower IHOLD -- standstill current is what cooks the board.",
                h.drvStatus);
    else if (h.drvStatus & DRV_STATUS_OTPW)
        problem(h,
                "DRV_STATUS=0x%08X: overtemperature pre-warning (120C). Lower "
                "IHOLD or add cooling.",
                h.drvStatus);
    if (h.drvStatus & (DRV_STATUS_S2GA | DRV_STATUS_S2GB))
        problem(h, "DRV_STATUS=0x%08X: short to ground (s2ga/s2gb)", h.drvStatus);
    if (m_cfg.driver->hasGlobalScaler &&
        (h.drvStatus & (DRV_STATUS_S2VSA | DRV_STATUS_S2VSB)))
        problem(h, "DRV_STATUS=0x%08X: short to supply (s2vsa/s2vsb)", h.drvStatus);
    return h;
}

bool TmcAxis::gates(std::string& err) {
    AxisHealth h = survey();
    if (!h.linkUp) {
        // The link being down means the chip lost power, which means XACTUAL
        // is meaningless. Record that durably before reporting.
        invalidateReference("lost contact with the driver chip -- it most "
                            "likely lost power");
    }
    if (!h.problems.empty()) {
        err = h.problems[0];
        for (size_t i = 1; i < h.problems.size(); i++)
            err += "\n" + h.problems[i];
        return false;
    }
    return true;
}

// -------------------------------------------------------------- motion --

bool TmcAxis::configureCurrents(int irun, int ihold, std::string& err) {
    if (!m_dev) {
        err = "SPI device is not open";
        return false;
    }
    if (!m_dev->writeVerified(REG_SW_MODE, 0, "SW_MODE", err))
        return false;
    m_dev->write(REG_CHOPCONF, m_cfg.motor.chopconf);
    // rmsCurrentAmps() divides by GLOBALSCALER, so the register must hold
    // the same value the arithmetic assumes.
    if (m_cfg.driver->hasGlobalScaler) {
        const int gs = m_cfg.motor.globalScaler;
        m_dev->write(REG_GLOBALSCALER, (gs >= 256) ? 0u : (uint32_t)gs);
    }
    m_dev->write(REG_IHOLD_IRUN,
                 makeIholdIrun(irun, ihold, m_cfg.motor.iholdDelay));
    m_coilsEnergised = true;
    return true;
}

void TmcAxis::stopMotion() {
    if (!m_dev)
        return;
    m_dev->write(REG_VSTART, 0);
    m_dev->write(REG_VMAX, 0);
}

// stopMotion() only starts a deceleration ramp (VMAX=0); the shaft keeps
// decelerating on DMAX after that. Reading XACTUAL before it settles records
// a stale final position, which the next reference check reports as a lost
// reference.
bool TmcAxis::waitUntilStopped() {
    if (!m_dev)
        return true;
    int32_t v = m_dev->readSigned(REG_VACTUAL, 24);
    if (v == 0)
        return true;
    // Deceleration time from |VACTUAL| on DMAX, in seconds: v*2^17/(a*fclk)
    // (velocity is usteps per 2^24/fclk s, acceleration per (2^24/fclk)^2/2^7
    // s^2). Half again for margin, and never under the old fixed second.
    double timeoutSec = 1.0;
    if (m_rampAmax > 0 && m_cfg.motor.fclkHz > 0) {
        double decel = std::fabs((double)v) * 131072.0 /
                       ((double)m_rampAmax * m_cfg.motor.fclkHz);
        timeoutSec = std::max(1.0, decel * 1.5 + 0.25);
    }
    double t0 = monotonicSeconds();
    while ((monotonicSeconds() - t0) < timeoutSec) {
        if (m_shuttingDown)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (m_dev->readSigned(REG_VACTUAL, 24) == 0)
            return true;
    }
    return false;
}

bool TmcAxis::pause(int ms) {
    for (int left = ms; left > 0 && !abortRequested(); left -= 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(left < 20 ? left : 20));
    return !abortRequested();
}

// Every way out of runTo()'s poll loop: ramp down, let the shaft settle,
// read where it really stopped.
int32_t TmcAxis::stopAndReadEnd() {
    stopMotion();
    if (m_shuttingDown) {
        // fppd waits on this thread: no standstill wait here.
        int32_t x = m_dev->readConsensusSigned(REG_XACTUAL, 32);
        if (m_dev->readSigned(REG_VACTUAL, 24) != 0)
            invalidateReference("fppd stopped while this stepper was moving, and its "
                                "coils were released before it came to rest");
        return x;
    }
    const bool stopped = waitUntilStopped();
    if (!stopped) {
        m_ref.settling = true;
        LogWarn(VB_PLUGIN, "%s: %s: still decelerating after the stop wait; the "
                "position is re-read before the next move\n", LOGPFX, m_cfg.name.c_str());
    }
    return m_dev->readConsensusSigned(REG_XACTUAL, 32);
}

void TmcAxis::releaseCoils() {
    if (!m_dev)
        return;
    m_dev->write(REG_VMAX, 0);
    m_dev->write(REG_IHOLD_IRUN, 0);
    // TOFF=0 turns the chopper off entirely. The position reference is kept:
    // XACTUAL is untouched.
    m_dev->write(REG_CHOPCONF, m_cfg.motor.chopconf & ~CHOPCONF_TOFF_MASK);
    m_coilsEnergised = false;
}

int32_t TmcAxis::readXactual(bool consensus) {
    if (!m_dev)
        return m_ref.xactualUsteps;
    return consensus ? m_dev->readConsensusSigned(REG_XACTUAL, 32)
                     : m_dev->readSigned(REG_XACTUAL, 32);
}

uint64_t TmcAxis::spiDatagrams() const {
    return m_dev ? m_dev->datagrams() : 0;
}

// Programs the ramp for a requested speed and returns the VMAX actually
// written -- at these speeds materially lower than the request, VMAX being
// an integer register.
//
// A normal move derives AMAX from a ramp TIME (MotionConfig::rampTimeSec),
// so a faster request gets a proportionally steeper ramp; with a fixed
// acceleration a short-travel axis never reaches a higher VMAX. Homing
// passes a fixed AMAX (RehomeConfig::amax): its stages run at speeds an
// order of magnitude apart, and a ramp that loses sync while finding a hard
// stop gets latched as home with no error.
static uint32_t programRamp(TmcDevice* dev, double rps, double fclkHz,
                            bool dynamicAccel, double rampTimeSec,
                            uint32_t fixedAmax, uint32_t& amaxOut) {
    uint32_t amax = dynamicAccel
        ? amaxForAccel((rps * FULLSTEPS_PER_REV) / rampTimeSec, fclkHz)
        : fixedAmax;
    dev->write(REG_VSTART, 0);
    dev->write(REG_A1, amax);
    dev->write(REG_V1, 0);
    dev->write(REG_AMAX, amax);
    dev->write(REG_DMAX, amax);
    dev->write(REG_D1, amax);
    dev->write(REG_VSTOP, 10);
    amaxOut = amax;
    // VMAX is NOT written here: in positioning mode a non-zero VMAX sets the
    // chip moving towards whatever XTARGET holds. runTo() writes it last.
    return vmaxForRps(rps, fclkHz);
}

MoveOutcome TmcAxis::runTo(int32_t xTarget, double rps, const char* label,
                           int32_t& xEnd, std::string& err, bool dynamicAccel,
                           double rampTimeSec) {
    // Non-positive means "no override" (Preset's -1/blank sentinel). The
    // floor guards the division: parsing rejects anything shorter, but a
    // zero that reached here would program AMAX from infinity.
    if (rampTimeSec <= 0)
        rampTimeSec = m_cfg.motion.rampTimeSec;
    if (rampTimeSec < MIN_RAMP_TIME_SEC)
        rampTimeSec = MIN_RAMP_TIME_SEC;

    if (!m_dev) {
        err = "SPI device is not open";
        return MoveOutcome::NotStarted;
    }

    int32_t x0 = m_dev->readConsensusSigned(REG_XACTUAL, 32);
    xEnd = x0;
    // A Stop or fppd's shutdown that has already landed: never start the
    // motor only to stop it again 20 ms later.
    if (abortRequested()) {
        err = "move stopped";
        return MoveOutcome::Interrupted;
    }

    int32_t v = 0;
    if (movingConfirmed(v)) {
        err = std::string("VACTUAL=") + std::to_string(v) +
              " - the motor is already moving; refusing to start another move";
        return MoveOutcome::NotStarted;
    }

    // Order matters. In positioning mode the chip heads for XTARGET whenever
    // VMAX is non-zero, and an interrupted move leaves the old target there.
    // So: VMAX to 0, mode and ramp, the new target written and verified, and
    // only then VMAX. Any failure on the way leaves VMAX at 0.
    stopMotion();
    if (!m_dev->writeVerified(REG_RAMPMODE, RAMPMODE_POSITIONING, "RAMPMODE", err))
        return MoveOutcome::NotStarted;

    uint32_t vmax = programRamp(m_dev, rps, m_cfg.motor.fclkHz, dynamicAccel,
                                rampTimeSec, m_cfg.rehome.amax, m_rampAmax);

    // The timeout comes from the velocity the chip will really run at: at
    // 0.02 rev/s it differs from the request enough to fire mid-move.
    // Plus the time to reach that speed and to stop again on the programmed
    // AMAX/DMAX (v*2^17/(a*fclk) each way, as in waitUntilStopped()): with a
    // fixed rehome AMAX as low as 10 that alone is several seconds, and a
    // short stage would otherwise be reported as falling short.
    double ustepsPerSec = ustepsPerSecForVmax(vmax, m_cfg.motor.fclkHz);
    double dist = std::fabs((double)xTarget - (double)x0);
    double rampSec = 0.0;
    if (m_rampAmax > 0 && m_cfg.motor.fclkHz > 0)
        rampSec = 2.0 * (double)vmax * 131072.0 / ((double)m_rampAmax * m_cfg.motor.fclkHz);
    double timeout = (ustepsPerSec > 0) ? (dist / ustepsPerSec * 2.0 + rampSec + 4.0) : 10.0;

    if (!m_dev->writeVerified(REG_XTARGET, (uint32_t)xTarget, "XTARGET", err)) {
        stopMotion();
        return MoveOutcome::NotStarted;
    }
    if (abortRequested()) {
        err = "move stopped";
        return MoveOutcome::Interrupted;
    }
    m_dev->write(REG_VMAX, vmax);

    double t0 = monotonicSeconds();
    bool reached = false;
    bool interrupted = false;

    // Open-load watch. A driver with no motor still ramps XACTUAL perfectly,
    // so every move "succeeds" and a rehome records a home that means
    // nothing. The chip's ola/olb flags are the only evidence and are only
    // valid in slow motion, so: slow moves only (the datasheet warns of
    // false positives at show speeds), only after nearly every sample for a
    // good fraction of a second (a single flagged read is normal at the
    // start of a move), and BOTH coils -- one coil flagged alone is what a
    // very slow, low-current move reports on a good motor.
    constexpr double OPEN_LOAD_MAX_RPS = 0.5;
    constexpr int OPEN_LOAD_MIN_SAMPLES = 15; // x 20ms = 300ms of motion
    const bool watchOpenLoad = rps <= OPEN_LOAD_MAX_RPS;
    int olSamples = 0, olHits = 0;

    while ((monotonicSeconds() - t0) < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        if (abortRequested() || (m_preempt && m_preempt())) {
            interrupted = true;
            break;
        }

        uint32_t d = readDrvStatusConfirmed(label);
        if (d & m_cfg.driver->faultMask) {
            xEnd = stopAndReadEnd();
            char buf[160];
            snprintf(buf, sizeof(buf), "driver fault during %s: DRV_STATUS=0x%08X",
                     label, d);
            err = buf;
            return MoveOutcome::Fault;
        }
        if (watchOpenLoad) {
            olSamples++;
            if ((d & (DRV_STATUS_OLA | DRV_STATUS_OLB)) == (DRV_STATUS_OLA | DRV_STATUS_OLB))
                olHits++;
            if (olSamples >= OPEN_LOAD_MIN_SAMPLES && olHits * 4 >= olSamples * 3) {
                xEnd = stopAndReadEnd();
                char buf[224];
                snprintf(buf, sizeof(buf),
                         "open load on both coils during %s (%d of %d samples): no motor "
                         "is connected to this driver; DRV_STATUS=0x%08X",
                         label, olHits, olSamples, d);
                err = buf;
                return MoveOutcome::OpenLoad;
            }
        }
        if (m_dev->read(REG_RAMP_STAT) & RAMP_STAT_POSITION_REACHED) {
            reached = true;
            break;
        }
    }

    xEnd = stopAndReadEnd();

    if (interrupted) {
        err = abortRequested() ? "move stopped" : "move interrupted by a channel change";
        return MoveOutcome::Interrupted;
    }
    if (!reached) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "%s did not report position_reached within %.1fs (at %+.2f full "
                 "steps, wanted %+.2f)",
                 label, timeout, ustepsToFullsteps(xEnd),
                 ustepsToFullsteps(xTarget));
        err = buf;
        return MoveOutcome::FellShort;
    }
    return MoveOutcome::Reached;
}

bool TmcAxis::zeroHere(std::string& err) {
    if (!m_dev) {
        err = "SPI device is not open";
        return false;
    }
    // HOLD mode first: writing XACTUAL in positioning mode STARTS A MOTION
    // towards whatever XTARGET still holds (datasheet, register 0x21).
    if (!m_dev->writeVerified(REG_RAMPMODE, RAMPMODE_HOLD, "RAMPMODE (hold)", err))
        return false;
    if (!m_dev->writeVerified(REG_XTARGET, 0, "XTARGET", err))
        return false;
    if (!m_dev->writeVerified(REG_XACTUAL, 0, "XACTUAL", err))
        return false;
    if (!m_dev->writeVerified(REG_RAMPMODE, RAMPMODE_POSITIONING, "RAMPMODE", err))
        return false;
    // Whatever the last stop left settling has just been overwritten.
    m_ref.settling = false;
    return true;
}

int TmcAxis::dirSign() const {
    return (m_ref.homeMode.find("ccw") != std::string::npos) ? -1 : 1;
}

double TmcAxis::toPositionFullsteps(int32_t usteps) const {
    return dirSign() * ustepsToFullsteps(usteps);
}

int32_t TmcAxis::toUsteps(double fullsteps) const {
    return dirSign() * fullstepsToUsteps(fullsteps);
}

bool TmcAxis::moveToFullsteps(double fullsteps, double rps, int irun, int ihold,
                              std::string& err, double rampTimeSec) {
    {
        std::string why;
        if (!attemptAutoRecover(why)) {
            err = why;
            return false;
        }
        if (fullsteps < m_ref.minFullsteps || fullsteps > m_ref.maxFullsteps) {
            char buf[220];
            snprintf(buf, sizeof(buf),
                     "target %.2f is outside the soft limits %d..%d full steps "
                     "(%+.1f..%+.1f deg)",
                     fullsteps, m_ref.minFullsteps, m_ref.maxFullsteps,
                     m_ref.minFullsteps * DEG_PER_FULLSTEP,
                     m_ref.maxFullsteps * DEG_PER_FULLSTEP);
            err = buf;
            return false;
        }
    }

    if (!configureCurrents(irun, ihold, err))
        return false;

    int32_t xEnd = 0;
    const bool ok = runTo(toUsteps(fullsteps), rps, "move", xEnd, err,
                          /*dynamicAccel=*/true, rampTimeSec) == MoveOutcome::Reached;

    // Record where it ended up even on failure: the reference has to follow
    // the shaft or the next cross-check reports a phantom reset.
    m_ref.xactualUsteps = xEnd;
    m_ref.positionFullsteps = (int)lround(toPositionFullsteps(xEnd));
    saveReference();
    return ok;
}

TmcAxis::JogResult TmcAxis::jog(double deltaFullsteps, double rps, int irun,
                                int ihold, std::string& err) {
    JogResult r;
    if (!gates(err))
        return r;
    if (!configureCurrents(irun, ihold, err))
        return r;
    r.beforeUsteps = readXactual();
    r.afterUsteps = r.beforeUsteps;
    int32_t target = r.beforeUsteps + fullstepsToUsteps(deltaFullsteps);

    // Soft limits only apply once a real span exists: before setMaxHere()
    // has run, min == max == 0, and clamping to that would make it
    // impossible to jog away from home to find the far end.
    if (m_ref.maxFullsteps > m_ref.minFullsteps) {
        int32_t lo = toUsteps(m_ref.minFullsteps);
        int32_t hi = toUsteps(m_ref.maxFullsteps);
        if (lo > hi)
            std::swap(lo, hi);
        if (target < lo) {
            target = lo;
            r.clampedToLimit = true;
        } else if (target > hi) {
            target = hi;
            r.clampedToLimit = true;
        }
    }

    r.outcome = runTo(target, rps, "jog", r.afterUsteps, err);
    // The reference follows every commanded move, cut short or not, or the
    // next check reports a reset that never happened.
    noteCommandedPosition(r.afterUsteps);
    return r;
}

bool TmcAxis::seek(bool ccw, int travelFs, double overtravelDeg, double rps,
                   int irun, int ihold, bool definesHome, std::string& err) {
    if (!gates(err))
        return false;
    if (!configureCurrents(irun, ihold, err))
        return false;

    double totalDeg = travelFs * DEG_PER_FULLSTEP + overtravelDeg;
    int32_t usteps = (int32_t)lround(totalDeg / 360.0 * USTEPS_PER_REV);
    // Positive usteps drives ccw.
    if (!ccw)
        usteps = -usteps;

    // Zero at the current position first, so the commanded distance is
    // relative to wherever we are now. That throws the old reference away:
    // say so now, so nothing (a settling stop below among them) can take
    // the re-zeroed counter for the old position if the seek fails.
    if (m_ref.homed) {
        m_ref.homed = false;
        m_ref.lostWhy = "a rehome was started and did not finish";
        m_ref.lostAt = timestampNow();
        saveReference();
    }
    if (!zeroHere(err))
        return false;

    int32_t xEnd = 0;
    // Positioning mode is open loop: the shaft slips against the stop for
    // the overtravel, the counter finishes anyway, and where the shaft
    // actually is -- the stop -- becomes zero below. Reached is the one
    // success and says nothing about whether a stop was met (StallGuard is
    // unusable at these speeds): a commissioned range shorter than the real
    // travel zeroes short of the stop. FellShort is a chip reset or a dead
    // link, not a stall. A move cut short, a fault or an open load leaves
    // nothing that may become position 0.
    switch (runTo(usteps, rps, "seek", xEnd, err, /*dynamicAccel=*/false)) {
    case MoveOutcome::Reached:
        break;
    case MoveOutcome::FellShort:
    case MoveOutcome::Interrupted:
        err = "seek " + err;
        return false;
    case MoveOutcome::Fault:
    case MoveOutcome::OpenLoad:
    case MoveOutcome::NotStarted:
        return false;
    }

    if (!pause(400)) {
        err = "seek aborted";
        return false;
    }
    if (!zeroHere(err))
        return false;

    const std::string mode = std::string("sought the stop, ") + (ccw ? "ccw" : "cw");
    if (definesHome) {
        recordReference(mode, 0, travelFs, 0);
        return true;
    }
    // Not a home yet: a rehome cut short after this must not leave the
    // coarse contact trusted, nor refill the auto-recovery budget. The
    // direction is noted because toUsteps() needs it for the backoff.
    m_ref.homeMode = mode;
    m_ref.lostWhy = "a rehome was started and did not finish";
    m_ref.lostAt = timestampNow();
    m_ref.xactualUsteps = 0;
    m_ref.positionFullsteps = 0;
    saveReference();
    return true;
}

bool TmcAxis::rehome(std::string& err,
                     const std::function<void(const std::string&)>& progress) {
    const RehomeConfig& r = m_cfg.rehome;

    // The full travel is what the coarse pass must cover; restored at the
    // end because stage 3 seeks with travelFs 0. The commissioned range in
    // the reference wins over config.json's (setMaxHere() never writes the
    // config; the config range is only the fallback for an axis never
    // commissioned). min/maxFullsteps survive a lost reference
    // (invalidateReference only clears `homed`), so this still finds the
    // real range from attemptAutoRecover().
    int travelFs = m_ref.maxFullsteps - m_ref.minFullsteps;
    if (travelFs <= 0)
        travelFs = m_cfg.limits.maxFullsteps;
    if (travelFs <= 0) {
        err = "this stepper has no travel range yet -- run the commissioning wizard "
              "to find home and the far limit first";
        return false;
    }
    if (r.backoffSteps >= travelFs) {
        err = "rehome backoffSteps must be smaller than the travel range";
        return false;
    }

    // Once a reference exists, approach the same raw direction it was
    // established in (dirSign()), not RehomeConfig::ccw -- a config value
    // disagreeing with Set Home would drive into the opposite stop. r.ccw is
    // only the first guess when nothing has ever been referenced.
    bool ccw = m_ref.homed ? (dirSign() < 0) : r.ccw;

    auto say = [&](const std::string& s) {
        LogInfo(VB_PLUGIN, "%s: %s: %s\n", LOGPFX, m_cfg.name.c_str(), s.c_str());
        if (progress)
            progress(s);
    };

    // Stage 1: hit the stop once at a usable speed to get into the
    // neighbourhood.
    say("stage 1/3: coarse approach");
    if (!seek(ccw, travelFs, r.coarseOvertravelDeg, r.coarseRps, r.coarseIrun,
              r.coarseIhold, /*definesHome=*/false, err))
        return false;
    if (!pause(r.settleMs)) {
        err = "rehome aborted";
        return false;
    }

    // Stage 2: retract a few steps so there is room for a slow final approach.
    say("stage 2/3: back off");
    if (!configureCurrents(r.creepIrun, r.creepIhold, err))
        return false;
    // Back off AWAY from the stop just contacted: seek() recorded this
    // attempt's direction in homeMode, so toUsteps() has the right sign.
    int32_t xEnd = 0;
    if (runTo(toUsteps(r.backoffSteps), r.backoffRps, "backoff", xEnd,
              err, /*dynamicAccel=*/false) != MoveOutcome::Reached)
        return false;
    if (!pause(r.settleMs)) {
        err = "rehome aborted";
        return false;
    }

    // Stage 3: creep back in slowly at low current -- too little momentum
    // to bounce -- and zero on THAT contact. travelFs is 0 on purpose: seek()
    // re-zeroes at the current position, only backoffSteps from the stop;
    // the real travel would mean a long low-current stall jammed against
    // the stop, which lets the rotor desync and walk away from it.
    say("stage 3/3: slow creep -- this contact defines zero");
    double creepOvertravelDeg = r.creepOvertravelSteps * DEG_PER_FULLSTEP;
    if (!seek(ccw, 0, creepOvertravelDeg, r.creepRps, r.creepIrun,
              r.creepIhold, /*definesHome=*/true, err))
        return false;
    // The creep contact is already zero: an abort here only cuts the settle
    // short, and the range below is restored either way.
    pause(r.settleMs);

    // seek() just wrote max_fullsteps = 0 (it was told travelFs 0). Restore
    // the real range now that the reference itself is established.
    recordReference(std::string("rehomed to the stop, ") + (ccw ? "ccw" : "cw"),
                    0, travelFs, 0);

    // Stage 3 left the creep currents on the chip. Put the running currents
    // back so the axis holds properly afterwards.
    if (!configureCurrents(m_cfg.motor.irun, m_cfg.motor.ihold, err))
        return false;

    say("rehomed: position 0 is the stop, range 0.." + std::to_string(travelFs) +
        " full steps");
    return true;
}

bool TmcAxis::setHomeHere(int minFullsteps, int maxFullsteps,
                          const std::string& approachDir, std::string& err) {
    if (!gates(err))
        return false;
    if (minFullsteps > 0 || maxFullsteps < 0) {
        err = "min must be <= 0 and max must be >= 0 (home is position 0)";
        return false;
    }
    int32_t v = 0;
    if (movingConfirmed(v)) {
        err = "the motor is moving; refusing to set a reference";
        return false;
    }

    // Energise BEFORE zeroing: on a sprung mechanism a de-energised shaft
    // moves the instant the reference is declared, and the twitch to the
    // nearest microstep detent as current comes on must happen before the
    // zero is latched.
    if (!configureCurrents(m_cfg.motor.irun, m_cfg.motor.ihold, err))
        return false;
    if (!pause(300)) {
        err = "set home aborted";
        return false;
    }

    if (!zeroHere(err))
        return false;
    // The approach direction goes into the mode string as seek()/rehome()
    // write it, so dirSign() agrees with a later Rehome.
    std::string mode = "declared at the current position";
    if (approachDir == "ccw" || approachDir == "cw")
        mode += ", " + approachDir;
    recordReference(mode, minFullsteps, maxFullsteps, 0);
    return true;
}

bool TmcAxis::setMaxHere(bool flip, bool& wrongSide, bool& flipped, std::string& err) {
    wrongSide = false;
    flipped = false;
    std::string why;
    if (!referenceValid(why)) {
        err = "position isn't known (" + why +
              "). Set home before setting the far limit.";
        return false;
    }
    int32_t x = readXactual();
    int fs = (int)lround(toPositionFullsteps(x));
    // Home at a stop (min 0) and a far end at a negative position: the
    // shaft went through the stop, which it cannot, so the recorded
    // approach direction is backwards.
    if (fs < 0 && m_ref.minFullsteps == 0) {
        if (!flip) {
            wrongSide = true;
            const bool ccw = dirSign() < 0;
            err = std::string("the far limit is on the other side of home: home is recorded as "
                              "approached ") +
                  (ccw ? "counter-clockwise" : "clockwise") +
                  ", but the travel from it runs the same way. Flip the homing direction "
                  "to set the far limit here";
            return false;
        }
        // Reverse the direction token seek()/setHomeHere() put in homeMode;
        // dirSign() follows it. An undecorated mode counts as cw.
        std::string& mode = m_ref.homeMode;
        const bool ccw = dirSign() < 0;
        const std::string from = ccw ? ", ccw" : ", cw";
        const size_t at = mode.rfind(from);
        if (at != std::string::npos && at + from.size() == mode.size())
            mode.erase(at);
        mode += ccw ? ", cw" : ", ccw";
        flipped = true;
        fs = (int)lround(toPositionFullsteps(x));
    }
    if (fs <= m_ref.minFullsteps) {
        err = "the far limit must be past home; jog away from home first";
        return false;
    }
    m_ref.maxFullsteps = fs;
    m_ref.travelDeg = (m_ref.maxFullsteps - m_ref.minFullsteps) * DEG_PER_FULLSTEP;
    m_ref.xactualUsteps = x;
    m_ref.positionFullsteps = fs;
    return saveReference();
}

bool TmcAxis::clearMaxHere(std::string& err) {
    if (!gates(err))
        return false;
    int32_t x = readXactual();
    m_ref.maxFullsteps = 0;
    m_ref.travelDeg = 0;
    m_ref.xactualUsteps = x;
    m_ref.positionFullsteps = (int)lround(toPositionFullsteps(x));
    return saveReference();
}

// ----------------------------------------------------------- reference --

bool TmcAxis::referenceValid(std::string& why) {
    if (!m_ref.homed) {
        why = m_ref.lostWhy.empty() ? "not yet homed" : m_ref.lostWhy;
        return false;
    }

    uint32_t gstat = latchGstat();
    if (gstat & GSTAT_RESET) {
        why = "the driver chip reset, most likely a brief power drop -- its "
              "position can no longer be trusted";
        invalidateReference(why);
        return false;
    }

    int32_t xNow = readXactual();
    if (m_ref.settling) {
        // The last stop was recorded mid-ramp. The chip finished it on its
        // own and did not reset (checked above), so its XACTUAL is where the
        // shaft came to rest.
        if (!waitUntilStopped()) {
            why = "still decelerating from the last move";
            return false;
        }
        xNow = readXactual();
        m_ref.settling = false;
        noteCommandedPosition(xNow);
    }
    if (xNow != m_ref.xactualUsteps) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "the position doesn't match what was expected (wanted %d, "
                 "driver reports %d) -- either it reset, or something else "
                 "moved the shaft",
                 m_ref.xactualUsteps, xNow);
        why = buf;
        invalidateReference(why);
        return false;
    }
    return true;
}

bool TmcAxis::referenceStatus(std::string& why) const {
    if (!m_ref.homed) {
        why = m_ref.lostWhy.empty() ? "not yet homed" : m_ref.lostWhy;
        return false;
    }
    why.clear();
    return true;
}

bool TmcAxis::attemptAutoRecover(std::string& why) {
    if (referenceValid(why))
        return true;

    if (!m_cfg.rehome.autoRecoverOnDemand)
        return false;

    // Rehoming drives into a hard stop; with the driver reporting trouble
    // (undervoltage, a short, the link down) it would fail partway or land
    // on a false stop. This check does not spend an attempt: a supply that
    // comes back gets the full budget.
    std::string gateErr;
    if (!gates(gateErr)) {
        why = "position isn't known (" + why + ") and the driver isn't healthy "
              "enough to rehome by itself: " + gateErr;
        return false;
    }

    if (m_autoRecoverAttempts >= m_cfg.rehome.autoRecoverMaxAttempts) {
        why = "position isn't known (" + why + "); already tried rehoming " +
              std::to_string(m_autoRecoverAttempts) + " time(s) and gave up -- "
              "press Rehome manually";
        return false;
    }

    m_autoRecoverAttempts++;
    LogWarn(VB_PLUGIN,
            "%s: %s: position isn't known (%s) -- rehoming automatically (%d/%d)\n",
            LOGPFX, m_cfg.name.c_str(), why.c_str(), m_autoRecoverAttempts,
            m_cfg.rehome.autoRecoverMaxAttempts);

    std::string rehomeErr;
    if (!rehome(rehomeErr, nullptr)) {
        // Stop or a channel change is not a failed attempt: a show that cuts
        // two rehomes short must not use up the budget. Callers treat it as
        // a normal end and log nothing, so it is logged here.
        if (!m_ref.homed && (abortRequested() || (m_preempt && m_preempt()))) {
            m_autoRecoverAttempts--;
            LogInfo(VB_PLUGIN,
                    "%s: %s: automatic rehome did not finish (%s); position still unknown\n",
                    LOGPFX, m_cfg.name.c_str(), rehomeErr.c_str());
            why = "position isn't known, the automatic rehome was interrupted: " + rehomeErr;
            return false;
        }
        why = "position isn't known, automatic rehome attempt " +
              std::to_string(m_autoRecoverAttempts) + " failed: " + rehomeErr;
        return false;
    }
    return true;
}

void TmcAxis::noteCommandedPosition(int32_t xactual) {
    m_ref.xactualUsteps = xactual;
    m_ref.positionFullsteps = (int)lround(toPositionFullsteps(xactual));
    saveReference();
}

void TmcAxis::invalidateReference(const std::string& why) {
    // Persisted because neither check can be repeated later: GSTAT's reset
    // flag is read-and-clear, and the XACTUAL cross-check is blind when the
    // last known position was 0 (a reset leaves XACTUAL at 0 too).
    if (!m_ref.homed)
        return;
    m_ref.homed = false;
    m_ref.lostWhy = why;
    m_ref.lostAt = timestampNow();
    saveReference();
    LogWarn(VB_PLUGIN, "%s: %s: position reference lost: %s\n", LOGPFX,
            m_cfg.name.c_str(), why.c_str());
}

void TmcAxis::recordReference(const std::string& mode, int minFs, int maxFs,
                              int32_t xactual) {
    m_ref.homed = true;
    m_ref.settling = false;
    m_ref.homedAt = timestampNow();
    m_ref.homeMode = mode;
    m_ref.lostWhy.clear();
    m_ref.lostAt.clear();
    m_ref.minFullsteps = minFs;
    m_ref.maxFullsteps = maxFs;
    m_ref.travelDeg = (maxFs - minFs) * DEG_PER_FULLSTEP;
    m_ref.xactualUsteps = xactual;
    m_ref.positionFullsteps = (int)lround(ustepsToFullsteps(xactual));

    // A reset flag latched on the way here is no longer evidence against
    // the reference just established; the next loss gets a fresh budget.
    acknowledgeReset();
    m_autoRecoverAttempts = 0;
    saveReference();
}

// The state file's fields, named once for both directions so nothing can be
// saved that the next start forgets to load.
template <typename F>
static void referenceFields(ReferenceState& r, F&& f) {
    f("homed", r.homed);
    f("homedAt", r.homedAt);
    f("homeMode", r.homeMode);
    f("lostWhy", r.lostWhy);
    f("lostAt", r.lostAt);
    f("minFullsteps", r.minFullsteps);
    f("maxFullsteps", r.maxFullsteps);
    f("travelDeg", r.travelDeg);
    f("positionFullsteps", r.positionFullsteps);
    f("xactualUsteps", r.xactualUsteps);
    f("settling", r.settling);
}

// A missing key keeps the field's default (false, "", 0).
static void jsonGet(const Json::Value& root, const char* k, bool& v) { v = root.get(k, v).asBool(); }
static void jsonGet(const Json::Value& root, const char* k, std::string& v) { v = root.get(k, v).asString(); }
static void jsonGet(const Json::Value& root, const char* k, int32_t& v) { v = root.get(k, v).asInt(); }
static void jsonGet(const Json::Value& root, const char* k, double& v) { v = root.get(k, v).asDouble(); }

bool TmcAxis::loadReference() {
    Json::Value root;
    if (!LoadJsonFromFile(statePath(), root))
        return false;
    // A newer plugin's file (see REFERENCE_FORMAT): costs a rehome at most.
    if (root.get("format", REFERENCE_FORMAT).asInt() > REFERENCE_FORMAT) {
        LogWarn(VB_PLUGIN, "%s: %s: the stored position was written by a newer version "
                "of the plugin; it needs homing again\n", LOGPFX, m_cfg.name.c_str());
        m_ref = ReferenceState();
        m_ref.lostWhy = "its stored position came from a newer version of the plugin";
        m_ref.lostAt = timestampNow();
        saveReference();
        return false;
    }
    referenceFields(m_ref, [&](const char* k, auto& v) { jsonGet(root, k, v); });
    const std::string wiring = root.get("wiring", "").asString();
    if (!wiring.empty() && wiring != wiringOf(m_cfg)) {
        LogWarn(VB_PLUGIN, "%s: %s: the stored position was measured on %s, but this "
                "stepper is now %s; it needs homing again\n", LOGPFX, m_cfg.name.c_str(),
                wiring.c_str(), wiringOf(m_cfg).c_str());
        m_ref = ReferenceState();
        m_ref.lostWhy = "its driver, bus or chip select changed since it was homed";
        m_ref.lostAt = timestampNow();
        saveReference();
        return false;
    }
    return true;
}

bool TmcAxis::saveReference() {
    Json::Value root;
    root["format"] = REFERENCE_FORMAT;
    referenceFields(m_ref, [&](const char* k, const auto& v) { root[k] = v; });
    root["wiring"] = wiringOf(m_cfg);

    // Atomic: a torn state file would look like a lost reference.
    std::string path = statePath();
    std::string tmp = path + ".tmp";
    if (!SaveJsonToFile(root, tmp)) {
        LogWarn(VB_PLUGIN, "%s: %s: could not write %s\n", LOGPFX,
                m_cfg.name.c_str(), tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        LogWarn(VB_PLUGIN, "%s: %s: could not rename %s -> %s\n", LOGPFX,
                m_cfg.name.c_str(), tmp.c_str(), path.c_str());
        return false;
    }
    // fppd runs as root; FPP's backup and file tools run as fpp.
    SetFilePerms(path);
    return true;
}

} // namespace tmc
