#pragma once
/*
 * Configuration model for the plugin: config.json parsed, validated and
 * written back.
 *
 * Parsing never throws and never half-applies: a bad config yields the FULL
 * list of human-readable errors (every field of every stepper, so a save can
 * fix them in one round trip) and no axes, so fppd starts with the motors
 * quiet rather than moving on values nobody validated. toJson() emits exactly
 * the file shape, so load -> toJson -> parse is a fixed point.
 *
 * Defaults live in the struct member initialisers below and nowhere else; the
 * field table in AxisConfig.cpp names each field's JSON path, scope and
 * bounds once and drives parse, validate and emit from that one row.
 *
 * Most settings are the same for every stepper on a box, so the file has a
 * top-level "defaults" block and a stepper's own block only carries what
 * differs. Resolution happens at parse time: a field's effective value is
 * the stepper's own, else the defaults block's, else the built-in default.
 * Everything downstream sees only effective values; which of them the file
 * spelled out per stepper is remembered in ownFields so toJson() writes the
 * file back in the shape it was read in.
 */

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fpp-json-fwd.h"

#include "TmcRegs.h"

namespace tmc {

// Eight axes: four per bus is comfortable on flying leads. Not a hardware
// limit under a GPIO chip select (any free header pin will do).
// plugin_setup.php reads this constant from the header for its pane count
// and hands it to the page as TMC.MAX_AXES.
constexpr int MAX_AXES = 8;
// The configuration file's layout. Written as "format"; a file with a higher
// one came from a newer plugin and is refused rather than half-read (its
// unknown fields would be dropped on the next save). Absent means 1.
constexpr int CONFIG_FORMAT = 1;
constexpr int SPI_BUSES = 2;

// One SPI bus a stepper may be on: the fixed SPI0/SPI1 wiring of every
// 40-pin Pi.
struct SpiBusInfo {
    int bus;                        // the logical number config.json names
    int spidev;                     // /dev/spidev<spidev>.0
    std::string miso, mosi, sclk;   // FPP header pin names, e.g. "P1-21"
};
const std::vector<SpiBusInfo>& spiBuses();
// nullptr for a bus the table does not have.
const SpiBusInfo* spiBus(int bus);
// Whether /dev/spidev<spidev>.0 exists on this box right now.
bool spiBusAvailable(const SpiBusInfo& b);

// Whether name is a header pin a chip select may be driven from: a GPIO pin
// of the 40-pin header ("P1-7" .. "P1-40", not power, ground or the HAT ID
// EEPROM pair P1-27/28) that is not a data pin of any bus.
bool isChipSelectPin(const std::string& name);

// A stepper name as it appears in its reference file name: letters, digits,
// '-' and '_' kept, anything else '_'. Two names with the same stem would
// share one file, so the parser refuses that.
std::string stepperFileStem(const std::string& name);

// Speed of a move that nothing specified one for: the commissioning jog, Park,
// returning home, a preset with no rps. The UI's Speed field defaults to the
// same figure (TMC.DEFAULT_RPS in assets/core.js).
constexpr double DEFAULT_MANUAL_RPS = 0.10;
// The fastest any move may ask for: presets, and manual jobs from the page,
// the API and FPP commands alike.
constexpr double MAX_RPS = 4.0;
// The largest distance or position a manual job may name, in full steps:
// 5000 revolutions, far inside the chip's 32-bit microstep position.
constexpr double MAX_MANUAL_FULLSTEPS = 1000000.0;

// FPP's own ceiling (FPPD_MAX_CHANNELS), to reject a typo'd channel number
// at save time.
constexpr int MAX_CHANNEL_NUMBER = 8388608;

// Bounds on a ramp time, shared by the per-stepper setting and the per-preset
// override. The floor is one iteration of runTo()'s 20ms poll: a shorter
// ramp buys no speed and only loses torque. The ceiling catches 150 typed
// for 1.5.
constexpr double MIN_RAMP_TIME_SEC = 0.02;
constexpr double MAX_RAMP_TIME_SEC = 5.0;

// Bounds on the homing acceleration, as a raw AMAX register value: gentler
// or somewhat harsher than the default without letting a mistyped number
// turn the approach to a hard stop into a slam (see RehomeConfig::amax).
constexpr uint32_t MIN_REHOME_AMAX = 10;
constexpr uint32_t MAX_REHOME_AMAX = 2000;
constexpr int MAX_SETTLE_MS = 5000;

// Longest finite hold (AxisConfig::holdSeconds): a day; an empty field
// means hold indefinitely. The default is long enough for a show cue to
// follow the last move, short enough that IHOLD is not left cooking a bare
// driver board through a parked stretch.
constexpr int MAX_HOLD_SECONDS = 86400;
constexpr int DEFAULT_HOLD_SECONDS = 30;

// Bounds on RehomeConfig::autoRecoverMaxAttempts; the ceiling only catches
// a typo.
constexpr int MIN_AUTO_RECOVER_ATTEMPTS = 1;
constexpr int MAX_AUTO_RECOVER_ATTEMPTS = 10;

struct Waypoint {
    double positionFullsteps = 0;
    int dwellMs = 0;
};

struct Preset {
    enum class Type {
        Rehome,
        Goto,
        Cycle,
        Release,
    };

