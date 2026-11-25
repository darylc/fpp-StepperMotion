<?php
/*
 * F1 help for the Stepper Motion page.
 *
 * FPP's config.php sets helpPage to plugin.php?plugin=<name>&nopage=1&page=help/<page>
 * for any plugin page, so this file is what F1 (and the ? in the menu) loads
 * into the help dialog. Same conventions as www/help/*.php: fragment only, no
 * <html>, h3 for the title and h4 per section.
 */
?>
<h3>Stepper Motion</h3>
<p>Moves stepper motors from your show. Each stepper follows one channel of the sequence, and
the channel's <em>value</em> picks a motion preset: value 10 might mean &ldquo;go to 50%&rdquo;
and value 20 &ldquo;sweep back and forth&rdquo;. <b>Value 0, or the show ending, sends the
stepper home.</b></p>
<p><b>Steppers can move when fppd starts:</b> one whose stored position is valid but away from
home drives home straight away. When fppd stops, every stepper's coils are released, so a sprung
or back-drivable mechanism may move while fppd is down; rehome it afterwards.</p>

<h4>Quick start</h4>
<ol>
  <li><b>Wire one board</b> to SPI0 (tables below): 3.3&nbsp;V, ground, the three data lines,
      chip select on header pin 24, <code>DRV_ENN</code> and <code>CLK16</code> to ground, motor
      and motor supply with the bulk capacitor fitted.</li>
  <li><b>Add stepper</b>. It gets channel slot 1 and chip select pin 24. Check the driver type
      under Global settings, <b>Save configuration</b>, and restart fppd when FPP asks.</li>
  <li><b>Set the motor current</b> (below), then <b>Probe</b>: it should find the chip and report
      no problems.</li>
  <li><b>Commission</b> on the stepper's Commissioning tab: jog to the end you want as home,
      <b>Set Home</b>, jog to the other end, <b>Set Far Limit</b>. Mark the mechanism at
      both ends and watch <b>Test travel</b>: every cycle should meet both marks.</li>
  <li><b>Add a preset</b> (value 10 = <em>Go to</em> 50%, say) and try it with <b>Run now</b>.</li>
  <li><b>Sequence it</b>: put exactly 10 on the stepper's channel in your sequence.</li>
</ol>

<h4>Channels and presets</h4>
<p>The plugin listens on <b>8 consecutive channels</b>, one slot per possible stepper: the
<em>channel block</em>. Global settings &rsaquo; Channels sets where it starts, as a channel
number or as the first channel of a pixel overlay model (looked up again at every fppd
start). A stepper can read a channel of its own instead (its Channel source, under General).</p>
<p>A preset for <em>All steppers</em> means the same motion on any stepper; one scoped to a
single stepper is ignored by the others, which suits a <em>Cycle</em>, whose waypoints are full
steps on one mechanism. <b>Value 0 is reserved</b> for &ldquo;return home&rdquo;, as is the
channel going quiet for longer than the idle timeout. Returning home is a plain move to the
stored position 0; it never drives into the stop.</p>

<h4>Sequencing it</h4>
<ul>
  <li>Make one model covering the 8 channels, starting where Global settings &rsaquo; Channels
      says: the model's channel 1 is slot 1.
      Make all 8, so adding a stepper never moves another.</li>
  <li>Put <b>exactly the preset's value</b> (0&ndash;255) on a stepper's channel for as long as
      the motion should run, with an effect that sets a precise level. Turn off brightness,
      dimming curves and gamma for that model: 9 or 11 instead of 10 runs a different preset,
      or none.</li>
  <li><b>Check what arrives:</b> while the sequence plays, each stepper's line in the Steppers
      card shows <code>ch &lt;channel&gt; = &lt;value&gt;</code> and the preset it is running.</li>
  <li>While a show drives a stepper, its manual controls on this page are refused.</li>
</ul>

<h4>Motor current</h4>
<p>The <b>IRUN</b> slider shows the real RMS current beside it: <b>keep it at or below your
motor's rated current per phase</b>. Then mark the mechanism at both ends and watch <b>Test
travel</b> for ten cycles: missing a mark means lost steps at that current and speed. Use the lowest
current at which every cycle meets both marks, plus a little margin. <b>The plugin cannot see lost
steps</b>: there is no shaft sensor, and the step counter reads correct even when the motor stalls.</p>
<p><b>IHOLD flows all the time between moves</b> and is what overheats things; keep it low
unless the mechanism needs holding torque. A sprung mechanism does: with no current its shaft is
pulled away and the position is lost. <em>Hold for</em> (30&nbsp;s by default) releases the
coils that long after the last move.</p>

