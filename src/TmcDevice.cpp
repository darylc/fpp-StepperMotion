#include "fpp-pch.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "TmcDevice.h"

#include "common.h"
#include "fppversion.h"
#include "log.h"
#include "settings.h"
#include "util/GPIOUtils.h"

#include "AxisConfig.h"
#include "TmcUtil.h"

namespace tmc {

// The running FPP (libfpp, not the headers this was built against) is 10.2 or
// later. Release builds report their minor version (10.2 -> "2"); master
// reports 1000, above every release.
static bool fppHasSpiPinMode() {
    const int major = atoi(getFPPMajorVersion());
    const int minor = atoi(getFPPMinorVersion());
    return major > 10 || (major == 10 && minor >= 2);
}


// ---------------------------------------------------------------- base --

void TmcDevice::write(uint8_t addr, uint32_t data) {
    uint8_t status = 0;
    uint32_t reply = 0;
    xfer(addr | 0x80, data, status, reply);
}

uint32_t TmcDevice::read(uint8_t addr) {
    uint8_t status = 0;
    uint32_t reply = 0;
    // First datagram latches the address; its reply belongs to whatever was
    // addressed previously and is discarded. The second returns our data.
    xfer(addr & 0x7F, 0, status, reply);
    xfer(addr & 0x7F, 0, status, reply);
    return reply;
}

// Sign-extend the low `bits` of a register word.
static int32_t signExtend(uint32_t v, int bits) {
    if (bits >= 32)
        return (int32_t)v;
    const uint32_t signBit = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    if (v & signBit)
        return (int32_t)(v - (1u << bits));
    return (int32_t)v;
}

int32_t TmcDevice::readSigned(uint8_t addr, int bits) {
    return signExtend(read(addr), bits);
}

uint32_t TmcDevice::readConsensus(uint8_t addr) {
    uint32_t last = 0;
    int agree = 0;
    uint32_t v = 0;
    for (int i = 0; i < 15; i++) {
        v = read(addr);
        if (i > 0 && v == last) {
            if (++agree >= 2) // three consecutive identical values
                return v;
        } else {
            agree = 0;
        }
        last = v;
    }
    // No three in a row: the last value read, since the status page needs a
    // number even on a bad link.
    return v;
}

int32_t TmcDevice::readConsensusSigned(uint8_t addr, int bits) {
    return signExtend(readConsensus(addr), bits);
}

uint32_t TmcDevice::readGstat(bool* resetConfirmed) {
    uint8_t statusLatch = 0, statusData = 0;
    uint32_t reply = 0;
    xfer(REG_GSTAT & 0x7F, 0, statusLatch, reply);
    xfer(REG_GSTAT & 0x7F, 0, statusData, reply);
    m_lastStatus = statusData;
    if (resetConfirmed) {
        *resetConfirmed = (statusLatch & SPI_STATUS_RESET) != 0;
    }
    // The TMC5130 clears GSTAT on read; the TMC5160's bits are
    // write-1-to-clear. Writing the flags back is a no-op on a 5130, so it
    // is done for both.
    if (reply & GSTAT_ALL_FLAGS)
        write(REG_GSTAT | 0x80, reply & GSTAT_ALL_FLAGS);
    return reply;
}

bool TmcDevice::writeVerified(uint8_t addr, uint32_t value, const char* name,
                              std::string& err) {
    uint32_t rb = 0;
    for (int i = 0; i < 5; i++) {
        write(addr, value);
        rb = read(addr);
        if (rb == value)
            return true;
        usleep(20000);
    }
    char buf[192];
    snprintf(buf, sizeof(buf),
             "could not verify write to %s (0x%02X): wrote 0x%08X, read back 0x%08X",
             name, addr & 0x7F, value, rb);
    err = buf;
    return false;
}

// ----------------------------------------------------------------- spi --

// Every chip on a bus shares its clock and data lines and has its own chip
// select, which this plugin drives. CS low, transfer, CS high has to happen
// as one unit, or two workers on the bus would select two chips at once and
// both would latch the same garbage; the bus lock makes it one. refs counts
// the open devices on the bus, so SPI1's data pins are muxed by the first
// and handed back by the last.
namespace {
struct SpiBusState {
    std::mutex lock;
    int refs = 0;
};
SpiBusState& busState(int bus) {
    static SpiBusState states[SPI_BUSES];
    return states[bus];
}

// SPI0's data pins are muxed by the device tree at boot (dtparam=spi=on)
// and stay that way; FPP's other SPI users (MAX7219, GenericSPI, SPIws2801)
// rely on it, so they are left alone. SPI1 comes from FPP's fpp-spi1-nopins
// overlay, which enables the controller with no pins assigned -- the
// BeagleBone/ttyAMA0 pattern -- so whoever uses it muxes them with
// configPin("spi") and releases them afterwards.
// A pin FPP could not request (another process holds it, say) still comes
// back from configPin() with 0; isAcquired() is how to tell, and it arrived
// in FPP after 10.1.2. On an FPP without it the request is taken at its word
// and a pin that was never taken shows up as a chip that never answers.
template <typename P>
bool pinAcquired(const P& p) {
    if constexpr (requires { p.isAcquired(); })
        return p.isAcquired();
    else
        return true;
}

bool muxesOwnDataPins(const SpiBusInfo& b) {
    return b.bus != 0;
}

bool acquireDataPins(const SpiBusInfo& b, std::string& err) {
    SpiBusState& st = busState(b.bus);
    std::lock_guard<std::mutex> lk(st.lock);
    if (st.refs == 0 && muxesOwnDataPins(b)) {
        const std::string pins[] = { b.miso, b.mosi, b.sclk };
        for (int i = 0; i < 3; i++) {
            if (PinCapabilities::getPinByName(pins[i]).configPin("spi", true, "StepperMotion SPI" + std::to_string(b.bus)) < 0) {
                for (int j = 0; j < i; j++)
                    PinCapabilities::getPinByName(pins[j]).releasePin();
                err = "could not mux " + pins[i] + " to SPI" + std::to_string(b.bus) +
                      " (fppd's log says why: usually another overlay, such as an I2S audio HAT, holds it)";
                return false;
            }
        }
    }
    st.refs++;
    return true;
}

void releaseDataPins(const SpiBusInfo& b) {
    SpiBusState& st = busState(b.bus);
    std::lock_guard<std::mutex> lk(st.lock);
    if (--st.refs == 0 && muxesOwnDataPins(b)) {
        for (const std::string& p : { b.miso, b.mosi, b.sclk })
            PinCapabilities::getPinByName(p).releasePin();
    }
}
} // namespace

SpiTmcDevice::SpiTmcDevice(int bus, const std::string& csPin, uint32_t speedHz,
                           const std::string& axisName) :
    m_bus(bus),
    m_csPin(csPin),
    m_speedHz(speedHz ? speedHz : 1000000),
    m_axisName(axisName) {
}

SpiTmcDevice::~SpiTmcDevice() {
    close();
}

std::string SpiTmcDevice::devicePath() const {
    const SpiBusInfo* b = spiBus(m_bus);
    return "/dev/spidev" + std::to_string(b ? b->spidev : m_bus) + ".0";
}

bool SpiTmcDevice::open(std::string& err) {
    if (m_fd >= 0)
        return true;

    const SpiBusInfo* b = spiBus(m_bus);
    if (!b) {
        err = "SPI" + std::to_string(m_bus) + " is not a bus this plugin knows";
        return false;
    }
    const std::string path = devicePath();
    if (!spiBusAvailable(*b)) {
        err = path + " does not exist";
        // SPI0 is in every FPP image's device tree; SPI1 only from FPP 10.2,
        // whose overlay enables it with no pins assigned.
        if (m_bus != 0)
            err += " -- SPI" + std::to_string(m_bus) +
                   " needs FPP's fpp-spi1-nopins overlay, which upgrading FPP adds; "
                   "reboot once after the upgrade (press F1 on the Stepper Motion page)";
        return false;
    }
    // The "spi" pin mode arrived in FPP 10.2 together with fpp-spi1-nopins. On an
    // FPP without it, configPin("spi") falls through to a plain GPIO request
    // and turns the data pins into GPIOs, so it is never probed: the running
    // FPP's version says whether it has both. A spidev1.0 on an older FPP
    // comes from some other overlay (the stock spi1-1cs, say), which already
    // muxes the pins and holds a chip select of its own.
    if (muxesOwnDataPins(*b) && !fppHasSpiPinMode()) {
        err = path + " exists, but this FPP cannot mux SPI" + std::to_string(m_bus) +
              "'s pins (no \"spi\" pin mode): the node comes from another overlay in config.txt. "
              "SPI" + std::to_string(m_bus) + " needs FPP 10.2 or later, which provides fpp-spi1-nopins "
              "(press F1 on the Stepper Motion page)";
        return false;
    }

    // dtoverlay=spi0-0cs entered FPP's image build on 2026-06-30 (FPP 10.0).
    // A box still on an image from before that, whose FPP was upgraded in
    // place from git rather than by an OS-image (fppos) upgrade -- which does
    // rewrite config.txt from the new image -- has no such line, so the kernel
    // still holds GPIO 8 and 7 (header P1-24 and P1-26) as SPI0's own chip
    // selects, and /dev/spidev0.1 exists to show it.  Those two pins cannot be
    // a GPIO chip select on such a box; say exactly that rather than the
    // generic failure below.
    if (m_bus == 0 && (m_csPin == "P1-24" || m_csPin == "P1-26") && FileExists("/dev/spidev0.1")) {
        err = m_csPin + " is held by the kernel as an SPI0 chip select on this box: config.txt has no "
              "dtoverlay=spi0-0cs (FPP images from 10.0 on ship it; an older image upgraded in place, "
              "without an OS image upgrade, keeps its old config.txt). Give this stepper another "
              "chip-select pin, or add dtoverlay=spi0-0cs to config.txt and reboot -- see the setup help";
        return false;
    }

    const PinCapabilities* cs = PinCapabilities::getPinByName(m_csPin).ptr();
    if (!cs) {
        err = m_csPin + " is not a pin this FPP knows";
        return false;
    }
    if (!acquireDataPins(*b, err))
        return false;
    m_holdsBus = true;

    // Output idling high before the device is opened, so the chip never sees
    // a floating CS while the mode is being set. The description is what
    // gpioinfo shows against the line.
    if (cs->configPin("gpio", true, "StepperMotion CS " + m_axisName) < 0 || !pinAcquired(*cs)) {
        err = "could not take " + m_csPin + " as the chip select (fppd's log says why: another "
              "FPP output or input may be configured on it)";
        close();
        return false;
    }
    m_cs = cs;
    m_cs->setValue(1);

    m_fd = ::open(path.c_str(), O_RDWR);
    if (m_fd < 0) {
        err = path + ": " + strerror(errno);
        close();
        return false;
    }

    // The datasheet says mode 3 (CPOL=1, CPHA=1); mode 0 is used instead.
    // The chip samples SDI on the rising edge of SCK and shifts SDO on the
    // falling edge, which is what both modes do -- they differ only in the
    // idle level of SCK, and the TMC does not care (200/200 clean IOIN and
    // XACTUAL reads in each on a TMC5160; mode 2 fails every read). What
    // does care is the Pi's SPI1 controller on the Pi 2/3/4 (bcm2835-aux-spi):
    // its driver refuses CPHA=1, so mode 3 would confine SPI1 to the Pi 5.
    //
    // SPI_NO_CS asks the controller not to drive a chip select of its own.
    // The bcm2835/2711 drivers honour it; the Pi 5's (RP1, DesignWare) does
    // not list it and answers EINVAL. There it is not needed: spi0-0cs and
    // fpp-spi1-nopins leave no CS pad muxed, so the controller's native CS
    // toggles into nothing. On a Pi <=4 without spi0-0cs the flag is what
    // keeps GPIO 8 quiet, so it is tried first and dropped only when refused.
    uint32_t mode = SPI_MODE_0 | SPI_NO_CS;
    if (ioctl(m_fd, SPI_IOC_WR_MODE32, &mode) < 0) {
        mode = SPI_MODE_0;
        if (ioctl(m_fd, SPI_IOC_WR_MODE32, &mode) < 0) {
            err = path + ": could not set SPI mode 0: " + strerror(errno);
            close();
            return false;
        }
        LogInfo(VB_PLUGIN, "%s: %s: driver does not support SPI_NO_CS, relying on no CS pad being muxed\n",
                LOGPFX, path.c_str());
    }
    // The mode lives on the kernel's spi_device, shared by every open of the
    // node, so another user of the same spidev (a kernel-CS channel output,
    // say) changes it for both. Say so once at open.
    uint32_t got = 0;
    if (ioctl(m_fd, SPI_IOC_RD_MODE32, &got) == 0 && got != mode) {
        LogWarn(VB_PLUGIN, "%s: %s: SPI mode reads back 0x%x after setting 0x%x; something else is using this device\n",
                LOGPFX, path.c_str(), got, mode);
    }
    uint8_t bits = 8;
    if (ioctl(m_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(m_fd, SPI_IOC_WR_MAX_SPEED_HZ, &m_speedHz) < 0) {
        err = path + ": failed to configure SPI: " + strerror(errno);
        close();
        return false;
    }
    return true;
}

void SpiTmcDevice::close() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_cs) {
        m_cs->releasePin();
        m_cs = nullptr;
    }
    if (m_holdsBus) {
        if (const SpiBusInfo* b = spiBus(m_bus))
            releaseDataPins(*b);
        m_holdsBus = false;
    }
}