    // Which stepper may run this preset: empty or "all" means every one,
    // otherwise the name of the single axis it belongs to (a cycle's
    // waypoints are absolute steps on one mechanism). Enforced at parse
    // time: an axis is only given the presets that apply to it.
    std::string stepperScope;

    // Free-text label from the presets dialog; carried through untouched.
    std::string name;

    Type type = Type::Goto;

    // Goto. positionPercent is resolved against the axis's soft limits at
    // run time, so re-commissioning the travel range does not invalidate
    // the preset.
    bool usePercent = false;
    double positionPercent = 0.0;
    double positionFullsteps = 0;

    // Cycle. repeat == 0 means "keep repeating while the channel holds this
    // value".
    std::vector<Waypoint> waypoints;
    int repeat = 0;

    // Motion parameters; -1 means "inherit the stepper default".
    double rps = -1.0;
    int irun = -1;
    int ihold = -1;

    // Seconds this preset's moves get to reach their speed; same -1
    // sentinel. Per preset because a short hop wants a brief ramp and a
    // full-travel sweep a gentle one (MotionConfig::rampTimeSec).
    double rampTimeSec = -1.0;

    std::string describe() const;
    static const char* typeName(Type t);
    Json::Value toJson() const;
};

// The plugin's block of channels in the show: CHANNEL_BLOCK_SIZE of them,
// one per possible stepper whether or not it is configured, so adding a
// stepper never moves another and the show side is one fixed-size prop.
// It starts at a plain channel number or at the first channel of an FPP
// pixel overlay model. A model is resolved at fppd startup and never stored
// resolved: a sequencer upload rewrites every model's start channel
// (and sets FPP's restart flag), so a cached number would point at
// somebody else's prop. FPP's channel output entry for the plugin mirrors
// the block (the setup page keeps it in step); the block itself lives here.
constexpr int CHANNEL_BLOCK_SIZE = MAX_AXES;

struct ChannelBlock {
    enum class Source {
        Channel,
        Model,
    };

    Source source = Source::Channel;
    int start = 1;         // Channel: 1-based first channel of the block
    std::string model;     // Model: pixel overlay model name

    std::string describe() const;
    bool operator==(const ChannelBlock& o) const {
        return source == o.source && start == o.start && model == o.model;
    }
};

// Where a stepper's one channel lives in the show.
//
// Slot is its place in the plugin's channel block (PluginConfig::
// channelBlock), what every stepper uses unless it has a reason not to.
// Absolute (a plain channel number) and Model (a pixel overlay model plus
// an offset, resolved like the block's) are the per-stepper override, for
// a stepper that has to sit somewhere else in the show.
struct ChannelSource {
    enum class Mode {
        Slot,
        Absolute,
        Model,
    };

    Mode mode = Mode::Slot;
    int slot = 0;          // Slot: 1..CHANNEL_BLOCK_SIZE
    int number = 0;        // Absolute: 1-based channel number
    std::string model;     // Model: pixel overlay model name
    int offset = 0;        // Model: channels into that model

