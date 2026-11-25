#pragma once
/*
 * Trinamic ramp-generator register map, and the small set of differences
 * between the driver chips this plugin supports.
 *
 * The motion registers below are identical on the TMC5130 and TMC5160 -- that
 * is the whole reason one code path can drive both. Everything that genuinely
 * differs is isolated in TmcDriverVariant at the bottom of this file, so
 * adding a third chip means adding one struct, not auditing the driver.
 */

#include <cstdint>
#include <string>

namespace tmc {

// ---- General / status -------------------------------------------------
constexpr uint8_t REG_GCONF = 0x00;
constexpr uint8_t REG_GSTAT = 0x01;
constexpr uint8_t REG_IOIN = 0x04;
constexpr uint8_t REG_IHOLD_IRUN = 0x10;
// TMC5160 only. 0 means full scale (256); 32..255 scale the motor current
// down; 1..31 are clipped to 32 by the chip. The register default is 0.
constexpr uint8_t REG_GLOBALSCALER = 0x0B;

// ---- Ramp generator ---------------------------------------------------
constexpr uint8_t REG_RAMPMODE = 0x20;
constexpr uint8_t REG_XACTUAL = 0x21;
constexpr uint8_t REG_VACTUAL = 0x22;
constexpr uint8_t REG_VSTART = 0x23;
constexpr uint8_t REG_A1 = 0x24;
constexpr uint8_t REG_V1 = 0x25;
constexpr uint8_t REG_AMAX = 0x26;
constexpr uint8_t REG_VMAX = 0x27;
constexpr uint8_t REG_DMAX = 0x28;
constexpr uint8_t REG_D1 = 0x2A;
constexpr uint8_t REG_VSTOP = 0x2B;
constexpr uint8_t REG_XTARGET = 0x2D;
constexpr uint8_t REG_SW_MODE = 0x34;
constexpr uint8_t REG_RAMP_STAT = 0x35;

// ---- Driver -----------------------------------------------------------
constexpr uint8_t REG_CHOPCONF = 0x6C;
constexpr uint8_t REG_DRV_STATUS = 0x6F;

// ---- RAMPMODE values --------------------------------------------------
constexpr uint32_t RAMPMODE_POSITIONING = 0;
constexpr uint32_t RAMPMODE_HOLD = 3;

// ---- Bit fields -------------------------------------------------------
constexpr uint32_t IOIN_DRV_ENN = 1u << 4;
constexpr uint32_t IOIN_SD_MODE = 1u << 6;

constexpr uint32_t GSTAT_RESET = 1u << 0;
constexpr uint32_t GSTAT_DRV_ERR = 1u << 1;
constexpr uint32_t GSTAT_UV_CP = 1u << 2;
constexpr uint32_t GSTAT_ALL_FLAGS = GSTAT_RESET | GSTAT_DRV_ERR | GSTAT_UV_CP;

// TMC5130 GCONF bits 0/1. On the TMC5160 these same two bits mean
// recalibrate/faststandstill, so gating on them there would be wrong -- see
// TmcDriverVariant::gconfIllegalMask.
constexpr uint32_t GCONF_I_SCALE_ANALOG = 1u << 0;
constexpr uint32_t GCONF_INTERNAL_RSENSE = 1u << 1;

constexpr uint32_t RAMP_STAT_POSITION_REACHED = 1u << 9;

constexpr uint32_t DRV_STATUS_OT = 1u << 25;   // overtemperature (shutdown)
constexpr uint32_t DRV_STATUS_OTPW = 1u << 26; // overtemperature pre-warning
constexpr uint32_t DRV_STATUS_S2GA = 1u << 27;
constexpr uint32_t DRV_STATUS_S2GB = 1u << 28;
// Open load on coil A/B. Only trustworthy during slow motion (the datasheet:
// false-triggers at standstill and at speed), so never a fault-mask bit;
// TmcAxis::runTo() evaluates it for slow moves across a run of samples.
constexpr uint32_t DRV_STATUS_OLA = 1u << 29;
constexpr uint32_t DRV_STATUS_OLB = 1u << 30;
constexpr uint32_t DRV_STATUS_STST = 1u << 31;

// TMC5160 adds short-to-supply detection the 5130 does not have.
constexpr uint32_t DRV_STATUS_S2VSA = 1u << 12;
constexpr uint32_t DRV_STATUS_S2VSB = 1u << 13;

// The SPI status byte returned as byte 0 of every datagram.
constexpr uint8_t SPI_STATUS_RESET = 1u << 0;
constexpr uint8_t SPI_STATUS_STANDSTILL = 1u << 3;

// ---- Motion geometry --------------------------------------------------
// MRES=0 in CHOPCONF means native 256 microsteps per full step.
constexpr int USTEPS_PER_FULLSTEP = 256;
constexpr int FULLSTEPS_PER_REV = 200;
constexpr int USTEPS_PER_REV = FULLSTEPS_PER_REV * USTEPS_PER_FULLSTEP;
constexpr double DEG_PER_FULLSTEP = 360.0 / FULLSTEPS_PER_REV; // 1.8

// TOFF=3, HSTRT=4, HEND=1, TBL=2, vsense=0, MRES=0.
constexpr uint32_t CHOPCONF_DEFAULT = 0x000100C3;
// Same value with TOFF cleared: the chopper off, coils de-energised.
constexpr uint32_t CHOPCONF_TOFF_MASK = 0xF;

// IHOLDDELAY: how long after standstill IRUN decays to IHOLD.
constexpr uint32_t IHOLD_DELAY_DEFAULT = 6;

inline uint32_t makeIholdIrun(int irun, int ihold, int iholdDelay) {
    return ((uint32_t)(iholdDelay & 0xF) << 16) |
           ((uint32_t)(irun & 0x1F) << 8) |
           (uint32_t)(ihold & 0x1F);
}

// ---- Driver chip variants ---------------------------------------------

struct TmcDriverVariant {
    const char* name;