<h4>Commissioning</h4>
<ol>
  <li><b>Probe</b> checks the SPI link, that the chip is the driver selected, that
      <code>DRV_ENN</code> and <code>SD_MODE</code> are low, and shows the RMS current IRUN
      gives. <b>It also forgets the far limit</b> (position 0 is kept): after probing a
      commissioned stepper, jog to the far end and press Set Far Limit again.</li>
  <li><b>Jog</b> to the end you want as home. The driver cannot feel the stop (it counts the steps
      it commanded), so a jog into the stop just slips there: watch the mechanism and stop when it
      stops.</li>
  <li><b>Set Home</b> makes this position 0 and records the direction the stop was approached
      from as the homing direction.</li>
  <li>Jog to the other end and <b>Set Far Limit</b>. Percent-based presets use this range, and
      jogs are clamped to it from now on.</li>
  <li><b>Test travel</b> runs the range and back for you to watch against the marks; it checks only the step
      counter, which cannot see lost steps.</li>
</ol>
<p><b>Rehome</b> finds position 0 again by itself: it hits the stop once at speed, backs off,
then creeps back in slowly and zeroes on that contact. <b>Home</b> is a plain move to position 0.</p>

<h4>When the position is lost</h4>
<p>Position is open-loop: there is no sensor on the shaft. The plugin cross-checks its stored
position against the driver chip and notices a driver reset; if either disagrees the position is
marked lost, and the stepper refuses to go to a position until it is rehomed. With <em>Auto-recover
on demand</em> (on by default) that rehome happens by itself before the next move, giving up after
a few failed attempts. A stepper still moving when fppd stops is marked lost too.
<em>Rehome at startup</em> (off by default) rehomes at every fppd start, for a prop that should
home itself at power-on.</p>

<h4>Troubleshooting</h4>
<table class="table table-sm">
  <thead><tr><th>Symptom</th><th>Likely cause and fix</th></tr></thead>
  <tbody>
    <tr><td>Probe: version byte <code>0x00</code></td><td>No motor supply on VS (the chip runs off it), or the chip-select or SDO wire is off.</td></tr>
    <tr><td>Probe: <code>0xFF</code>, or reads that come and go</td><td>Check SCK/SDI/SDO and ground; make sure nothing else in FPP (GPIO output, pixel string, cape) uses the bus's data pins; lower <em>Advanced &rsaquo; SPI speed</em>.</td></tr>
    <tr><td>Probe: another version byte</td><td>Wrong driver selected: TMC5160 reads <code>0x30</code>, TMC5130 <code>0x11</code>.</td></tr>
    <tr><td>Probe: <code>DRV_ENN is high</code></td><td>Tie <code>DRV_ENN</code> to ground.</td></tr>
    <tr><td>Probe: <code>SD_MODE high</code></td><td>The board's R1/R2 jumper must have R2 fitted (factory default).</td></tr>
    <tr><td>&ldquo;open load on both coils&rdquo;</td><td>No motor connected.</td></tr>
    <tr><td>A stepper on SPI1 refuses to start</td><td>No <code>/dev/spidev1.0</code> yet: reboot once after upgrading FPP.</td></tr>
    <tr><td>A stepper on pin 24 or 26 refuses to start</td><td>An older image upgraded in place: pick another chip-select pin, or add <code>dtoverlay=spi0-0cs</code> to <code>/boot/firmware/config.txt</code> and reboot.</td></tr>
    <tr><td>Corrupted reads or a hung chip at higher current</td><td>Fit the bulk capacitor at VS; short, heavy supply leads.</td></tr>
    <tr><td>Test travel misses a mark</td><td>Lost steps: raise IRUN, lengthen the ramp, or lower the speed.</td></tr>
    <tr><td>&ldquo;position doesn't match&rdquo; / &ldquo;the driver chip reset&rdquo;</td><td>Position lost (power drop, or the shaft was moved): Rehome.</td></tr>
    <tr><td>Steppers ignore the show</td><td>Read the warning at the top of the page, if any (output entry off or missing); check the value arriving; check a preset exists for it.</td></tr>
  </tbody>
</table>
<p>The plugin logs to fppd's log, under the <em>Plugin</em> category.</p>

