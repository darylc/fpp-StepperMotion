#include "fpp-pch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "AxisConfig.h"

#include "common.h"
#include "fpp-json.h"

namespace tmc {

// ---------------------------------------------------------------- spi --

const std::vector<SpiBusInfo>& spiBuses() {
    // The fixed wiring of every 40-pin Pi: SPI0 on GPIO 9/10/11, SPI1 on
    // GPIO 19/20/21. Built once and never changed, so the axis workers and
    // the HTTP handlers can all hold pointers into it.
    static const std::vector<SpiBusInfo> table = {
        { 0, 0, "P1-21", "P1-19", "P1-23" },
        { 1, 1, "P1-35", "P1-38", "P1-40" },
    };
    return table;
}

bool spiBusAvailable(const SpiBusInfo& b) {
    // The spidev nodes come and go with overlays, so this is looked up
    // every time rather than cached.
    return FileExists("/dev/spidev" + std::to_string(b.spidev) + ".0");
}

const SpiBusInfo* spiBus(int bus) {
    for (const SpiBusInfo& b : spiBuses()) {
        if (b.bus == bus)
            return &b;
    }
    return nullptr;
}

std::string stepperFileStem(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_')
            out += c;
        else
            out += '_';
    }
    return out;
}

bool isChipSelectPin(const std::string& name) {
    // The GPIO pins of the 40-pin header, by header number. Left out: power
    // and ground, P1-3/5 (I2C1, which FPP's own OLED and cape detection
    // use) and P1-27/28 (the HAT ID EEPROM bus).
    static const int gpioPins[] = { 7, 8, 10, 11, 12, 13, 15, 16, 18, 19, 21, 22, 23, 24,
                                    26, 29, 31, 32, 33, 35, 36, 37, 38, 40 };
    if (name.compare(0, 3, "P1-") != 0 || name.size() < 4 ||
        name.find_first_not_of("0123456789", 3) != std::string::npos)
        return false;
    const int n = atoi(name.c_str() + 3);
    if (std::find(std::begin(gpioPins), std::end(gpioPins), n) == std::end(gpioPins))
        return false;
    for (const SpiBusInfo& b : spiBuses()) {
        if (name == b.miso || name == b.mosi || name == b.sclk)
            return false;
    }
    return true;
}

std::string SpiConfig::describe() const {
    if (fake)
        return "simulator";
    const SpiBusInfo* b = spiBus(bus);
    return "SPI" + std::to_string(bus) + " CS " + cs + " (/dev/spidev" +
           std::to_string(b ? b->spidev : bus) + ".0)";
}

std::string Preset::describe() const {
    char buf[160];
    // The scope belongs in every log line a preset appears in: "goto 50%" on a
    // two-stepper rig is ambiguous, "goto 50% [wing]" is not.
    auto scoped = [this](const std::string& s) {
        return stepperScope.empty() ? s : s + " [" + stepperScope + "]";
    };
    switch (type) {
    case Type::Rehome:
        return scoped("rehome");
    case Type::Release:
        return scoped("release");
    case Type::Goto:
        if (usePercent)
            snprintf(buf, sizeof(buf), "goto %.0f%%", positionPercent);
        else
            snprintf(buf, sizeof(buf), "goto %.2f fullsteps", positionFullsteps);
        return scoped(buf);
    case Type::Cycle:
        snprintf(buf, sizeof(buf), "cycle, %d waypoints, %s",
                 (int)waypoints.size(),
                 repeat == 0 ? "while held" : "fixed repeats");
        return scoped(buf);
    }
    return "unknown";
}

const char* Preset::typeName(Type t) {
    switch (t) {
    case Type::Rehome: return "rehome";
    case Type::Goto: return "goto";
    case Type::Cycle: return "cycle";
    case Type::Release: return "release";
    }
    return "goto";
}

// --------------------------------------------------------------- utils --

// A member of an object, or a null value when the parent is not an object or
// has no such member. Everything reads through this so a config.json (or a
// request body) with a number where a block should be cannot make jsoncpp
// assert inside fppd.
static const Json::Value& sub(const Json::Value& v, const char* key) {
    static const Json::Value none;
    if (!v.isObject() || !v.isMember(key))
        return none;
    return v[key];
}

static double getDouble(const Json::Value& v, const char* key, double dflt) {
    const Json::Value& x = sub(v, key);
    return x.isNumeric() ? x.asDouble() : dflt;
}

static int getInt(const Json::Value& v, const char* key, int dflt) {
    const Json::Value& x = sub(v, key);
    return x.isNumeric() ? x.asInt() : dflt;
}

static bool getBool(const Json::Value& v, const char* key, bool dflt) {
    const Json::Value& x = sub(v, key);
    if (x.isBool())
        return x.asBool();
    if (x.isNumeric())
        return x.asInt() != 0;
    return dflt;
}

static std::string getString(const Json::Value& v, const char* key,
                             const std::string& dflt) {
    const Json::Value& x = sub(v, key);
    return x.isString() ? x.asString() : dflt;
}

// CHOPCONF is the one field a user is likely to paste as "0x000100C3".
static uint32_t getChopconf(const Json::Value& v, const char* key, uint32_t dflt) {
    const Json::Value& x = sub(v, key);
    if (x.isNumeric())
        return (uint32_t)x.asUInt64();
    if (x.isString()) {
        const std::string s = x.asString();
        if (!s.empty()) {
            char* end = nullptr;
            unsigned long parsed = strtoul(s.c_str(), &end, 0);
            if (end && *end == '\0')
                return (uint32_t)parsed;
        }
    }
    return dflt;
}