    // Top byte of IOIN. This is the only positive proof the SPI link is up and
    // talking to the chip we think it is.
    uint8_t versionByte;

    // GCONF bits that must be clear before we move. On the TMC5130 these are
    // I_scale_analog and internal_Rsense: either one silently redefines what
    // IRUN means, so a current calculated from IRUN and R_sense would be a
    // fiction. The TMC5160 has unrelated bits there, so its mask is 0.
    uint32_t gconfIllegalMask;

    // DRV_STATUS bits that abort a move in progress.
    uint32_t faultMask;

    // Whether GLOBALSCALER (0x0B) exists and participates in current scaling.
    bool hasGlobalScaler;

    // Datasheet-typical internal oscillator. Only a default -- fCLK is per-axis
    // config because the real part varies (the 5130 spec spread is 10.1-17.2
    // MHz) and it scales every velocity we program.
    double defaultFclkHz;

    // Default sense resistor for a typical breakout of this chip.
    double defaultRsense;
};

constexpr TmcDriverVariant DRIVER_TMC5130 = {
    "TMC5130",
    0x11,
    GCONF_I_SCALE_ANALOG | GCONF_INTERNAL_RSENSE,
    DRV_STATUS_OT | DRV_STATUS_S2GA | DRV_STATUS_S2GB,
    false,
    13.2e6,
    0.15,
};

constexpr TmcDriverVariant DRIVER_TMC5160 = {
    "TMC5160",
    0x30,
    0,
    DRV_STATUS_OT | DRV_STATUS_S2GA | DRV_STATUS_S2GB |
        DRV_STATUS_S2VSA | DRV_STATUS_S2VSB,
    true,
    12.0e6,
    0.075,
};

// Returns nullptr for an unrecognised name, so config validation can report it
// rather than silently defaulting to the wrong chip.
inline const TmcDriverVariant* driverByName(const std::string& n) {
    if (n == "TMC5130")
        return &DRIVER_TMC5130;
    if (n == "TMC5160")
        return &DRIVER_TMC5160;
    return nullptr;
}

// RMS phase current for a given IRUN/CS setting.
//
//   I_rms = (globalScaler/256) * ((CS+1)/32) * (V_fs / (R_sense + 0.02)) / sqrt(2)
//
// V_fs is 0.325 V normally, 0.180 V with CHOPCONF's vsense bit set (which
// trades headroom for resolution at low current). The +0.02 ohm is the
// datasheet allowance for parasitic trace and bond-wire resistance. The
// globalScaler term is 1.0 on a chip without that register.
inline double rmsCurrentAmps(const TmcDriverVariant& v, int irun, double rsense,
                             bool vsense, int globalScaler) {
    const double vfs = vsense ? 0.180 : 0.325;
    double scale = 1.0;
    if (v.hasGlobalScaler) {
        // GLOBALSCALER 0 means full scale (256), not zero current.
        int gs = (globalScaler <= 0 || globalScaler > 256) ? 256 : globalScaler;
        scale = gs / 256.0;
    }
    return scale * ((irun + 1) / 32.0) * (vfs / (rsense + 0.02)) / 1.41421356237;
}

// Velocity register value for a requested revolutions/second.
// VMAX is an integer, so at the very low speeds this plugin uses the
// programmed velocity can sit well below the requested one -- callers must
// derive move timeouts from the returned value, not from the request.
inline uint32_t vmaxForRps(double rps, double fclkHz) {
    double v = rps * USTEPS_PER_REV * 16777216.0 / fclkHz;
    // VMAX is 23 bits, and the datasheet keeps it below 2^23 - 512.
    constexpr double VMAX_LIMIT = 8388096.0;
    if (!(v < VMAX_LIMIT))
        v = VMAX_LIMIT;
    long r = (long)(v + 0.5);
    if (r < 1)
        r = 1;
    return (uint32_t)r;
}

// Microsteps/second the chip will actually run at for a given VMAX register.
inline double ustepsPerSecForVmax(uint32_t vmax, double fclkHz) {
    return (double)vmax * fclkHz / 16777216.0;
}

// AMAX/DMAX/A1/D1 register value for a requested acceleration, in
// full steps/s^2. Datasheet formula: a[usteps/s^2] = AMAX * fCLK^2 /
// (512*256) / 2^24, inverted here to solve for AMAX. These fields are
// 16-bit, so the result is clamped to that range -- a caller asking for
// more acceleration than the chip can represent just gets the chip's max.
inline uint32_t amaxForAccel(double fullstepsPerSecSq, double fclkHz) {
    double usteps_s2 = fullstepsPerSecSq * USTEPS_PER_FULLSTEP;
    double a = usteps_s2 * 512.0 * 256.0 * 16777216.0 / (fclkHz * fclkHz);
    long r = (long)(a + 0.5);
    if (r < 1)
        r = 1;
    if (r > 65535)
        r = 65535;
    return (uint32_t)r;
}

} // namespace tmc