    std::string describe() const;
    static const char* modeName(Mode m);
    bool operator==(const ChannelSource& o) const {
        return mode == o.mode && slot == o.slot && number == o.number &&
               model == o.model && offset == o.offset;
    }
};

struct SpiConfig {
    // Which bus and chip select this stepper is on. bus is the logical SPI
    // bus (0 = SPI0, 1 = SPI1); cs the FPP name of the header pin the
    // plugin drives as its chip select ("P1-24"). A real stepper must name
    // both; a simulated one has neither and cs stays empty.
    int bus = 0;
    std::string cs;
    uint32_t speedHz = 1000000;
    // Runs this axis against the built-in simulator instead of real silicon.
    bool fake = false;

    // "/dev/spidev0.0 CS P1-24", or "simulator".
    std::string describe() const;
};

struct MotorConfig {
    int irun = 16;
    int ihold = 8;
    int iholdDelay = (int)IHOLD_DELAY_DEFAULT;
    double fclkHz = 13.2e6;
    double rsense = 0.15;
    bool vsense = false;
    int globalScaler = 256;
    uint32_t chopconf = CHOPCONF_DEFAULT;
};

// How a normal (non-homing) move accelerates. Homing deliberately does not
// use this -- see RehomeConfig::amax.
struct MotionConfig {
    // How long the ramp is given to reach the requested speed; acceleration
    // is derived from this and the speed asked for, so a faster preset gets
    // a proportionally steeper ramp. Shortening it is the only lever that
    // speeds up a SHORT move (a few full steps is all ramp and never touches
    // VMAX). What it costs is torque margin: too short and the motor loses
    // steps, silently, and the position reference is wrong from then on.
    double rampTimeSec = 0.15;
};

struct LimitsConfig {
    int minFullsteps = 0;
    int maxFullsteps = 0;
};

struct RehomeConfig {
    // Which way the mechanical stop lies.
    bool ccw = true;

    double coarseOvertravelDeg = 10.0;
    double coarseRps = 0.15;
    int coarseIrun = 10;
    int coarseIhold = 4;

    int backoffSteps = 3;
    double backoffRps = 0.05;

    // Must exceed backoffSteps or the creep never reaches the stop.
    int creepOvertravelSteps = 5;
    double creepRps = 0.02;
    int creepIrun = 6;
    int creepIhold = 4;

    int settleMs = 500;

    // Acceleration for every homing move (coarse approach, backoff, creep),
    // as a raw AMAX register value. An acceleration rather than a ramp time
    // like MotionConfig::rampTimeSec: the three stages run at speeds an
    // order of magnitude apart (coarseRps 0.15 against creepRps 0.02), so a
    // shared ramp TIME would hand them accelerations just as far apart, when
    // what works against a hard stop is one gentle acceleration common to
    // all three. The riskiest number on the setup page: a homing ramp that
    // loses sync still ends somewhere, and that somewhere is latched as home
    // with no error -- an open-loop stepper cannot tell a stop it reached
    // from one it slipped short of.
    uint32_t amax = 200;

    // Automatic recovery of a lost position reference. Refusing every
    // commanded move until someone presses Rehome is always safe, but on a
    // chip whose supply is genuinely marginal it means the prop is off for
    // the rest of the show. Each knob defaults to the side with the smaller
    // blast radius: recovering in response to a move request is on,
    // recovering unattended at startup is off.

    // Rehome the moment something asks this axis to move and finds no valid
    // reference, instead of refusing that move -- including the axis
    // returning home, since "go home" is a real request whether the
    // sequence wrote it or the show just went quiet. Never attempted while
    // survey() reports the driver unhealthy (TmcAxis::attemptAutoRecover).
    bool autoRecoverOnDemand = true;

    // Also rehome once, unattended, right after the worker thread starts
    // with no valid reference -- for a prop that must be at a known
    // position before the show can use it. Off by default: startup is the
    // one case that moves the shaft with nobody watching or having asked.
    bool autoRecoverAtStartup = false;