// A number for the file: whole values as integers (12000000, not 12000000.0),
// which is what the setup page wrote and what a person expects to read.
static Json::Value numJson(double v) {
    if (std::fabs(v) < 9007199254740992.0 && v == std::floor(v))
        return Json::Value((Json::Int64)v);
    return Json::Value(v);
}

// Appends a message unless the same one is already there for this block.
// Two fields can share one sentence ("rehome speeds must be > 0"), and it
// must read once however many of them are wrong.
static void say(std::vector<std::string>& errors, size_t since, const std::string& msg) {
    for (size_t i = since; i < errors.size(); i++) {
        if (errors[i] == msg)
            return;
    }
    errors.push_back(msg);
}

// std::to_string() on a double gives "0.020000". These bounds are quoted back
// to the user in an error they have to act on, so they get formatted the way
// the setup page shows them.
static bool checkRampTime(double v, const char* what, const std::string& axis,
                          std::vector<std::string>& errors) {
    if (v >= MIN_RAMP_TIME_SEC && v <= MAX_RAMP_TIME_SEC)
        return true;
    char buf[200];
    snprintf(buf, sizeof(buf),
             "%s: %s must be %.2f..%.2f seconds, got %g", axis.c_str(), what,
             MIN_RAMP_TIME_SEC, MAX_RAMP_TIME_SEC, v);
    errors.push_back(buf);
    return false;
}

static bool checkCurrent(int v, const char* what, const std::string& axis,
                         std::vector<std::string>& errors) {
    if (v < 0 || v > 31) {
        errors.push_back(axis + ": " + what + " must be 0..31, got " +
                         std::to_string(v));
        return false;
    }
    return true;
}

// ---------------------------------------------------------- field table --
//
// Every numeric and boolean field of a stepper: its JSON path, its scope,
// where it lives in AxisConfig and what it accepts. parseDefaults() and
// parseAxis() read through it, the validator walks it, toJson() writes it
// back. The default is whatever the struct member was initialised to, so it
// is not repeated here. The four enum-ish fields (name, driver, channel
// source, rehome.dir) and the nullable holdSeconds are handled by hand around
// the table.
//
// Scope: Axis fields live only in a stepper's block. Inherit fields live in
// the defaults block and a stepper may set its own. Global fields live only
// in the defaults block; one in a stepper's block is refused.

struct Field {
    enum class Kind { Int, UInt, Dbl, Bool, Hex };
    enum class Scope { Axis, Inherit, Global };
    const char* group; // JSON object under the axis: "motor", "spi", ...
    const char* key;
    Kind kind;
    Scope scope;
    void* (*at)(AxisConfig&); // address of the member, typed by kind

    // Accepted range, inclusive, NaN = open. above: lo itself is refused.
    double lo = NAN, hi = NAN;
    bool above = false;
    // The message when out of range. nullptr = "<group.key> must be lo..hi",
    // with ", got <v>" appended when got is set.
    const char* msg = nullptr;
    bool got = false;

    Field& range(double l, double h) { lo = l; hi = h; return *this; }
    Field& min(double l) { lo = l; return *this; }
    Field& positive() { lo = 0; above = true; return *this; }
    Field& say(const char* m) { msg = m; return *this; }
    Field& withGot() { got = true; return *this; }

    std::string path() const { return std::string(group) + "." + key; }
};

#define FIELD(KIND, SCOPE, GROUP, KEY, MEMBER) \
    Field { GROUP, KEY, Field::Kind::KIND, Field::Scope::SCOPE, \
            [](AxisConfig& a) -> void* { return &a.MEMBER; } }

// spi.bus / spi.cs are not here: the chip select is a pin name, and the pair
// has rules of its own (readPort).
static const Field AXIS_FIELDS[] = {
    FIELD(UInt, Global, "spi", "speedHz", spi.speedHz),
    FIELD(Bool, Axis, "spi", "fake", spi.fake),

    FIELD(Int, Inherit, "motor", "irun", motor.irun).range(0, 31).withGot(),
    FIELD(Int, Inherit, "motor", "ihold", motor.ihold).range(0, 31).withGot(),
    FIELD(Int, Global, "motor", "iholdDelay", motor.iholdDelay),
    // fclk and rsense are derived from the effective driver when neither the
    // stepper nor the defaults block sets them: an explicit value is the
    // override for a board that is not a BOB.
    FIELD(Dbl, Inherit, "motor", "fclk", motor.fclkHz).range(1e6, 30e6)
        .say("fclk looks wrong (expected ~10-17 MHz)"),
    FIELD(Dbl, Inherit, "motor", "rsense", motor.rsense).positive().say("rsense must be > 0"),
    FIELD(Bool, Global, "motor", "vsense", motor.vsense),
    FIELD(Int, Global, "motor", "globalScaler", motor.globalScaler).range(32, 256)
        .say("motor.globalScaler must be 32..256 (the chip clips anything below 32)"),
    FIELD(Hex, Global, "motor", "chopconf", motor.chopconf),

    FIELD(Dbl, Inherit, "motion", "rampTimeSec", motion.rampTimeSec)
        .range(MIN_RAMP_TIME_SEC, MAX_RAMP_TIME_SEC),

    FIELD(Int, Axis, "limits", "minFullsteps", limits.minFullsteps),
    FIELD(Int, Axis, "limits", "maxFullsteps", limits.maxFullsteps),

    FIELD(Dbl, Global, "rehome", "coarseOvertravelDeg", rehome.coarseOvertravelDeg),
    FIELD(Dbl, Inherit, "rehome", "coarseRps", rehome.coarseRps).positive()
        .say("rehome speeds must be > 0"),
    FIELD(Int, Inherit, "rehome", "coarseIrun", rehome.coarseIrun).range(0, 31).withGot(),
    FIELD(Int, Global, "rehome", "coarseIhold", rehome.coarseIhold).range(0, 31).withGot(),
    FIELD(Int, Global, "rehome", "backoffSteps", rehome.backoffSteps).min(1)
        .say("rehome.backoffSteps must be >= 1"),
    FIELD(Dbl, Global, "rehome", "backoffRps", rehome.backoffRps).positive()
        .say("rehome speeds must be > 0"),
    FIELD(Int, Global, "rehome", "creepOvertravelSteps", rehome.creepOvertravelSteps),
    FIELD(Dbl, Inherit, "rehome", "creepRps", rehome.creepRps).positive()
        .say("rehome speeds must be > 0"),
    FIELD(Int, Inherit, "rehome", "creepIrun", rehome.creepIrun).range(0, 31).withGot(),
    FIELD(Int, Global, "rehome", "creepIhold", rehome.creepIhold).range(0, 31).withGot(),
    FIELD(Int, Global, "rehome", "settleMs", rehome.settleMs).range(0, MAX_SETTLE_MS).withGot(),
    // Read as a signed int: a negative in the file would otherwise wrap to an
    // enormous unsigned value and sail through the range check.
    FIELD(UInt, Global, "rehome", "amax", rehome.amax).range(MIN_REHOME_AMAX, MAX_REHOME_AMAX)
        .withGot(),
    FIELD(Bool, Global, "rehome", "autoRecoverOnDemand", rehome.autoRecoverOnDemand),
    FIELD(Bool, Global, "rehome", "autoRecoverAtStartup", rehome.autoRecoverAtStartup),
    FIELD(Int, Global, "rehome", "autoRecoverMaxAttempts", rehome.autoRecoverMaxAttempts)
        .range(MIN_AUTO_RECOVER_ATTEMPTS, MAX_AUTO_RECOVER_ATTEMPTS).withGot(),
};