<h4>Wiring a TMC5130A-BOB / TMC5160-BOB</h4>
<p>Both boards share one 22-pin header (left 1&ndash;11, right 12&ndash;22, pin 1 marked). A
bus's three data lines are shared by every board on it; each board has its own chip select, any
free GPIO pin of the header (set under the stepper's General section). New steppers get these
pins in turn; Global settings &rsaquo; SPI configuration draws the table for what is configured.
Each cell is GPIO / header pin.</p>
<table class="table table-sm w-auto">
    <thead><tr><th>Default</th><th>Bus</th><th>CS &rarr; CSN (BOB 3)</th><th>MOSI &rarr; SDI (BOB 5)</th><th>MISO &larr; SDO (BOB 6)</th><th>SCLK &rarr; SCK (BOB 4)</th></tr></thead>
    <tbody>
        <tr><td>1</td><td>SPI0</td><td>GPIO8 / 24</td><td>GPIO10 / 19</td><td>GPIO9 / 21</td><td>GPIO11 / 23</td></tr>
        <tr><td>2</td><td>SPI0</td><td>GPIO7 / 26</td><td>GPIO10 / 19</td><td>GPIO9 / 21</td><td>GPIO11 / 23</td></tr>
        <tr><td>3</td><td>SPI0</td><td>GPIO25 / 22</td><td>GPIO10 / 19</td><td>GPIO9 / 21</td><td>GPIO11 / 23</td></tr>
        <tr><td>4</td><td>SPI0</td><td>GPIO24 / 18</td><td>GPIO10 / 19</td><td>GPIO9 / 21</td><td>GPIO11 / 23</td></tr>
        <tr><td>5</td><td>SPI1</td><td>GPIO18 / 12</td><td>GPIO20 / 38</td><td>GPIO19 / 35</td><td>GPIO21 / 40</td></tr>
        <tr><td>6</td><td>SPI1</td><td>GPIO17 / 11</td><td>GPIO20 / 38</td><td>GPIO19 / 35</td><td>GPIO21 / 40</td></tr>
        <tr><td>7</td><td>SPI1</td><td>GPIO16 / 36</td><td>GPIO20 / 38</td><td>GPIO19 / 35</td><td>GPIO21 / 40</td></tr>
        <tr><td>8</td><td>SPI1</td><td>GPIO27 / 13</td><td>GPIO20 / 38</td><td>GPIO19 / 35</td><td>GPIO21 / 40</td></tr>
    </tbody>
</table>
<table class="table table-sm w-auto">
    <thead><tr><th>BOB pin</th><th>Signal</th><th>Connect to</th><th>Notes</th></tr></thead>
    <tbody>
        <tr><td>1</td><td>VCC_IO</td><td>Pi 3.3&nbsp;V (header 1 or 17)</td><td><b>3.3&nbsp;V, not 5&nbsp;V</b></td></tr>
        <tr><td>2, 7</td><td>GND</td><td>Pi ground (6, 9, 14, 20, 25, 30, 34, 39)</td><td></td></tr>
        <tr><td>8</td><td>CLK16</td><td>Ground</td><td>Internal clock</td></tr>
        <tr><td>9</td><td>DRV_ENN</td><td>Ground</td><td>Floating or high: the motor never moves</td></tr>
        <tr><td>10, 11</td><td>REFL, REFR</td><td>Nothing (or ground)</td><td>Ignored</td></tr>
        <tr><td>12</td><td>VS</td><td>Motor supply +</td><td>Bulk capacitor here</td></tr>
        <tr><td>13, 18</td><td>GND</td><td>Motor supply &minus;</td><td></td></tr>
        <tr><td>14&ndash;17</td><td>B2, B1, A2, A1 (5160) / OA1, OA2, OB1, OB2 (5130A)</td><td>Motor coils</td><td>Swap one pair to reverse</td></tr>
        <tr><td>19&ndash;22</td><td>DIAG0 / AIN_IREF, ENCA, ENCB, ENCN</td><td>Nothing</td><td></td></tr>
    </tbody>
</table>
<ul>
    <li><b>Bulk capacitor:</b> fit the 100&ndash;220&nbsp;&micro;F electrolytic the board has a
        footprint for, right at VS. Without it, motor current above about 0.7&nbsp;A on jumper
        leads corrupts the SPI link and can hang the chip.</li>
    <li><b>The pins a running stepper uses are fppd's:</b> its bus's data lines (SPI0 19, 21, 23;
        SPI1 35, 38, 40) and its chip select. Don't configure them for FPP GPIO or pixel strings;
        FPP's GPIO page does not know about them.</li>
    <li><b>SPI1 uses the I&sup2;S pins</b>, so it rules out an I&sup2;S audio HAT or DAC. SPI1
        needs FPP's <code>fpp-spi1-nopins</code> overlay, which upgrading FPP adds; reboot once
        after the upgrade.</li>
    <li><b>Keep SPI leads short:</b> four boards per bus on flying leads is the comfortable
        limit.</li>
    <li><b>Other TMC5130/5160 boards are untested.</b> They need SPI mode with the internal ramp
        generator; many 3D-printer modules are set up for step/direction. A different sense
        resistor or clock goes under the stepper's Advanced settings.</li>
</ul>

<h4>Settings</h4>
<p>Each stepper inherits every setting from <b>Global settings</b> unless it overrides it: an
empty field, a &ldquo;(global &hellip;)&rdquo; choice or an override switch left off follows the
global value, shown greyed. The register-level settings and the rehome staging are global only.
Tuning values apply as soon as you save; changes to the steppers themselves (adding, removing,
bus, chip select, driver) need the fppd restart FPP prompts for. Turning <b>Enabled</b> off keeps
a stepper configured while fppd leaves its board alone.</p>
<p>The configuration is saved in <code>config/plugin.<?php echo htmlspecialchars(basename(dirname(__DIR__))); ?>.json</code>,
which FPP's backup includes; a restored file is picked up while fppd runs. The plugin keeps its
channel output entry (Channel Outputs, <em>Misc</em> tab) in step with the channel block; a
deleted entry comes back at the next fppd start or save, and this page warns while it is missing
or off.</p>

<h4>Commands</h4>
<p><em>Stepper Motion Rehome</em>, <em>Go To Position</em>, <em>Park</em>, <em>Release</em> and
<em>Stop</em> drive steppers from playlists, the scheduler, events and MQTT without a channel.</p>