bool SpiTmcDevice::xfer(uint8_t addr, uint32_t data, uint8_t& status, uint32_t& reply) {
    m_datagrams++;
    status = 0;
    reply = 0;
    if (m_fd < 0) {
        return false;
    }

    // One 40-bit datagram: address, then the data MSB first.
    uint8_t tx[5] = {
        addr,
        (uint8_t)((data >> 24) & 0xFF),
        (uint8_t)((data >> 16) & 0xFF),
        (uint8_t)((data >> 8) & 0xFF),
        (uint8_t)(data & 0xFF),
    };
    uint8_t rx[5] = { 0, 0, 0, 0, 0 };

    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = 5;
    tr.speed_hz = m_speedHz;
    tr.bits_per_word = 8;

    // CS rising at the end is the frame boundary the TMC latches on.
    int ret;
    {
        std::lock_guard<std::mutex> lk(busState(m_bus).lock);
        m_cs->setValue(0);
        ret = ioctl(m_fd, SPI_IOC_MESSAGE(1), &tr);
        m_cs->setValue(1);
    }
    if (ret < 1) {
        return false;
    }

    status = rx[0];
    m_lastStatus = status;
    reply = ((uint32_t)rx[1] << 24) | ((uint32_t)rx[2] << 16) |
            ((uint32_t)rx[3] << 8) | (uint32_t)rx[4];
    return true;
}