#undef FIELD

// Reads one field from the axis's JSON into the struct; the member's current
// value is the default. Returns the value as a double for the range check.
static double readField(const Field& f, const Json::Value& a, AxisConfig& out) {
    const Json::Value& g = sub(a, f.group);
    void* p = f.at(out);
    switch (f.kind) {
    case Field::Kind::Int: {
        int& v = *(int*)p;
        v = getInt(g, f.key, v);
        return v;
    }
    case Field::Kind::UInt: {
        uint32_t& v = *(uint32_t*)p;
        const int i = getInt(g, f.key, (int)v);
        v = (uint32_t)i;
        return i;
    }
    case Field::Kind::Dbl: {
        double& v = *(double*)p;
        v = getDouble(g, f.key, v);
        return v;
    }
    case Field::Kind::Bool: {
        bool& v = *(bool*)p;
        v = getBool(g, f.key, v);
        return v;
    }
    case Field::Kind::Hex: {
        uint32_t& v = *(uint32_t*)p;
        v = getChopconf(g, f.key, v);
        return v;
    }
    }
    return 0;
}

static void checkField(const Field& f, double v, const std::string& label,
                       size_t since, std::vector<std::string>& errors) {
    const bool bad = (!std::isnan(f.lo) && (f.above ? v <= f.lo : v < f.lo)) ||
                     (!std::isnan(f.hi) && v > f.hi);
    if (!bad)
        return;
    const std::string what = f.path();
    if (f.msg) {
        say(errors, since, label + ": " + f.msg);
    } else if (f.kind == Field::Kind::Dbl) {
        // The one double with a generated message is the ramp time, whose
        // wording is shared with the per-preset override.
        checkRampTime(v, what.c_str(), label, errors);
    } else {
        std::string m = label + ": " + what + " must be " + std::to_string((int)f.lo) +
                        ".." + std::to_string((int)f.hi);
        if (f.got)
            m += ", got " + std::to_string((int)v);
        say(errors, since, m);
    }
}

static void writeField(const Field& f, const AxisConfig& in, Json::Value& a) {
    void* p = f.at(const_cast<AxisConfig&>(in));
    Json::Value& g = a[f.group];
    switch (f.kind) {
    case Field::Kind::Int: g[f.key] = *(int*)p; break;
    case Field::Kind::UInt: g[f.key] = (Json::UInt)*(uint32_t*)p; break;
    case Field::Kind::Dbl: g[f.key] = numJson(*(double*)p); break;
    case Field::Kind::Bool: g[f.key] = *(bool*)p; break;
    case Field::Kind::Hex: {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", *(uint32_t*)p);
        g[f.key] = buf;
        break;
    }
    }
}

// Whether the block spells this field out at all (a value of the wrong type
// still counts as "set"; readField falls back for it).
static bool hasField(const Field& f, const Json::Value& block) {
    return sub(block, f.group).isMember(f.key);
}

static void copyField(const Field& f, const AxisConfig& from, AxisConfig& to) {
    const void* src = f.at(const_cast<AxisConfig&>(from));
    void* dst = f.at(to);
    switch (f.kind) {
    case Field::Kind::Int: *(int*)dst = *(const int*)src; break;
    case Field::Kind::UInt:
    case Field::Kind::Hex: *(uint32_t*)dst = *(const uint32_t*)src; break;
    case Field::Kind::Dbl: *(double*)dst = *(const double*)src; break;
    case Field::Kind::Bool: *(bool*)dst = *(const bool*)src; break;
    }
}

// The two fields the table cannot describe, shared by the defaults block and
// a stepper's own. Each returns whether the block had the key at all.

