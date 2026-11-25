#pragma once
/*
 * The SPI transport for one Trinamic driver.
 *
 * A TmcDevice owns one chip select for the life of the process. It is NOT
 * thread safe by design: every device belongs to exactly one axis worker
 * thread, which is the single serialisation point for that chip. Nothing else
 * -- not the fppd output thread, not an HTTP handler -- ever touches it. The
 * bus underneath is shared between chips and serialised per datagram by a
 * per-bus lock (SpiTmcDevice::xfer), so two workers on one bus never
 * interleave a transfer.
 */

#include <cstdint>
#include <string>

#include "TmcRegs.h"

class PinCapabilities;

namespace tmc {

class TmcDevice {
public:
    virtual ~TmcDevice() {}

    virtual bool open(std::string& err) = 0;
    virtual void close() = 0;

    // True for the simulator, so callers can label what they are reporting
    // instead of a user mistaking simulated numbers for a live chip.
    virtual bool isSimulated() const { return false; }

    // One 40-bit datagram. addr already carries the write bit if this is a
    // write. Returns false only on a transport failure (a closed or erroring
    // fd), not on anything the chip reports.
    virtual bool xfer(uint8_t addr, uint32_t data, uint8_t& status, uint32_t& reply) = 0;

    // --- helpers, implemented on top of xfer() -------------------------

    void write(uint8_t addr, uint32_t data);

    // A read costs two datagrams: the first latches the address, the second
    // returns the data (the TMC SPI protocol).
    uint32_t read(uint8_t addr);
    int32_t readSigned(uint8_t addr, int bits = 32);

    // Poll until three consecutive reads agree, up to 15 attempts: for
    // XACTUAL and VACTUAL, where one corrupted read would become a real
    // position.
    int32_t readConsensusSigned(uint8_t addr, int bits = 32);

    // GSTAT is clear-on-read on the TMC5130 and write-1-to-clear on the
    // TMC5160 (the flags are written back so both end up cleared), so it
    // cannot be re-read for consensus. Instead the two datagrams of one read
    // are cross-checked: the address-latch transfer's status byte reports
    // the reset flag while still live, the second transfer returns it as
    // data. *resetConfirmed is true only when both agree; a reset seen by
    // the data byte alone is returned but flagged unconfirmed.
    uint32_t readGstat(bool* resetConfirmed = nullptr);

    // Write, read back, retry up to 5 times 20ms apart. Only for readable
    // registers (most ramp registers are write-only): RAMPMODE, XTARGET,
    // XACTUAL and SW_MODE.
    bool writeVerified(uint8_t addr, uint32_t value, const char* name, std::string& err);

    // Status byte from the most recent datagram. Free on every transfer:
    // reset / driver_error / sg2 / standstill.
    uint8_t lastStatus() const { return m_lastStatus; }

    // Datagrams sent since open, real or simulated.
    uint64_t datagrams() const { return m_datagrams; }

protected:
    // Only readConsensusSigned() needs the raw consensus read.
    uint32_t readConsensus(uint8_t addr);

    uint8_t m_lastStatus = 0;
    uint64_t m_datagrams = 0;
};

// ---------------------------------------------------------------------

class SpiTmcDevice : public TmcDevice {
public:
    // bus is the logical SPI bus (0 = SPI0, 1 = SPI1, as spiBuses() in
    // AxisConfig.h numbers them); csPin the FPP name of the header pin driven
    // as this chip's select ("P1-24"). axisName only labels the pin: it is
    // the gpiod consumer description FPP gives the CS line.
    SpiTmcDevice(int bus, const std::string& csPin, uint32_t speedHz,
                 const std::string& axisName);
    ~SpiTmcDevice() override;

    bool open(std::string& err) override;
    void close() override;
    bool xfer(uint8_t addr, uint32_t data, uint8_t& status, uint32_t& reply) override;

    std::string devicePath() const;

private:
    int m_bus;
    std::string m_csPin;
    uint32_t m_speedHz;
    std::string m_axisName;
    // /dev/spidev<bus>.0, opened so the controller drives no chip select of
    // its own (SPI_NO_CS where the driver accepts it); this chip's CS line,
    // requested through FPP's PinCapabilities and toggled around every
    // xfer() under the bus's lock; and whether this device holds one
    // reference on the bus's data pins (SPI1's are muxed with
    // configPin("spi") by the first device on the bus, released by the last).
    int m_fd = -1;
    const PinCapabilities* m_cs = nullptr;
    bool m_holdsBus = false;
};

// ---------------------------------------------------------------------

/*
 * Enough of a Trinamic ramp generator to exercise everything above the SPI
 * layer with no hardware attached. It moves in real time at the programmed
 * velocity rather than jumping XACTUAL to XTARGET, so move timeouts,
 * position_reached polling, live status mid-move and preempting a cycle
 * mid-leg are all exercised for real.
 */
class FakeTmcDevice : public TmcDevice {
public:
    explicit FakeTmcDevice(const TmcDriverVariant& variant);

    bool open(std::string& err) override;
    void close() override {}
    bool isSimulated() const override { return true; }
    bool xfer(uint8_t addr, uint32_t data, uint8_t& status, uint32_t& reply) override;

    // Model yanking VS: registers reset, XACTUAL zeroed, GSTAT's reset flag
    // comes up. This is how the reference-loss path gets tested.
    void simulatePowerCycle();

private:
    void advance();
    uint32_t latchFor(uint8_t addr);

    // Every datagram returns the data for the address sent in the PREVIOUS
    // datagram, as on real silicon. Answering immediately would hide
    // read-and-clear registers: GSTAT would clear on the address latch and
    // the second transfer read 0, making a driver reset invisible.
    uint32_t m_pendingReply = 0;

    const TmcDriverVariant& m_variant;
    uint32_t m_reg[128];
    int32_t m_xactual = 0;
    int32_t m_xtarget = 0;
    double m_lastAdvance = 0.0;
};

} // namespace tmc