// ---------------------------------------------------------------- fake --

FakeTmcDevice::FakeTmcDevice(const TmcDriverVariant& variant) :
    m_variant(variant) {
    simulatePowerCycle();
}

bool FakeTmcDevice::open(std::string&) {
    return true;
}

void FakeTmcDevice::simulatePowerCycle() {
    memset(m_reg, 0, sizeof(m_reg));
    // VERSION in the top byte, DRV_ENN and SD_MODE low: a correctly wired,
    // freshly powered chip.
    m_reg[REG_IOIN] = ((uint32_t)m_variant.versionByte << 24) | 0x0C;
    m_reg[REG_GSTAT] = GSTAT_RESET;
    m_reg[REG_DRV_STATUS] = DRV_STATUS_STST;
    m_reg[REG_RAMPMODE] = RAMPMODE_POSITIONING;
    m_xactual = 0;
    m_xtarget = 0;
    m_lastAdvance = monotonicSeconds();
}

void FakeTmcDevice::advance() {
    double now = monotonicSeconds();
    double dt = now - m_lastAdvance;
    m_lastAdvance = now;
    if (dt <= 0)
        return;

    if (m_reg[REG_RAMPMODE] != RAMPMODE_POSITIONING || m_reg[REG_VMAX] == 0) {
        m_reg[REG_VACTUAL] = 0;
        m_reg[REG_DRV_STATUS] |= DRV_STATUS_STST;
        return;
    }

    double uss = ustepsPerSecForVmax(m_reg[REG_VMAX], m_variant.defaultFclkHz);
    double step = uss * dt;
    int32_t remaining = m_xtarget - m_xactual;
    if (remaining == 0) {
        m_reg[REG_VACTUAL] = 0;
        m_reg[REG_DRV_STATUS] |= DRV_STATUS_STST;
        return;
    }

    int32_t delta = (int32_t)(remaining > 0 ? std::floor(step) : -std::floor(step));
    if (std::abs((long)delta) >= std::abs((long)remaining))
        delta = remaining;
    m_xactual += delta;
    // No mechanical stops are modelled: on the real chip XACTUAL reaches
    // XTARGET whether or not the shaft followed, and a simulator that showed
    // a stall would invite code the silicon cannot support.

    if (m_xactual == m_xtarget) {
        m_reg[REG_VACTUAL] = 0;
        m_reg[REG_DRV_STATUS] |= DRV_STATUS_STST;
    } else {
        m_reg[REG_VACTUAL] = (uint32_t)(int32_t)(delta >= 0 ? (int32_t)uss : -(int32_t)uss);
        m_reg[REG_DRV_STATUS] &= ~DRV_STATUS_STST;
    }
}