static bool readDriver(const Json::Value& block, const std::string& label,
                       AxisConfig& out, std::vector<std::string>& errors) {
    if (!block.isObject() || !block.isMember("driver"))
        return false;
    const std::string driverName = getString(block, "driver", "");
    if (const TmcDriverVariant* d = driverByName(driverName))
        out.driver = d;
    else
        errors.push_back(label + ": unknown driver '" + driverName +
                         "' (expected TMC5130 or TMC5160)");
    return true;
}

// Nullable: null means hold indefinitely (-1). Absent is the caller's to
// resolve -- inherit for a stepper, DEFAULT_HOLD_SECONDS for the defaults
// block.
static bool readHoldSeconds(const Json::Value& block, const std::string& label,
                            AxisConfig& out, std::vector<std::string>& errors) {
    if (!block.isObject() || !block.isMember("holdSeconds"))
        return false;
    const Json::Value& h = block["holdSeconds"];
    if (h.isNull()) {
        out.holdSeconds = -1;
        return true;
    }
    const double v = h.isNumeric() ? h.asDouble() : -1;
    if (!h.isNumeric() || v < 0 || v > MAX_HOLD_SECONDS || v != std::floor(v)) {
        char got[48];
        if (h.isNumeric())
            snprintf(got, sizeof(got), "%g", v);
        else
            snprintf(got, sizeof(got), "'%s'", h.isString() ? h.asCString() : "?");
        errors.push_back(label + ": holdSeconds must be 0.." +
                         std::to_string(MAX_HOLD_SECONDS) +
                         " whole seconds, or empty to hold indefinitely, got " + got);
    } else {
        out.holdSeconds = (int)v;
    }
    return true;
}

// Without this margin the slow final approach stops short of the stop and
// zeroes on thin air, which looks like a successful home and is not one.
static void checkCreepMargin(const AxisConfig& c, const std::string& label,
                             std::vector<std::string>& errors) {
    if (c.rehome.creepOvertravelSteps <= c.rehome.backoffSteps)
        errors.push_back(label +
                         ": rehome.creepOvertravelSteps must exceed backoffSteps, "
                         "or the final creep never reaches the stop");
}

// -------------------------------------------------------------- preset --

static bool parsePreset(const Json::Value& p, const std::string& label,
                        Preset& out, std::vector<std::string>& errors) {
    const size_t since = errors.size();
    out.stepperScope = getString(p, "stepper", "");
    if (out.stepperScope == "all")
        out.stepperScope.clear();
    out.name = getString(p, "name", "");

    const std::string type = getString(p, "type", "");
    if (type == "rehome") {
        out.type = Preset::Type::Rehome;
    } else if (type == "goto") {
        out.type = Preset::Type::Goto;
        if (p.isMember("positionPercent")) {
            out.usePercent = true;
            out.positionPercent = getDouble(p, "positionPercent", 0.0);
            if (out.positionPercent < 0.0 || out.positionPercent > 100.0)
                errors.push_back(label + ": positionPercent must be 0..100");
        } else if (p.isMember("positionFullsteps")) {
            out.positionFullsteps = getDouble(p, "positionFullsteps", 0.0);
        } else {
            errors.push_back(label +
                             ": goto needs positionFullsteps or positionPercent");
        }
    } else if (type == "cycle") {
        out.type = Preset::Type::Cycle;
        out.repeat = getInt(p, "repeat", 0);
        if (out.repeat < 0)
            errors.push_back(label + ": repeat must be >= 0 (0 = while held)");
        const Json::Value& wps = sub(p, "waypoints");
        if (!wps.isArray() || wps.size() < 2) {
            errors.push_back(label + ": cycle needs at least 2 waypoints");
        } else {
            for (Json::ArrayIndex i = 0; i < wps.size(); i++) {
                Waypoint w;
                w.positionFullsteps = getDouble(wps[i], "position", 0.0);
                w.dwellMs = getInt(wps[i], "dwellMs", 0);
                if (w.dwellMs < 0)
                    say(errors, since, label + ": waypoint dwellMs must be >= 0");
                out.waypoints.push_back(w);
            }
        }
    } else if (type == "release") {
        out.type = Preset::Type::Release;
    } else {
        errors.push_back(label + ": unknown preset type '" + type +
                         "' (expected rehome, goto, cycle or release)");
    }

    out.rps = getDouble(p, "rps", -1.0);
    if (out.rps > MAX_RPS)
        errors.push_back(label + ": rps above 4 rev/s is not sensible here");
    out.irun = getInt(p, "irun", -1);
    out.ihold = getInt(p, "ihold", -1);
    if (out.irun >= 0)
        checkCurrent(out.irun, "irun", label, errors);
    if (out.ihold >= 0)
        checkCurrent(out.ihold, "ihold", label, errors);
    out.rampTimeSec = getDouble(p, "rampTimeSec", -1.0);
    if (out.rampTimeSec > 0)
        checkRampTime(out.rampTimeSec, "rampTimeSec", label, errors);
    return errors.size() == since;
}

Json::Value Preset::toJson() const {
    Json::Value v;
    v["type"] = typeName(type);
    if (!name.empty())
        v["name"] = name;
    if (!stepperScope.empty())
        v["stepper"] = stepperScope;
    if (type == Type::Goto) {
        if (usePercent)
            v["positionPercent"] = numJson(positionPercent);
        else
            v["positionFullsteps"] = numJson(positionFullsteps);
    } else if (type == Type::Cycle) {
        v["waypoints"] = Json::Value(Json::arrayValue);
        for (const auto& w : waypoints) {
            Json::Value wp;
            wp["position"] = numJson(w.positionFullsteps);
            wp["dwellMs"] = w.dwellMs;
            v["waypoints"].append(wp);
        }
        v["repeat"] = repeat;
    }
    // Overrides only when set: absent means "inherit the stepper default".
    if (rps >= 0)
        v["rps"] = numJson(rps);
    if (irun >= 0)
        v["irun"] = irun;
    if (ihold >= 0)
        v["ihold"] = ihold;
    if (rampTimeSec >= 0)
        v["rampTimeSec"] = numJson(rampTimeSec);
    return v;
}

