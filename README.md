# Stepper Motion for FPP

Moves stepper motors from your show. Up to eight Trinamic **TMC5130** or
**TMC5160** driver boards are wired to the Raspberry Pi's header, and each
stepper follows one channel of your sequence: the channel's value picks a
motion (go to a position, sweep back and forth, return home). Make the
channels in your sequencer like any other prop and the motors move with the show.

The setup page has a commissioning wizard (find the home stop, measure the
travel), a tuning panel, a preset editor, and FPP commands for playlists, the
scheduler, events and MQTT.

**Needs:** a Raspberry Pi running FPP 10.4 or later, and TMC5130A-BOB or
TMC5160-BOB breakout boards (see [Boards](#boards)).

## Quick start

1. **Wire one board** to SPI0 as in [Wiring](#wiring): 3.3 V logic supply,
   ground, the three SPI data lines, a chip select on header pin 24, and
   `DRV_ENN` and `CLK16` tied to ground. Connect the motor and its supply,
   with the bulk capacitor fitted.
2. **Install** the plugin from FPP's **Plugin Manager** (Content Setup →
   Plugin Manager) and restart fppd when FPP asks.
3. Open **Input/Output Setup → Stepper Motion**. Press **F1** there at any
   time for help.
4. **Add stepper** (or **Add simulated** to try the page without a board). It
   gets channel slot 1 and the SPI0 chip select on header
   pin 24. Pick the driver (TMC5130 or TMC5160) under Global settings if it
   is not the default, then **Save configuration** and restart fppd when FPP
   asks.
5. **Set the motor current** (see [Motor current](#motor-current)), then
   **Probe**. It should report the chip it found and no problems.
6. **Commission** on the stepper's *Commissioning* tab: jog to the end of
   travel you want as home, **Set Home**, jog to the other end, **Set Far
   Limit**. Mark the mechanism at both ends and watch **Test travel**: every
   cycle should meet both marks.
7. **Add a preset**: in the Presets table, value 10 = *Go to* 50%, for
   example. **Run now** tries it.
8. **Sequence it**: a stepper moves to preset 10 when its channel carries
   exactly 10 (see [Sequencing it](#sequencing-it)).
   Value 0, or the end of the show, sends it home.

## How it works

- **The channel value is a preset number, not a position.** Value 10 might
  mean "go to 50%" and value 20 "sweep back and forth". Presets are defined
  once in the page's Presets table; one marked *All steppers* means the same
  motion on any stepper, one scoped to a single stepper is ignored by the
  others.
- **Value 0 means "return to home"**, and so does the channel going quiet
  (no frames for longer than the idle timeout, 500 ms by default). Returning
  home is a plain move to the stored position 0; it never drives into the
  stop.
- **The plugin listens on 8 consecutive channels**, one slot per possible
  stepper: the *channel block*. Set where it starts under Global settings →
  Channels: a channel number, or the first channel of a pixel
  overlay model (looked up again at every fppd start, so the block follows
  the model if a new upload moves it). A stepper can instead read a channel of
  its own anywhere in the show (its *Channel source*).
- **Position is open-loop.** The driver counts the steps it commanded; there
  is no sensor on the shaft. The plugin cross-checks the count against the
  driver chip and notices a driver reset. If they disagree, the stepper's
  position is marked lost and it must be rehomed before it moves to a
  position again. With *Auto-recover on demand* (on by default) that rehome
  happens by itself before the next move.

### When the steppers move on their own

- **When fppd starts:** with no show running every channel is 0, so a stepper
  whose stored position is valid but away from home (after *Go To Position*,
  *Park*, or fppd stopping mid-show) **drives home as soon as fppd starts**.
  A stepper with no valid position (never homed, or its driver lost power)
  does not move. *Rehome at startup* (off by default) makes a stepper rehome
  into its stop at every fppd start instead.
- **When fppd stops:** every stepper's coils are released, whatever its hold
  setting. A mechanism that is sprung or can be back-driven may move while
  fppd is down, and its stored position is then wrong until the next
  *Rehome*. fppd does not wait for a moving stepper to stop: one still moving
  is stopped, released and marked as needing a rehome.

## Sequencing it

1. Make one model covering the plugin's **8 channels**, starting at the
   channel set under Global settings → Channels: the model's channel 1 is
   stepper slot 1, and so on. Always make all 8, so adding a stepper later
   never moves another.
2. On a stepper's channel, put **exactly the preset's value**, 0–255, for as
   long as the motion should run. Use an effect that sets a precise level,
   and turn off anything that scales values on that model or its output:
   brightness, dimming curves, gamma. A channel carrying 9 or 11 instead of
   10 runs a different preset, or none.
3. End with 0, or simply let the sequence end: either sends the stepper home.

**Check what arrives:** while the sequence plays, each stepper's line in
the Steppers card shows its channel and the value it is receiving, as
`ch <channel> = <value>`, followed by the preset it is running. If the
value is not the one you meant, fix it in the sequence.

While a show drives a stepper, the page's manual controls for it are
refused ("a sequence is currently driving this stepper").

## Motor current

Set the current before the first move, under Global settings (or a
stepper's own override). The **IRUN** slider shows the real RMS current
beside it; **keep that at or below your motor's rated current per phase**
(on its datasheet or label). Too much heats the motor and driver for no
gain; too little loses steps.

Then mark the mechanism at both ends of its travel and watch *Test travel*
for ten cycles. Missing a mark means the motor is losing steps at that
current and speed. Use the lowest current at which every cycle meets both
marks, plus a little margin. **The plugin cannot see lost steps**: there is no
shaft sensor, and the step counter it reports reads correct even when the
motor stalls. Check the far mark too: a stalling motor can still be pushed
back against the home stop.

**IHOLD flows all the time between moves** and is what overheats things.
Keep it low unless the mechanism needs holding torque. A sprung mechanism
needs some: with no current its shaft is pulled away and the stored position
is lost. *Hold for* (30 s by default) releases the coils that long after the
last move.

## Troubleshooting

**A successful *Probe* forgets the far limit** (position 0 is kept): it is step 1
of commissioning. After probing a commissioned stepper, jog to the far end
and press *Set Far Limit* again.

| Symptom | Likely cause and fix |
|---|---|
| *Probe*: version byte `0x00` (all-zero reads) | The board has no motor supply (the chip runs off VS), or the CSN or SDO wire is off. Check VS, then the chip-select wire, then SDO. |
| *Probe*: version byte `0xFF`, or reads come and go | Wiring or pin conflict: check SCK/SDI/SDO and ground; make sure nothing else in FPP (a GPIO output, a pixel string, a cape) uses the bus's data pins. Lower *Advanced → SPI speed*. |
| *Probe*: a different version byte | The stepper is set to the wrong driver: TMC5160 reads `0x30`, TMC5130 `0x11`. |
| *Probe*: `DRV_ENN is high` | Tie the board's `DRV_ENN` pin to ground. The chip answers, but the motor never moves. |
| *Probe*: `SD_MODE high` | The board is in step/direction mode: the R1/R2 jumper must have R2 fitted (factory default). |
| "open load on both coils" | No motor connected, or both coil pairs open. Check the motor plug. |
| A stepper on SPI1 refuses to start | The box has no `/dev/spidev1.0`: reboot once after upgrading FPP (see [SPI1](#spi1)). |
| A stepper on header pin 24 or 26 refuses to start | An older image: see [Older images](#older-images-upgraded-in-place). |
| Corrupted reads or a hanging chip at higher current | Fit the bulk capacitor at VS; use short, heavy supply leads. |
| *Test travel* misses a mark | Lost steps: raise IRUN, lengthen the ramp time, or lower the speed. |
| "position doesn't match" / "the driver chip reset" | The position is lost (a brief power drop, or the shaft was moved). *Rehome*, or let *Auto-recover on demand* do it. |
| The motor hums but barely moves | Current too low, or speed too high for the motor: lower the speed, lengthen the ramp. |
| Steppers ignore the show | The page's top warning says if FPP's channel output entry is off or missing. Otherwise check the value arriving (see [Sequencing](#sequencing-it)) and that a preset exists for it. |
| The page says fppd is not running | The page loads and saves through fppd; start it. |

The plugin logs to fppd's log (`logs/fppd.log`), under the *Plugin* category.

## Wiring

Up to eight TMC5130A-BOB or TMC5160-BOB boards on the Pi's SPI0 and SPI1
buses. Both boards use the same 22-pin header (left column 1–11, right
column 12–22; pin 1 is marked). The three data lines of a bus are shared by
every board on it; each board gets its own chip select. Pi pins are physical
40-pin header numbers, the same on every 40-pin Pi.

**The chip select is a pin you choose** under the stepper's General section;
any free GPIO pin of the header works. New steppers get the next pin of this
default wiring (each cell is BCM GPIO / header pin; the arrow is the
direction of the signal):

| Default | Bus | CS → CSN (BOB 3) | MOSI → SDI (BOB 5) | MISO ← SDO (BOB 6) | SCLK → SCK (BOB 4) |
|---|---|---|---|---|---|
| 1 | SPI0 | GPIO8 / 24 | GPIO10 / 19 | GPIO9 / 21 | GPIO11 / 23 |
| 2 | SPI0 | GPIO7 / 26 | GPIO10 / 19 | GPIO9 / 21 | GPIO11 / 23 |
| 3 | SPI0 | GPIO25 / 22 | GPIO10 / 19 | GPIO9 / 21 | GPIO11 / 23 |
| 4 | SPI0 | GPIO24 / 18 | GPIO10 / 19 | GPIO9 / 21 | GPIO11 / 23 |
| 5 | SPI1 | GPIO18 / 12 | GPIO20 / 38 | GPIO19 / 35 | GPIO21 / 40 |
| 6 | SPI1 | GPIO17 / 11 | GPIO20 / 38 | GPIO19 / 35 | GPIO21 / 40 |
| 7 | SPI1 | GPIO16 / 36 | GPIO20 / 38 | GPIO19 / 35 | GPIO21 / 40 |
| 8 | SPI1 | GPIO27 / 13 | GPIO20 / 38 | GPIO19 / 35 | GPIO21 / 40 |

The page's Global settings → SPI configuration draws this table for the
steppers you have actually configured.

The rest of the BOB header:

| BOB pin | Signal | Connect to | Notes |
|---|---|---|---|
| 1 | VCC_IO | Pi 3.3 V (header 1 or 17) | **3.3 V, not 5 V**: at 5 V the board's SDO overdrives the Pi's input |
| 2, 7 | GND | Pi ground (6, 9, 14, 20, 25, 30, 34 or 39) | |
| 8 | CLK16 | Ground | Internal clock |
| 9 | DRV_ENN | Ground | Floating or high: the chip answers but the motor never moves; *Probe* reports it |
| 10, 11 | REFL, REFR | Nothing (or ground) | End-stop inputs; the plugin ignores them |
| 12 | VS | Motor supply + | Within the board's rated range; fit the bulk capacitor here |
| 13, 18 | GND | Motor supply − | |
| 14–17 | B2, B1, A2, A1 (5160) / OA1, OA2, OB1, OB2 (5130A) | Motor coils | Swap one coil pair to reverse direction |
| 19 | DIAG0 (5160) / AIN_IREF (5130A) | Nothing | |
| 20–22 | ENCA, ENCB, ENCN | Nothing | Encoder inputs, unused |

- **`SD_MODE` is not on the header.** It is the R1/R2 solder jumper: R2
  fitted (factory default) selects the internal ramp generator the plugin
  needs.
- **Fit the bulk capacitor** the board has a footprint for (100–220 µF,
  rated above your supply) right at VS. Without it, motor current above
  about 0.7 A on jumper-wire supply leads turns the SPI link to garbage and
  can hang the chip.
- **Run the motor supply at the board's working voltage, on short, heavy
  leads.** A marginal supply shows up as the odd corrupted read during fast
  moves. The plugin re-reads before believing a fault, so one bad read does
  not stop a show; a real fault still does.
- **Keep SPI leads short.** Four boards per bus on flying leads is the
  comfortable limit.
- **The pins a running stepper uses are fppd's** while it runs: the data
  lines of its bus (SPI0 19/21/23, SPI1 35/38/40) and its chip select. Don't
  configure them for FPP GPIO inputs/outputs or pixel strings; FPP's GPIO
  page does not know about them.
- **SPI1's data pins are the I²S pins**, so a rig using SPI1 cannot also
  carry an I²S audio HAT or DAC. SPI0 alone leaves I²S free.

### SPI1

SPI1 comes from FPP's `fpp-spi1-nopins` overlay. New FPP images have it,
and upgrading FPP adds it to `config.txt` and asks for a reboot; after that
reboot `/dev/spidev1.0` exists. Until then a stepper on SPI1 refuses to start
and says so, while steppers on SPI0 run as usual. A `/dev/spidev1.0` from a
different overlay (the stock `spi1-1cs`, say) is refused too: that overlay
already holds the pins and a chip select.

### Older images upgraded in place

FPP images from 10.0 on have `dtoverlay=spi0-0cs` in `config.txt`, which
leaves header pins 24 and 26 free for the plugin's chip selects. A box still
on an older image whose FPP was upgraded in place lacks that line, and the
kernel keeps those two pins as SPI0's own chip selects: a stepper on pin 24
or 26 refuses to start and says so. Pick another chip-select pin for it, or
add `dtoverlay=spi0-0cs` to `/boot/firmware/config.txt` and reboot.

### Boards

Developed and tested with:

- **Raspberry Pi:** a Compute Module 5 (SPI0) and a Pi 3 class board (SPI0
  and SPI1).
- **TMC5160-BOB** (external MOSFETs, for larger motors):
  [product page](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/tmc5160-bob.html) ·
  [datasheet (PDF)](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC5160-BOB_datasheet_rev1.10.pdf)
- **TMC5130A-BOB** (integrated MOSFETs, up to about 1 A RMS):
  [product page](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/tmc5130a-bob.html) ·
  [datasheet (PDF)](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC5130A-BOB_datasheet_rev1.00.pdf)

**Other TMC5130/TMC5160 boards are untested.** A board can only work if it
runs the chip in SPI mode with its internal ramp generator (`SD_MODE` low,
`SPI_MODE` high) and brings out CSN, SCK, SDI, SDO and `DRV_ENN`. Many
TMC5160 modules sold for 3D printers are set up for step/direction instead.
If its sense resistor or clock differs from the BOB's (0.075 Ω and 12 MHz on
a 5160, 0.15 Ω and 13.2 MHz on a 5130), enter them under the stepper's
*Advanced* settings.

## Using the page

Everything is on **Input/Output Setup → Stepper Motion**:

- **Steppers**: live state and manual controls for each stepper. The dial
  shows the position in degrees, with full steps and the measured range
  beneath it; that full-steps figure is what a *Go to* preset in full steps
  or a *Cycle* waypoint takes. **Home** is a plain move to position 0;
  **Rehome** drives into the stop to find position 0 again. The buttons in
  the card header act on every stepper at once.
- **Presets**: the channel value → motion table.
- **Stepper Configuration**: Global settings first (channels, SPI
  configuration, the driver and motor settings every stepper shares), then
  one tab per stepper: General (name, channel, SPI bus and chip select,
  *Enabled*), Commissioning, and its own overrides.

A stepper inherits every setting from Global settings unless it overrides
it: an empty field, a "(global …)" choice or an override switch left off
follows the global value, shown greyed. Turning **Enabled** off keeps a
stepper configured while fppd leaves its board alone (for a board that is
unplugged for now; takes effect at the next fppd restart).

Changes to the steppers themselves (adding, removing, the bus, chip select
or driver) need an fppd restart, which FPP prompts for. Tuning values apply
at once.

The plugin keeps its channel output entry (Channel Outputs page, *Misc* tab)
in step with the channel block. If you delete the entry, it comes back at the
next fppd start or the next *Save configuration*; the page warns while it is
missing or switched off.

### Commands

`Stepper Motion Rehome`, `Go To Position`, `Park`, `Release` and `Stop`,
usable from playlists, the scheduler, GPIO/event triggers and MQTT without a
channel.

### HTTP API

`/api/plugin-apis/stepper-motion?action=…&axis=<name>`, documented on FPP's
`/api` page. GET answers only the reads (`status`; without an axis,
`axisNames` and `config`). Everything that moves a motor or saves needs a
POST: `probe`, `jog`, `setHome`, `setMax`, `goto`, `rehome`, `testTravel`,
`runPreset`, `park`, `release`, `stop`, and without an axis `saveConfig`
(`{config, revision}`) and `savePresets` (`{presets, revision}`). A save
whose `revision` no longer matches the configuration file (another tab saved,
Set Home stored a homing direction, a backup was restored) is refused with
409.

## Files and backup

- **Configuration:** `config/plugin.fpp-StepperMotion.json`, included in
  FPP's backup. fppd validates and writes it when you save; a restored or
  hand-edited file is picked up while fppd runs (tuning at once, stepper
  changes after a restart).
- **Stored positions:** `plugindata/fpp-StepperMotion/axis-<name>.json`, one
  per stepper, deliberately **not** in the backup: they describe where this
  box's motors are, so a configuration restored onto another box starts with
  every stepper needing a home. Renaming a stepper carries its position to
  the new name.
- An update or a Reinstall keeps both; uninstalling removes both and the
  channel output entry.

## Privacy

Runs on the device only and sends nothing off it. The one change outside its
own files (the channel output entry) is declared in `pluginInfo.json`. It
never edits `/boot/firmware/config.txt`.

## License

GPL-2.0, see [LICENSE](LICENSE).