    // Give up after this many consecutive failed on-demand attempts and
    // leave the reference lost rather than grinding into the stop on a chip
    // that keeps resetting. Reset to 0 the moment any reference is
    // established, manually or automatically. Manual Rehome always works;
    // only autoRecoverOnDemand is rate-limited by this.
    int autoRecoverMaxAttempts = 2;
};

struct AxisConfig {
    std::string name;
    // A disabled stepper stays in the file, keeps its chip select and channel
    // slot (so no other stepper's default wiring or slot moves, and enabling
    // it again needs no other edit) and is validated like any other, but
    // fppd starts no worker for it: no SPI traffic, its chip-select pin left
    // alone, its channel ignored. For a stepper whose board or motor is
    // unplugged for now. Written to the file only when false.
    bool enabled = true;
    ChannelSource channel;
    const TmcDriverVariant* driver = &DRIVER_TMC5130;

    SpiConfig spi;
    MotorConfig motor;
    MotionConfig motion;
    LimitsConfig limits;
    RehomeConfig rehome;

    // Seconds to keep IHOLD flowing after the last move, wherever the shaft
    // is (not only once it is back home), then release the coils: -1 (null
    // in the file) holds indefinitely, which a sprung or back-drivable
    // mechanism needs (a released shaft moves and the stored position goes
    // silently wrong); 0 releases as soon as the move is over; N > 0 holds
    // for N seconds. IHOLD flowing continuously is what cooks a bare driver
    // board on a show that parks a stepper away from home for long
    // stretches, which is why the default is a finite DEFAULT_HOLD_SECONDS
    // rather than forever (the page seeds a new configuration with the same
    // value). See AxisWorker::maybeReleaseIdleCoils().
    int holdSeconds = DEFAULT_HOLD_SECONDS;

    // Which inheritable fields this stepper's block in config.json spells
    // out itself, as JSON paths ("motor.irun", "holdSeconds"). The values
    // above are always effective; this records where they came from so
    // toJson() writes back exactly the fields the file had.
    std::set<std::string> ownFields;

    // The plugin-wide presets (PluginConfig::presets) that apply to this
    // axis, copied in once parsing finishes so AxisWorker never reaches past
    // its own config.
    std::map<int, Preset> presets;

    double rmsCurrentAmps(int irun) const {
        return tmc::rmsCurrentAmps(*driver, irun, motor.rsense, motor.vsense,
                                   motor.globalScaler);
    }

    // The axis as config.json holds it: its own fields only (no presets:
    // those are the plugin's).
    Json::Value toJson() const;
    // Every field with its effective value, in the same shape, for the setup
    // page's placeholders (?action=config axesEffective). holdSeconds is
    // null when it holds indefinitely.
    Json::Value effectiveJson() const;
};

struct PluginConfig {
    // How long after the last output frame an axis decides the show has
    // stopped driving it and returns home.
    int idleTimeoutMs = 500;
    // Where the plugin's channels start (see ChannelBlock). A file without
    // one is given one when it is read: see parsePluginConfig().
    ChannelBlock channelBlock;
    // The file's "defaults" block: what every stepper inherits unless its
    // own block says otherwise. Held as an AxisConfig so the one field table
    // reads, checks and writes it too; only inheritable fields mean anything
    // in it. Its ownFields say which the file actually had (none: toJson()
    // writes no block).
    AxisConfig defaults;
    std::vector<AxisConfig> axes;

    // Shared by every stepper: channel value -> preset.
    std::map<int, Preset> presets;

    // The whole document in the file shape.
    Json::Value toJson() const;
    // The defaults block with every inheritable and global field filled in,
    // built-in values included: what a stepper that sets nothing gets, for the
    // setup page (?action=config defaultsEffective). fclk and rsense are
    // those of the defaults' driver.
    Json::Value defaultsEffectiveJson() const;
};

// Copies the subset of an axis configuration that can be changed while the
// axis is running: currents, speeds, ramp, rehome staging, presets, limits
// and the hold. SPI bus and chip select, driver type and channel source only
// take effect when fppd rebuilds the channel output at startup and are
// reported through structuralChangePending() instead.
void copyTunables(const AxisConfig& from, AxisConfig& to);

// Parses a config.json document. Every problem found is appended to errors,
// not just the first; returns false (leaving cfg untouched) if any of them
// was fatal. Stepper errors are fatal, preset errors are not: a bad preset is
// dropped and reported, the steppers still run.
bool parsePluginConfig(const Json::Value& root, PluginConfig& cfg,
                       std::vector<std::string>& errors);

} // namespace tmc