std::string ChannelSource::describe() const {
    switch (mode) {
    case Mode::Slot:
        return "slot " + std::to_string(slot);
    case Mode::Absolute:
        return "channel " + std::to_string(number);
    case Mode::Model:
        return "model '" + model + "'" +
               (offset ? " + " + std::to_string(offset) : "");
    }
    return "channel ?";
}

const char* ChannelSource::modeName(Mode m) {
    switch (m) {
    case Mode::Slot:
        return "slot";
    case Mode::Absolute:
        return "absolute";
    case Mode::Model:
        return "model";
    }
    return "slot";
}

std::string ChannelBlock::describe() const {
    if (source == Source::Model)
        return "model '" + model + "'";
    return "channel " + std::to_string(start);
}

// ------------------------------------------------------------ defaults --

// Fills out from the file's "defaults" block (absent: nothing is own and every
// value is built in), appending every problem found with a "defaults: "
// prefix. Returns false if anything was wrong with it.
static bool parseDefaults(const Json::Value& d, AxisConfig& out,
                          std::vector<std::string>& errors) {
    const size_t since = errors.size();
    const std::string label = "defaults";
    if (readDriver(d, label, out, errors))
        out.ownFields.insert("driver");
    out.motor.fclkHz = out.driver->defaultFclkHz;
    out.motor.rsense = out.driver->defaultRsense;
    for (const Field& f : AXIS_FIELDS) {
        if (f.scope == Field::Scope::Axis || !hasField(f, d))
            continue;
        checkField(f, readField(f, d, out), label, since, errors);
        out.ownFields.insert(f.path());
    }
    if (!out.ownFields.count("rehome.creepOvertravelSteps"))
        out.rehome.creepOvertravelSteps = out.rehome.backoffSteps + 2;
    // The margin is checked here on the block's own effective values, so a
    // bad pair in the defaults reads as one message, not one per stepper.
    if (d.isObject())
        checkCreepMargin(out, label, errors);
    if (readHoldSeconds(d, label, out, errors))
        out.ownFields.insert("holdSeconds");
    return errors.size() == since;
}

// ---------------------------------------------------------------- axis --

// Where a real stepper is on the SPI header: its bus (SPI0 unless said
// otherwise) and chip select. The bus must be one FPP has, the pin a GPIO
// pin of the header that is not an SPI data line. Uniqueness across
// steppers is checked by the caller once every stepper is read.
static void readPort(const Json::Value& a, const std::string& label,
                     AxisConfig& out, std::vector<std::string>& errors) {
    const Json::Value& spi = sub(a, "spi");
    if (!sub(spi, "cs").isString()) {
        errors.push_back(label + ": a real stepper needs spi.cs, its chip-select pin (e.g. \"P1-24\")");
        return;
    }
    out.spi.cs = getString(spi, "cs", "");
    out.spi.bus = getInt(spi, "bus", 0);
    if (!spiBus(out.spi.bus)) {
        std::string known;
        for (const SpiBusInfo& b : spiBuses())
            known += (known.empty() ? "" : ", ") + std::to_string(b.bus);
        errors.push_back(label + ": spi.bus must be one of " + known + ", got " +
                         std::to_string(out.spi.bus));
    }
    if (!isChipSelectPin(out.spi.cs))
        errors.push_back(label + ": spi.cs must be a GPIO pin of the 40-pin header (P1-7 .. P1-40) "
                                 "that is not an SPI data line, got '" +
                         out.spi.cs + "'");
}