// Computes what this address will return on the NEXT datagram, applying any
// read side effects at the moment the address is latched.
uint32_t FakeTmcDevice::latchFor(uint8_t a) {
    switch (a) {
    case REG_XACTUAL:
        return (uint32_t)m_xactual;
    case REG_XTARGET:
        return (uint32_t)m_xtarget;
    case REG_RAMP_STAT:
        return (m_xactual == m_xtarget) ? RAMP_STAT_POSITION_REACHED : 0;
    case REG_GSTAT: {
        // Read-and-clear, as on the chip.
        uint32_t v = m_reg[REG_GSTAT];
        m_reg[REG_GSTAT] = 0;
        return v;
    }
    case REG_DRV_STATUS:
        // CS_ACTUAL tracks IRUN so the tuning UI has something to show.
        return (m_reg[REG_DRV_STATUS] & ~(0x1Fu << 16)) |
               (((m_reg[REG_IHOLD_IRUN] >> 8) & 0x1F) << 16);
    default:
        return m_reg[a];
    }
}

bool FakeTmcDevice::xfer(uint8_t addr, uint32_t data, uint8_t& status, uint32_t& reply) {
    m_datagrams++;
    bool isWrite = (addr & 0x80) != 0;
    uint8_t a = addr & 0x7F;

    advance();

    status = 0;
    if (m_reg[REG_GSTAT] & GSTAT_RESET)
        status |= SPI_STATUS_RESET;
    if (m_reg[REG_DRV_STATUS] & DRV_STATUS_STST)
        status |= SPI_STATUS_STANDSTILL;
    m_lastStatus = status;

    // This datagram carries the answer to the previous one's address.
    reply = m_pendingReply;

    if (isWrite) {
        if (a == REG_GSTAT) {
            // Write-1-to-clear, as on the TMC5160.
            m_reg[a] &= ~data;
        } else {
            m_reg[a] = data;
        }
        if (a == REG_XTARGET) {
            m_xtarget = (int32_t)data;
        } else if (a == REG_XACTUAL) {
            // Writing XACTUAL in positioning mode starts a motion, as on
            // real silicon.
            m_xactual = (int32_t)data;
            if (m_reg[REG_RAMPMODE] != RAMPMODE_POSITIONING)
                m_xtarget = m_xactual;
        }
        m_pendingReply = 0;
    } else {
        m_pendingReply = latchFor(a);
    }
    return true;
}

} // namespace tmc