// Fills out from one stepper block, inheriting from defaults, appending every
// problem found. Returns false if anything was wrong with it. A range rule is
// only applied to a value the block sets itself: an inherited one was checked
// once, in parseDefaults(), and is reported there.
static bool parseAxis(const Json::Value& a, int index, const AxisConfig& defaults,
                      AxisConfig& out, std::vector<std::string>& errors) {
    const size_t since = errors.size();
    out.name = getString(a, "name", "axis" + std::to_string(index));
    const std::string label = "stepper '" + out.name + "'";

    if (a.isObject() && a.isMember("enabled")) {
        if (a["enabled"].isBool())
            out.enabled = a["enabled"].asBool();
        else
            errors.push_back(label + ": enabled must be true or false");
    }

    // Channel source: a slot in the plugin's channel block unless the
    // stepper names its own channel, by number or by pixel overlay model.
    // A stepper with no channel at all, or a slot of 0, is given the lowest
    // free slot once every stepper has been read (parsePluginConfig).
    const Json::Value& ch = sub(a, "channel");
    const std::string mode = getString(ch, "mode", "slot");
    if (mode == "slot") {
        out.channel.mode = ChannelSource::Mode::Slot;
        out.channel.slot = getInt(ch, "slot", 0);
        if (out.channel.slot < 0 || out.channel.slot > CHANNEL_BLOCK_SIZE)
            errors.push_back(label + ": channel slot must be 1.." +
                             std::to_string(CHANNEL_BLOCK_SIZE));
    } else if (mode == "absolute") {
        out.channel.mode = ChannelSource::Mode::Absolute;
        out.channel.number = getInt(ch, "number", 0);
        if (out.channel.number < 1 || out.channel.number > MAX_CHANNEL_NUMBER)
            errors.push_back(label + ": channel must be 1.." +
                             std::to_string(MAX_CHANNEL_NUMBER));
    } else if (mode == "model") {
        out.channel.mode = ChannelSource::Mode::Model;
        out.channel.model = getString(ch, "model", "");
        out.channel.offset = getInt(ch, "offset", 0);
        if (out.channel.model.empty())
            errors.push_back(label + ": channel mode 'model' needs a model name");
        if (out.channel.offset < 0)
            errors.push_back(label + ": channel offset into the model must be >= 0");
    } else {
        errors.push_back(label + ": unknown channel mode '" + mode +
                         "' (expected slot, absolute or model)");
    }

    if (readDriver(a, label, out, errors))
        out.ownFields.insert("driver");
    else
        out.driver = defaults.driver;

    // Defaults the table cannot know: chip-dependent ones, applied before the
    // inherited and own values so a config that only names the driver still
    // gets sane numbers for that part.
    out.motor.fclkHz = out.driver->defaultFclkHz;
    out.motor.rsense = out.driver->defaultRsense;

    for (const Field& f : AXIS_FIELDS) {
        if (f.scope == Field::Scope::Global && hasField(f, a))
            errors.push_back(label + ": " + f.path() +
                             " is a global setting; set it in the defaults block");
        if (f.scope == Field::Scope::Global ||
            (f.scope == Field::Scope::Inherit && !hasField(f, a))) {
            if (defaults.ownFields.count(f.path()))
                copyField(f, defaults, out);
            continue;
        }
        checkField(f, readField(f, a, out), label, since, errors);
        if (f.scope != Field::Scope::Axis)
            out.ownFields.insert(f.path());
    }

    // The creep default rides on backoffSteps, so it is only known now.
    if (!defaults.ownFields.count("rehome.creepOvertravelSteps"))
        out.rehome.creepOvertravelSteps = out.rehome.backoffSteps + 2;

    if (out.limits.minFullsteps > out.limits.maxFullsteps)
        errors.push_back(label + ": minFullsteps must be <= maxFullsteps");

    out.rehome.ccw = (getString(sub(a, "rehome"), "dir", "ccw") != "cw");
    if (readHoldSeconds(a, label, out, errors))
        out.ownFields.insert("holdSeconds");
    else
        out.holdSeconds = defaults.holdSeconds;
    // The simulator takes no port: whatever the block says about one is
    // read past and not written back.
    if (!out.spi.fake)
        readPort(a, label, out, errors);
    return errors.size() == since;
}

// The inheritable and global fields of a block, written out: all of them
// (everything) or only those in ownFields.
static void writeFields(const AxisConfig& c, Json::Value& v, bool everything) {
    if (everything || c.ownFields.count("driver"))
        v["driver"] = c.driver->name;
    for (const Field& f : AXIS_FIELDS) {
        if (f.scope != Field::Scope::Axis && (everything || c.ownFields.count(f.path())))
            writeField(f, c, v);
    }
    if (everything || c.ownFields.count("holdSeconds"))
        v["holdSeconds"] = c.holdSeconds >= 0 ? Json::Value(c.holdSeconds) : Json::Value();
}

static void writeAxisOnly(const AxisConfig& c, Json::Value& v) {
    v["name"] = c.name;
    if (!c.enabled)
        v["enabled"] = false;
    switch (c.channel.mode) {
    case ChannelSource::Mode::Slot:
        v["channel"]["mode"] = "slot";
        v["channel"]["slot"] = c.channel.slot;
        break;
    case ChannelSource::Mode::Absolute:
        v["channel"]["mode"] = "absolute";
        v["channel"]["number"] = c.channel.number;
        break;
    case ChannelSource::Mode::Model:
        v["channel"]["mode"] = "model";
        v["channel"]["model"] = c.channel.model;
        v["channel"]["offset"] = c.channel.offset;
        break;
    }
    for (const Field& f : AXIS_FIELDS) {
        if (f.scope == Field::Scope::Axis)
            writeField(f, c, v);
    }
    if (!c.spi.fake) {
        v["spi"]["bus"] = c.spi.bus;
        v["spi"]["cs"] = c.spi.cs;
    }
    v["rehome"]["dir"] = c.rehome.ccw ? "ccw" : "cw";
}

Json::Value AxisConfig::toJson() const {
    Json::Value v;
    writeAxisOnly(*this, v);
    writeFields(*this, v, false);
    return v;
}

Json::Value AxisConfig::effectiveJson() const {
    Json::Value v;
    writeAxisOnly(*this, v);
    writeFields(*this, v, true);
    v["enabled"] = enabled;
    // The device node behind the bus, so the page can name it without
    // knowing FPP's bus table; nothing for a simulated stepper.
    if (!spi.fake) {
        const SpiBusInfo* b = spiBus(spi.bus);
        v["spi"]["path"] = "/dev/spidev" + std::to_string(b ? b->spidev : spi.bus) + ".0";
    }
    return v;
}

// A channel-value -> preset object. A bad preset is DROPPED, with an error,
// rather than failing the parse: presets are show content, and refusing the
// whole configuration over one mistyped percentage would take every stepper
// offline. Stepper definitions stay all-or-nothing (parseAxis), since a
// half-applied chip select or driver type is a hazard.
static void parsePresetsMap(const Json::Value& presets, const std::string& ownerLabel,
                            std::map<int, Preset>& out,
                            std::vector<std::string>& errors) {
    if (!presets.isObject())
        return;
    for (const auto& key : presets.getMemberNames()) {
        int value = atoi(key.c_str());
        if (value < 1 || value > 255) {
            errors.push_back(ownerLabel + ": preset key '" + key +
                             "' must be 1..255 (0 is reserved for idle). Ignoring it.");
            continue;
        }
        Preset p;
        const std::string label = ownerLabel + " preset " + std::to_string(value);
        if (!presets[key].isObject()) {
            errors.push_back(label + " is not an object. Ignoring it.");
            continue;
        }
        if (!parsePreset(presets[key], label, p, errors)) {
            errors.push_back(label + " is ignored until that is fixed; the other presets "
                                     "and every stepper still run.");
            continue;
        }
        out[value] = p;
    }
}

void copyTunables(const AxisConfig& from, AxisConfig& to) {
    to.motor = from.motor;
    to.motion = from.motion;
    to.rehome = from.rehome;
    to.limits = from.limits;
    to.presets = from.presets;
    to.holdSeconds = from.holdSeconds;
}

// -------------------------------------------------------------- plugin --

// The channel block and every stepper's place in the show, once all the
// steppers have been read: slots are handed out and checked across the lot.
// A file with no block gets the default one, starting at channel 1.
static bool parseChannels(const Json::Value& root, PluginConfig& parsed,
                          std::vector<std::string>& errors) {
    bool ok = true;
    ChannelBlock& blk = parsed.channelBlock;
    if (root.isObject() && root.isMember("channelBlock")) {
        const Json::Value& b = sub(root, "channelBlock");
        const std::string source = getString(b, "source", "channel");
        if (source == "channel") {
            blk.source = ChannelBlock::Source::Channel;
            blk.start = getInt(b, "start", 1);
            const int last = MAX_CHANNEL_NUMBER - CHANNEL_BLOCK_SIZE + 1;
            if (blk.start < 1 || blk.start > last) {
                errors.push_back("channel block start must be 1.." + std::to_string(last));
                ok = false;
            }
        } else if (source == "model") {
            blk.source = ChannelBlock::Source::Model;
            blk.model = getString(b, "model", "");
            if (blk.model.empty()) {
                errors.push_back("channel block source 'model' needs a model name");
                ok = false;
            }
        } else {
            errors.push_back("unknown channel block source '" + source +
                             "' (expected channel or model)");
            ok = false;
        }
    }

    // Slots named twice, then the lowest free slot for each stepper that
    // named none.
    std::set<int> taken;
    for (size_t i = 0; i < parsed.axes.size(); i++) {
        const ChannelSource& c = parsed.axes[i].channel;
        if (c.mode != ChannelSource::Mode::Slot || c.slot < 1 || c.slot > CHANNEL_BLOCK_SIZE)
            continue;
        if (!taken.insert(c.slot).second) {
            for (size_t k = 0; k < i; k++) {
                if (parsed.axes[k].channel.mode == ChannelSource::Mode::Slot &&
                    parsed.axes[k].channel.slot == c.slot) {
                    errors.push_back("steppers '" + parsed.axes[k].name + "' and '" +
                                     parsed.axes[i].name + "' both use channel slot " +
                                     std::to_string(c.slot));
                    break;
                }
            }
            ok = false;
        }
    }
    for (auto& ac : parsed.axes) {
        if (ac.channel.mode != ChannelSource::Mode::Slot || ac.channel.slot != 0)
            continue;
        for (int s = 1; s <= CHANNEL_BLOCK_SIZE; s++) {
            if (taken.insert(s).second) {
                ac.channel.slot = s;
                break;
            }
        }
    }

    // Two steppers reading the same byte would move together on every
    // value, which is never what was meant -- whichever way each names it.
    // Where a channel comes from a model it is compared as model + offset:
    // the number is not known until fppd resolves the model.
    auto where = [&blk](const ChannelSource& c) -> std::string {
        switch (c.mode) {
        case ChannelSource::Mode::Slot:
            if (c.slot < 1)
                return "";
            if (blk.source == ChannelBlock::Source::Model)
                return "model '" + blk.model + "' + " + std::to_string(c.slot - 1);
            return "channel " + std::to_string(blk.start + c.slot - 1);
        case ChannelSource::Mode::Absolute:
            return c.number >= 1 ? "channel " + std::to_string(c.number) : "";
        case ChannelSource::Mode::Model:
            return c.model.empty() ? "" : "model '" + c.model + "' + " + std::to_string(c.offset);
        }
        return "";
    };
    for (size_t i = 0; i < parsed.axes.size(); i++) {
        const std::string wi = where(parsed.axes[i].channel);
        if (wi.empty())
            continue;
        for (size_t k = 0; k < i; k++) {
            const bool bothSlots = parsed.axes[i].channel.mode == ChannelSource::Mode::Slot &&
                                   parsed.axes[k].channel.mode == ChannelSource::Mode::Slot;
            if (!bothSlots && where(parsed.axes[k].channel) == wi) {
                errors.push_back("steppers '" + parsed.axes[k].name + "' and '" +
                                 parsed.axes[i].name + "' both read " + wi);
                ok = false;
            }
        }
    }
    return ok;
}

bool parsePluginConfig(const Json::Value& root, PluginConfig& cfg,
                       std::vector<std::string>& errors) {
    bool fatal = false;
    PluginConfig parsed;
    const int format = getInt(root, "format", CONFIG_FORMAT);
    if (format > CONFIG_FORMAT) {
        errors.push_back("this configuration was written by a newer version of Stepper "
                         "Motion (format " + std::to_string(format) + "); update the plugin");
        return false;
    }
    parsed.idleTimeoutMs = getInt(root, "idleTimeoutMs", 500);
    if (parsed.idleTimeoutMs < 100 || parsed.idleTimeoutMs > 60000) {
        errors.push_back("idleTimeoutMs must be 100..60000");
        fatal = true;
    }
    // A bad defaults block is as fatal as a bad stepper: every stepper reads
    // through it.
    if (!parseDefaults(sub(root, "defaults"), parsed.defaults, errors))
        fatal = true;

    const Json::Value& axes = sub(root, "axes");
    if (!axes.isArray()) {
        // No axes configured is a valid, quiet state -- a freshly installed
        // plugin nobody has commissioned yet. The channel block may already
        // be set.
        if (!parseChannels(root, parsed, errors))
            fatal = true;
        if (fatal)
            return false;
        cfg = parsed;
        return true;
    }
    if ((int)axes.size() > MAX_AXES) {
        errors.push_back("at most " + std::to_string(MAX_AXES) +
                         " steppers are supported");
        fatal = true;
    }

    for (Json::ArrayIndex i = 0; i < axes.size(); i++) {
        AxisConfig ac;
        if (!axes[i].isObject()) {
            errors.push_back("stepper " + std::to_string(i + 1) + " is not an object");
            fatal = true;
            continue;
        }
        if (!parseAxis(axes[i], (int)i, parsed.defaults, ac, errors))
            fatal = true;
        if (ac.name.find_first_not_of(" \t") == std::string::npos) {
            errors.push_back("stepper " + std::to_string(i + 1) + " has no name");
            fatal = true;
        }
        for (const auto& existing : parsed.axes) {
            // The name is the stepper's identity: commands, scoped presets
            // and its stored position (axis-<name>.json) all find it by name.
            if (existing.name == ac.name) {
                errors.push_back("two steppers are named '" + ac.name + "'; every stepper "
                                 "needs its own name");
                fatal = true;
            } else if (stepperFileStem(existing.name) == stepperFileStem(ac.name)) {
                errors.push_back("steppers '" + existing.name + "' and '" + ac.name +
                                 "' differ only in punctuation or spaces, so they would "
                                 "share one stored position; rename one");
                fatal = true;
            }
            // One chip select per chip: two steppers on the same pin would
            // both answer every datagram. A pin that failed its own check is
            // not compared; it has been reported already.
            if (!existing.spi.fake && !ac.spi.fake && isChipSelectPin(ac.spi.cs) &&
                existing.spi.cs == ac.spi.cs) {
                errors.push_back("steppers '" + existing.name + "' and '" + ac.name +
                                 "' both use chip select " + ac.spi.cs);
                fatal = true;
            }
        }
        parsed.axes.push_back(ac);
    }

    if (!parseChannels(root, parsed, errors))
        fatal = true;

    if (root.isObject() && root.isMember("presets"))
        parsePresetsMap(root["presets"], "preset table", parsed.presets, errors);
    // Presets live in the one shared table (scoped to a stepper by name when
    // they should not apply to all). A presets block under a stepper is
    // reported and ignored. Non-fatal: the steppers still run.
    for (Json::ArrayIndex i = 0; i < axes.size(); i++) {
        if (axes[i].isObject() && axes[i].isMember("presets"))
            errors.push_back("stepper '" + getString(axes[i], "name", "axis" + std::to_string(i)) +
                             "' has a presets block of its own; presets belong in the "
                             "shared table and these are ignored");
    }
    // Hand each axis only the presets that apply to it: AxisWorker finds
    // nothing for another stepper's preset, the "no preset mapped" case it
    // already handles.
    for (auto& ac : parsed.axes) {
        ac.presets.clear();
        for (const auto& kv : parsed.presets) {
            if (kv.second.stepperScope.empty() || kv.second.stepperScope == ac.name)
                ac.presets[kv.first] = kv.second;
        }
    }

    // A scope naming no configured axis means the preset can never run.
    for (const auto& kv : parsed.presets) {
        if (kv.second.stepperScope.empty())
            continue;
        bool found = false;
        for (const auto& ac : parsed.axes) {
            if (ac.name == kv.second.stepperScope) {
                found = true;
                break;
            }
        }
        if (!found) {
            errors.push_back("preset " + std::to_string(kv.first) +
                             " is scoped to stepper '" + kv.second.stepperScope +
                             "', which is not configured");
        }
    }

    if (fatal)
        return false;
    cfg = parsed;
    return true;
}

Json::Value PluginConfig::toJson() const {
    Json::Value v;
    v["format"] = CONFIG_FORMAT;
    v["idleTimeoutMs"] = idleTimeoutMs;
    Json::Value& blk = v["channelBlock"];
    if (channelBlock.source == ChannelBlock::Source::Model) {
        blk["source"] = "model";
        blk["model"] = channelBlock.model;
    } else {
        blk["source"] = "channel";
        blk["start"] = channelBlock.start;
    }
    // Only when the file had one, so load -> toJson stays a fixed point.
    if (!defaults.ownFields.empty()) {
        Json::Value d(Json::objectValue);
        writeFields(defaults, d, false);
        v["defaults"] = d;
    }
    v["axes"] = Json::Value(Json::arrayValue);
    for (const auto& ac : axes)
        v["axes"].append(ac.toJson());
    v["presets"] = Json::Value(Json::objectValue);
    for (const auto& kv : presets)
        v["presets"][std::to_string(kv.first)] = kv.second.toJson();
    return v;
}

Json::Value PluginConfig::defaultsEffectiveJson() const {
    Json::Value v(Json::objectValue);
    writeFields(defaults, v, true);
    return v;
}

} // namespace tmc
