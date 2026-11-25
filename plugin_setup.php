<?php
/*
 * Stepper Motion - setup, commissioning and tuning.
 *
 * One page on purpose: stepper configuration, the commissioning wizard, the
 * tuning panel, the preset editor and the help all live here, so the plugin
 * takes a single entry in FPP's Output menu (PLUGIN_GUIDELINES.md #9.1).
 *
 * This file is the page shell plus the stepper form. The form is static
 * markup: one pane per possible stepper (MAX_AXES, read from
 * src/AxisConfig.h), emitted by the loop at the bottom and shown or hidden
 * by assets/bind.js, which fills every control from the configuration and
 * reads it back. Each control carries data-path (its place in config.json,
 * e.g. "motor.irun"); a row that only applies sometimes carries
 * data-showif="path=value". The Global settings form above the panes holds
 * the SPI configuration (the wiring table and which buses this box has) and
 * the defaults block, whose fields a pane may override (see "inherited
 * fields" below). A stepper's SPI bus and chip-select pin are its own, two
 * selects in its General section. The min/max/step attributes are hints
 * only -- the daemon validates on save.
 *
 * The page loads and saves through the plugin API (?action=config,
 * saveConfig, savePresets), so it needs fppd running; it says so if not.
 *
 * Why assets/ and not js/ + css/: www/plugin.php auto-injects every file in
 * a plugin's js/ and css/ directories in readdir() order, and these modules
 * must load in dependency order, so the tags are emitted by hand from a
 * directory FPP does not sweep.
 */
// The plugin's directory name (its repoName): what www/plugin.php validated
// from ?plugin= before including this page.
$tmcPlugin = (isset($pluginName) && $pluginName !== '') ? $pluginName : basename(__DIR__);
// Apache exposes plugin routes only under /api/plugin-apis/, which it
// rewrites onto fppd stripping that whole prefix.
$tmcApi = 'api/plugin-apis/stepper-motion';
// How many steppers the daemon accepts (MAX_AXES in src/AxisConfig.h): the
// pane count below and TMC.MAX_AXES for the page. The literal is only the
// fallback for a build where the source is not shipped.
function tmcMaxAxes()
{
    $h = @file_get_contents(__DIR__ . '/src/AxisConfig.h');
    if ($h !== false && preg_match('/constexpr\s+int\s+MAX_AXES\s*=\s*(\d+)\s*;/', $h, $m)) {
        return max(1, intval($m[1]));
    }
    return 8;
}
$tmcMaxAxes = tmcMaxAxes();
// Static assets go through plugin.php's file= route. Cache-busted on mtime so
// a deployed change is picked up without a hard refresh.
function tmcAsset($file)
{
    global $tmcPlugin;
    $path = __DIR__ . '/assets/' . $file;
    $v = file_exists($path) ? filemtime($path) : 0;
    return 'plugin.php?plugin=' . $tmcPlugin . '&file=assets/' . $file
        . '&nopage=1&v=' . $v;
}

// ------------------------------------------------------ form helpers --
// The same help-icon markup PrintToolTip() emits in www/common.php.
// Tooltips are initialised by assets/core.js after load.
function tmcHelp($tip)
{
    if ($tip === '') {
        return '';
    }
    $t = htmlspecialchars($tip, ENT_QUOTES | ENT_HTML5, 'UTF-8', false);
    return '<span class="tmcHelpIcon" data-bs-html="true" data-bs-placement="auto"'
        . ' data-bs-title="' . $t . '">'
        . '<img src="images/redesign/help-icon.svg" class="icon-help" alt="help icon"></span>';
}
// FPP's label/field row: see PrintSetting() in www/common.php.
function tmcRow($label, $help, $field, $showIf = '', $note = '')
{
    if ($note !== '') {
        $field .= '<div class="form-text">' . $note . '</div>';
    }
    return '<div class="row"' . ($showIf !== '' ? ' data-showif="' . $showIf . '"' : '') . '>'
        . '<div class="printSettingLabelCol col-md-4 col-lg-3"><b>' . $label . ':</b>' . tmcHelp($help) . '</div>'
        . '<div class="printSettingFieldCol col-md-8 col-lg-9">' . $field . '</div></div>';
}
function tmcNum($path, $attrs = '')
{
    return '<input type="number" class="form-control" data-path="' . $path . '" ' . $attrs . '>';
}
function tmcText($path)
{
    return '<input class="form-control" data-path="' . $path . '">';
}
function tmcSelect($path, $options)
{
    $o = '';
    foreach ($options as $v => $label) {
        $o .= '<option value="' . $v . '">' . $label . '</option>';
    }
    return '<select class="form-select" data-path="' . $path . '">' . $o . '</select>';
}
function tmcSwitch($path)
{
    return '<div class="form-check form-switch"><input class="form-check-input" type="checkbox" role="switch" data-path="'
        . $path . '"></div>';
}
// IRUN/IHOLD: a 0..31 slider with the real RMS current beside it (bind.js
// keeps the badge current; its id is what refreshAmps() writes).
function tmcRange($path, $badgeId)
{
    return '<div class="d-flex align-items-center gap-2">'
        . '<input type="range" class="form-range" min="0" max="31" step="1" data-path="' . $path . '" data-amps="' . $badgeId . '">'
        . '<span class="badge text-bg-secondary" id="' . $badgeId . '"></span></div>';
}
// One collapsible section of a stepper's pane. The header carries a summary
// of what is inside, so a shut section still tells you what it holds.
function tmcSection($i, $key, $title, $body)
{
    $id = 'tmcSec' . $i . $key;
    return '<div class="card mb-3 tmc-section" data-section="' . $key . '"><div class="card-header">'
        . '<button class="btn btn-link tmc-section-toggle collapsed" type="button" data-bs-toggle="collapse" data-bs-target="#'
        . $id . '" aria-expanded="false"><i class="fas fa-chevron-down"></i><span class="fw-semibold">' . $title . '</span>'
        . '<span class="tmc-section-summary small text-body-secondary" id="tmcSum' . $i . $key . '"></span></button></div>'
        . '<div id="' . $id . '" class="collapse"><div class="card-body">' . $body . '</div></div></div>';
}
// The "Advanced settings" disclosure inside a section: register-level and
// simulator-only fields, set once if ever. Always starts shut.
function tmcAdvanced($id, $body)
{
    return '<div class="tmc-advanced mt-2"><button class="btn btn-link btn-sm ps-0 tmc-adv-toggle" type="button" '
        . 'data-bs-toggle="collapse" data-bs-target="#' . $id . '" aria-expanded="false">'
        . '<i class="fas fa-chevron-right"></i> Advanced settings</button>'
        . '<div id="' . $id . '" class="collapse"><div class="container-fluid settingsTable pt-2">' . $body . '</div></div></div>';
}
function tmcTable($rows)
{
    return '<div class="container-fluid settingsTable">' . $rows . '</div>';
}

// ------------------------------------------------- inherited fields --
// Most settings are the same for every stepper, so they live in the
// "defaults" block (the Global settings form) and a stepper's pane only
// carries an override: an empty input, the "(global: ...)" driver option
// or an unticked override switch means inherit, with the effective value
// as the placeholder (bind.js). The rows below are written once and drawn
// in both forms with $scope telling which: 'global' (every field, plain
// controls) or 'stepper' (only the inheritable fields, as overrides). fclk
// and rsense derive from the effective driver when neither form sets them.
function tmcInherit($path, $control)
{
    return $control . '<div class="form-text tmc-inherit-hint" data-inherit="' . $path . '"></div>';
}
// A number input that inherits when empty (stepper form) or is plain
// (global form); $derived marks fclk/rsense, empty in the global form too.
function tmcNumRow($scope, $path, $label, $help, $attrs, $note = '', $showIf = '', $derived = false)
{
    if ($scope === 'stepper' || $derived) {
        return tmcRow($label, $help, tmcInherit($path, tmcNum($path, $attrs)), $showIf, $note);
    }
    return tmcRow($label, $help, tmcNum($path, $attrs), $showIf, $note);
}
// IRUN/IHOLD: a slider cannot be empty, so in a stepper's pane it carries an
// override switch instead -- off, it shows the global value and is inert.
function tmcRangeRow($scope, $i, $path, $label, $help, $badge, $note = '')
{
    $control = tmcRange($path, $badge . $i);
    if ($scope === 'stepper') {
        $control = '<div class="d-flex align-items-center gap-2 flex-wrap">'
            . '<div class="form-check form-switch mb-0 tmcTip" data-bs-title="Override the global setting for this stepper">'
            . '<input class="form-check-input" type="checkbox" role="switch" data-override="' . $path . '"></div>'
            . '<div class="flex-grow-1">' . $control . '</div></div>';
        $control = tmcInherit($path, $control);
    }
    return tmcRow($label, $help, $control, '', $note);
}

// The Motor, current & ramp rows of either form.
function tmcMotorRows($scope, $i)
{
    $g = $scope === 'global';
    $driverOpts = array('TMC5130' => 'TMC5130', 'TMC5160' => 'TMC5160');
    if (!$g) {
        // The first option means inherit; bind.js writes the global chip
        // into its label.
        $driverOpts = array('' => '(global)') + $driverOpts;
    }
    $rows = tmcRow('Driver', 'Which TMC driver chip is wired up. Sets the current-sense formula and, unless overridden under Advanced settings, the clock and R<sub>sense</sub> of that chip&rsquo;s BOB.'
            . ($g ? ' Every stepper uses this chip unless its own pane says otherwise.' : ' Leave on the global choice unless this stepper is on a different board.'),
            $g ? tmcSelect('driver', $driverOpts) : tmcInherit('driver', tmcSelect('driver', $driverOpts)))
        . tmcRangeRow($scope, $i, 'motor.irun', 'Run current (IRUN)', 'Motor current while moving. Lower is cooler; too low loses steps, which nothing reports &mdash; watch the mechanism against marks during Test travel in Commissioning. Commonly 8&ndash;24 for small NEMA steppers on these boards; start low and raise until every cycle meets the marks.',
            'irunAmps')
        . tmcRangeRow($scope, $i, 'motor.ihold', 'Hold current (IHOLD)', 'Motor current while holding position between moves. It flows continuously after every move, so this is what overheats a bare driver board. Commonly 4&ndash;10, well below IRUN.',
            'iholdAmps', 'Flows continuously after every move. This is what overheats a bare driver board &mdash; raise it only if you need holding torque, and add cooling if you do.');
    $holdHelp = 'Keep powering the coils this long after the last move, then release them. 0 releases at once; hold indefinitely is needed on a sprung or back-drivable mechanism, where a released shaft moves and the stored position goes silently wrong. Also applies after a failed return home. The coils are always released when fppd stops, whatever this says.';
    if ($g) {
        $rows .= tmcRow('Hold for (s)', $holdHelp . ' 30 s unless set otherwise; leave empty to hold indefinitely.',
            tmcNum('holdSeconds', 'min="0" max="86400" step="1" placeholder="hold indefinitely"'));
    } else {
        // Empty already means "hold indefinitely" here, so inherit has to be
        // an explicit choice: a mode select, with the seconds beside it.
        $rows .= tmcRow('Hold for', $holdHelp . ' <b>Global</b> follows the global setting; the other two override it for this stepper.',
            tmcInherit('holdSeconds',
                '<div class="d-flex align-items-center gap-2 flex-wrap">'
                . '<select class="form-select w-auto" data-path="holdSeconds" data-hold="mode">'
                . '<option value="">(global)</option><option value="hold">Hold indefinitely</option><option value="n">Release after</option></select>'
                . '<input type="number" class="form-control w-auto d-none" min="0" max="86400" step="1" data-path="holdSeconds" data-hold="secs" placeholder="seconds">'
                . '</div>'));
    }
    $rows = tmcTable($rows)
        . '<div class="alert alert-secondary py-2 small"><strong>Ramp time, not speed, is what makes a short move faster.</strong> A move of a few full steps never reaches the speed it was asked for &mdash; it is all ramp up and ramp down &mdash; so raising Speed does nothing for it. Shortening the ramp does. At 0.15s and 2 rev/s the ramp alone needs about 30 full steps to get up to speed, which on a mechanism with a few dozen full steps of travel means very nearly every move is ramp-limited.<div class="mt-1">What it costs is torque margin. Too short a ramp and the motor loses steps, and <strong>there is no sensor here to notice</strong>: nothing reports it, the move still looks like it worked, and the stored position stays wrong until the next Rehome. Shorten it a little at a time and watch that <em>Test travel</em> still meets your marks at both ends.</div></div>'
        . tmcTable(
            tmcNumRow($scope, 'motion.rampTimeSec', 'Ramp time (s)', 'How long a move is given to reach its speed. Acceleration is worked out from this and the speed actually requested, so a faster preset gets a steeper ramp instead of being quietly capped by a fixed acceleration. Commonly 0.05&ndash;0.3s; 0.02&ndash;5 is accepted.',
                'min="0.02" max="5" step="0.01"',
                'Applies to presets, cycles, Go to, park, jog and Test travel. Homing does <b>not</b> use it &mdash; it has its own acceleration under Rehome staging, deliberately kept separate. Individual presets may override this in the preset editor.')
        );
    $adv = tmcNumRow($scope, 'motor.rsense', 'R<sub>sense</sub> (&#8486;)', 'Sense resistor value on the driver board, from its datasheet. Used in the current formula. Commonly 0.05&ndash;0.22&#8486; across breakout boards &mdash; stock value is 0.15&#8486; on a TMC5130-BOB, 0.075&#8486; on a TMC5160-BOB, which is what an empty field means for the chosen driver. Only fill it in for a board with a different resistor.',
            'min="0.001" step="0.001"', '', '', true)
        . tmcNumRow($scope, 'motor.fclk', 'f<sub>CLK</sub> (Hz)', 'The driver chip&rsquo;s internal clock, used to convert speeds and accelerations into register values. Empty means the datasheet-typical internal oscillator for the chosen driver &mdash; 13.2MHz on a TMC5130, 12MHz on a TMC5160. Only fill it in to match a clock actually wired to CLK.',
            'min="1000000" max="30000000" step="100000"', '', '', true);
    if ($g) {
        $adv .= tmcRow('IHOLDDELAY', 'How many steps of delay before IRUN decays to IHOLD after a move stops &mdash; 0&ndash;15, higher is a slower, gentler ramp down. Commonly 4&ndash;8; the default of 6 suits most setups.',
                tmcNum('motor.iholdDelay', 'min="0" max="15" step="1"'))
            . tmcRow('CHOPCONF', 'Chopper configuration register, in hex. Controls microstep interpolation and chopper mode. No general-purpose "typical range" here &mdash; it is a 32-bit register best left at the shipped default, 0x000100C3, unless you are working from the datasheet chapter on it.',
                tmcText('motor.chopconf'))
            . tmcRow('vsense', 'Selects the driver&rsquo;s low-sensitivity current range. Only matters at very low IRUN/IHOLD; leave off unless the datasheet says otherwise.',
                tmcSwitch('motor.vsense'))
            . tmcRow('GLOBALSCALER', 'TMC5160 only: extra current scaling on top of IRUN/IHOLD, 32-256. Typically left at 256 (full scale); rarely lowered below ~128. Written to the chip\'s GLOBALSCALER register.',
                tmcNum('motor.globalScaler', 'min="32" max="256" step="1"'), 'driver=TMC5160')
            . tmcRow('SPI speed (Hz)', 'SPI clock rate to the driver chips. Commonly 500kHz&ndash;4MHz over short jumper wiring; the 1MHz default has margin to spare.',
                tmcNum('spi.speedHz', 'min="100000" step="100000"'));
    }
    return $rows . tmcAdvanced('tmcAdvMotor' . $i, $adv);
}

// The Rehome staging rows of either form. The stepper form has only the
// direction (its own) and the four inheritable speed/current settings.
function tmcRehomeRows($scope, $i)
{
    $g = $scope === 'global';
    $rows = '';
    if ($g) {
        $rows .= tmcRow('Auto-recover on demand', 'When something asks a stepper to move and its position reference has been lost (a driver reset, a stall the chip caught, or the SPI link dropping out), rehome automatically instead of just refusing the move. Only fires while survey() reports the driver otherwise healthy, and only in response to an actual move request &mdash; never on a timer.',
                tmcSwitch('rehome.autoRecoverOnDemand'))
            . tmcRow('Give up after (attempts)', 'How many consecutive automatic rehome attempts to allow before leaving the reference lost and waiting for a manual Rehome, so a chip that keeps resetting does not grind into the stop forever. Resets the moment any reference is established. Commonly 2&ndash;3.',
                tmcNum('rehome.autoRecoverMaxAttempts', 'min="1" max="10" step="1"'))
            . tmcRow('Rehome at startup', 'Also rehome once, unattended, right after fppd starts a stepper, if there is no valid reference already &mdash; for a prop that is expected to be at a known position before the show can use it. Off by default: unlike the setting above, this can move the shaft with nobody having asked for it this time.',
                tmcSwitch('rehome.autoRecoverAtStartup'));
    } else {
        $rows .= tmcRow('Homing direction', 'Which way Rehome turns to find the stop. While a home is recorded, Rehome follows the direction that home was actually approached in; this setting is what it falls back on once that is lost. Set Home works the direction out from the way you jogged into the stop and saves it here, and Set Far Limit offers to flip it if the far end turns out to be on the wrong side of home.',
            tmcSelect('rehome.dir', array('ccw' => 'Counter Clockwise', 'cw' => 'Clockwise')));
    }
    $rows .= tmcNumRow($scope, 'rehome.coarseRps', 'Coarse rev/s', 'Speed of the first, fast approach to the stop. Commonly 0.1&ndash;0.5 rev/s &mdash; fast enough to reach the stop quickly, slow enough the impact is not punishing.',
            'min="0.01" step="0.01"')
        . tmcNumRow($scope, 'rehome.coarseIrun', 'Coarse IRUN', 'Motor current during the fast approach. Commonly 8&ndash;16 &mdash; enough torque to reach the stop reliably at speed.',
            'min="0" max="31" step="1"');
    if ($g) {
        $rows .= tmcRow('Back off (steps)', 'How far to back off the stop after the fast approach, before creeping back in slowly. Commonly 2&ndash;10 full steps &mdash; just enough to clear stop-bounce.',
            tmcNum('rehome.backoffSteps', 'min="1" step="1"'));
    }
    $rows .= tmcNumRow($scope, 'rehome.creepRps', 'Creep rev/s', 'Speed of the final slow approach that actually zeroes position 0. Commonly 0.01&ndash;0.05 rev/s &mdash; slow enough that a few extra full steps of overtravel do not matter.',
            'min="0.01" step="0.01"')
        . tmcNumRow($scope, 'rehome.creepIrun', 'Creep IRUN', 'Motor current during the final slow approach. Commonly 4&ndash;10 &mdash; just enough to register contact without slamming.',
            'min="0" max="31" step="1"');
    if (!$g) {
        return tmcTable($rows);
    }
    $rows .= tmcRow('Creep overtravel (steps)', 'How far the final creep is allowed to travel looking for the stop. Commonly 5&ndash;20, and always more than Back off above.',
        tmcNum('rehome.creepOvertravelSteps', 'min="2" step="1"'), '',
        'Must exceed the back-off above, or the creep never reaches the stop and the rehome fails.');
    // When a stepper goes home on its own: plugin-wide (config.json's
    // top-level idleTimeoutMs, not the defaults block), so no data-path --
    // main.js reads and writes it by id.
    $rows .= tmcRow('Idle timeout (ms)', 'How long with no output frames before a stepper decides the show has stopped driving it and returns home on its own. Shared by every stepper, not per-stepper.',
        '<input type="number" min="100" max="60000" step="100" class="form-control" style="max-width:12rem" id="tmcIdleTimeoutMs" value="500">');
    return '<div class="alert alert-warning py-2 small" data-showif="rehome.autoRecoverAtStartup=true"><strong>Every stepper will rehome the moment fppd starts, with nobody watching.</strong> Only turn this on for props that are expected to home themselves at power-on.</div>'
        . tmcTable($rows)
        . tmcAdvanced('tmcAdvRehome' . $i,
            tmcRow('Coarse overtravel (&deg;)', 'How far the fast approach is allowed to travel, in degrees, looking for the stop before giving up. Commonly 5&ndash;30&deg;.',
                tmcNum('rehome.coarseOvertravelDeg', 'min="0.1" step="0.1"'))
            . tmcRow('Coarse IHOLD', 'Hold current during the fast-approach stage, used only if it pauses to settle. Commonly 2&ndash;6, near your normal IHOLD.',
                tmcNum('rehome.coarseIhold', 'min="0" max="31" step="1"'))
            . tmcRow('Back off rev/s', 'Speed of the deliberate reversal off the stop before creeping back in. It is a short, controlled move rather than a stop-detection one, so it can run faster than Creep. Commonly 0.02&ndash;0.1 rev/s.',
                tmcNum('rehome.backoffRps', 'min="0.01" step="0.01"'))
            . tmcRow('Creep IHOLD', 'Hold current during the final slow-approach stage, used only if it pauses to settle. Commonly 2&ndash;6, near your normal IHOLD.',
                tmcNum('rehome.creepIhold', 'min="0" max="31" step="1"'))
            . tmcRow('Settle time (ms)', 'Pause between homing stages, giving mechanical ringing from the last move time to die out before the next one starts. Commonly 200&ndash;1000ms.',
                tmcNum('rehome.settleMs', 'min="0" max="5000" step="50"'))
            . tmcRow('Homing acceleration (AMAX)', 'Acceleration for all three homing stages, as the driver&rsquo;s raw AMAX register value. It is an acceleration and not a ramp time like Motor, current &amp; ramp&rsquo;s, because the coarse, back-off and creep stages run at speeds an order of magnitude apart and were tuned against one gentle acceleration shared by all of them. 200 is the proven default; 10&ndash;2000 is accepted.',
                tmcNum('rehome.amax', 'min="10" max="2000" step="1"'), '',
                'Raise this with care. A homing ramp that loses steps still ends somewhere, and that somewhere is recorded as home with no error &mdash; an open-loop stepper cannot tell a stop it reached from one it fell short of. A wrong home reference is far worse than a slow rehome.')
        );
}

// ------------------------------------------------- the global form --
// Three collapsible sections: the SPI configuration (the wiring table drawn
// live by bind.js renderSpi from every stepper's bus and chip select, and
// which buses this box has), then the defaults block as two sections with
// the same rows the stepper panes have, drawn plain. Section ids use 'g'
// where a pane uses its index, so bind.js's summary helpers work on all of
// them.
function tmcSpiRows()
{
    return '<div class="alert alert-secondary py-2 small"><strong>Each stepper names its own bus and chip-select pin</strong> under its General section; this table is the wiring that implies. The plugin drives chip select itself, from the pin you pick, so any free GPIO pin of the header will do and nothing has to be enabled at boot: SPI0 works on a stock FPP image. The three data lines of a bus are shared by every board on it. New steppers are given the next pin of the default wiring (SPI0: 24, 26, 22, 18; SPI1: 12, 11, 36, 13), which is the table in the README and the F1 help.</div>'
        // One complete row per stepper (chip select plus the bus's three
        // shared lines, BOB pin names beside the Pi's), so a board can be
        // wired from its row alone.
        . '<div class="tmc-wiring-wrap"><table class="table table-sm w-auto mb-2" id="tmcSpiWiring"><thead><tr><th>Stepper</th><th>Bus</th><th>CS &rarr; CSN</th><th>MOSI &rarr; SDI</th><th>MISO &larr; SDO</th><th>SCLK &rarr; SCK</th></tr></thead><tbody></tbody></table></div>'
        . '<div class="form-text mb-2">Each cell is header pin / GPIO. CSN is each board&rsquo;s own; the other three are the bus&rsquo;s, shared.<span id="tmcSpiBuses"></span></div>'
        . '<div class="form-text mb-1" id="tmcSpiKernel"></div>'
        . '<div class="form-text text-warning d-none" id="tmcSpiStale"></div>';
}
// The plugin's channel block (config.json's top-level channelBlock, not the
// defaults block), so no data-path: main.js reads and writes it by id, the
// way it does the idle timeout.
function tmcChannelRows()
{
    global $tmcMaxAxes;
    $n = intval($tmcMaxAxes);
    return '<div class="alert alert-secondary py-2 small"><strong>The plugin reads a block of ' . $n . ' channels</strong>, one per possible stepper whether or not it is configured, so adding a stepper never moves another and the show side is one ' . $n . '-channel prop. Each stepper takes a slot in the block under its General section; a stepper that has to sit elsewhere in the show can have its own channel instead, chosen with its Channel source. The channel value is a preset number.</div>'
        . tmcTable(
            tmcRow('Block starts at', 'A channel number, or the first channel of a pixel overlay model. A model is looked up by name every time fppd starts, so the block follows it when a new upload moves the model&rsquo;s channels.',
                '<select class="form-select w-auto" id="tmcBlockSource"><option value="channel">A channel number</option><option value="model">A pixel overlay model</option></select>')
            . '<div id="tmcBlockStartRow">' . tmcRow('First channel', 'The channel slot 1 reads; slot ' . $n . ' reads ' . ($n - 1) . ' channels on.',
                '<input type="number" min="1" max="' . (8388608 - $n + 1) . '" step="1" class="form-control" style="max-width:12rem" id="tmcBlockStart" value="1">') . '</div>'
            . '<div id="tmcBlockModelRow" class="d-none">' . tmcRow('Model', 'The pixel overlay model whose first ' . $n . ' channels are the block. Make it ' . $n . ' channels in your sequencer.',
                '<select class="form-select" id="tmcBlockModel"></select>'
                . '<div class="form-text text-warning d-none" id="tmcBlockModelWarn"></div>') . '</div>'
        )
        . '<div class="form-text mb-2" id="tmcBlockInfo"></div>';
}
function tmcGlobalForm()
{
    $motor = '<div class="alert alert-secondary py-2 small"><strong>These apply to every stepper</strong> unless its own pane overrides them &mdash; a stepper&rsquo;s empty field or &ldquo;(global)&rdquo; choice means it follows what is set here. <strong>More current is not better.</strong> Current is torque: too little and the motor loses steps under load, too much and the driver and motor run hot for no gain. Mark the mechanism at both ends and watch <em>Test travel</em>: use the lowest current at which every cycle meets both marks, plus a little margin. IRUN only flows while moving; <strong>IHOLD flows continuously between moves</strong>, which is what actually cooks a bare driver board.<div class="mt-1">Both chips take the same 0&ndash;31 setting, but it means different amps on each: the figure beside each slider is the real RMS current for the driver, sense resistor, vsense and (on a TMC5160) GLOBALSCALER chosen here. A TMC5130 drives the motor from its own output stage and runs out of headroom around 1&nbsp;A RMS per phase; a TMC5160 switches external MOSFETs and will happily deliver far more than a small motor wants, so watch the amps, not the number.</div></div>'
        . tmcMotorRows('global', 'g');
    return '<div id="tmcGlobalForm">'
        . tmcSection('g', 'channels', 'Channels', tmcChannelRows())
        . tmcSection('g', 'spi', 'SPI configuration', tmcSpiRows())
        . tmcSection('g', 'motor', 'Driver, motor, current &amp; ramp', $motor)
        . tmcSection('g', 'rehome', 'Rehome staging', tmcRehomeRows('global', 'g'))
        . '</div>';
}

// ------------------------------------------------- the stepper pane --
function tmcStepperPane($i)
{
    $hardware = tmcTable(
        tmcRow('Name', '', tmcText('name'))
        . tmcRow('Enabled', 'Off keeps this stepper in the configuration, with its chip select, channel and settings, but fppd does not start it: nothing talks to its board, its chip-select pin is left alone and its channel is ignored. For a board or motor that is unplugged for now. Takes effect at the next fppd restart.',
            tmcSwitch('enabled'))
        . tmcRow('Channel source', 'A slot in the plugin&rsquo;s channel block, which is what almost every stepper wants, or this stepper&rsquo;s own channel somewhere else in the show: a plain channel number, or a pixel overlay model plus an offset. A model is looked up by name every time fppd starts, so it keeps working when a new upload moves the model&rsquo;s channels.',
            tmcSelect('channel.mode', array('slot' => 'Slot in the channel block', 'absolute' => 'Own channel number', 'model' => 'Own pixel overlay model')))
        // The slot select is filled by bind.js: each slot's channel, and
        // which other stepper holds it.
        . tmcRow('Channel slot', 'Which of the plugin&rsquo;s block of channels (Global settings &rsaquo; Channels) this stepper watches. Its value is a preset number. Each stepper needs its own slot.',
            '<select class="form-select w-auto" data-path="channel.slot"></select>', 'channel.mode=slot')
        . tmcRow('Channel', 'The sequence channel this stepper watches. Its value is a preset number.',
            tmcNum('channel.number', 'min="1" max="8388608" step="1"'), 'channel.mode=absolute')
        . tmcRow('Model', 'The pixel overlay model whose channels this stepper follows. The model says which channel range it occupies.',
            '<select class="form-select" data-path="channel.model"></select>'
            . '<div class="form-text text-warning d-none" id="tmcModelWarn' . $i . '"></div>', 'channel.mode=model')
        . tmcRow('Offset into model', 'Which channel within that model, counting from 0. Leave at 0 for the model&rsquo;s first channel.',
            tmcNum('channel.offset', 'min="0" step="1"'), 'channel.mode=model')
        // The bus and chip-select selects are filled by bind.js: the buses
        // this FPP knows (with whether the box has the device node) and the
        // header pins a chip select may be driven from.
        . tmcRow('SPI bus', 'Which of the Pi&rsquo;s SPI buses this board&rsquo;s SDI/SDO/SCK are wired to. SPI0 (header pins 19/21/23) works on any FPP image; SPI1 (pins 38/35/40, also the I&sup2;S pins) needs FPP&rsquo;s fpp-spi1-nopins overlay, which upgrading FPP adds, and one reboot, for /dev/spidev1.0 &mdash; the choice says whether this box has it.',
            '<select class="form-select w-auto" data-path="spi.bus"></select>'
            . '<div class="form-text text-warning d-none" id="tmcBusWarn' . $i . '"></div>', 'spi.fake=false')
        . tmcRow('Chip select', 'The header pin wired to this board&rsquo;s CSN (BOB pin 3). Any GPIO pin of the 40-pin header that is not one of the SPI data lines; each board needs its own. The plugin drives it while fppd runs, so keep it out of FPP&rsquo;s own GPIO and pixel-string configuration. Press F1 for the wiring guide.',
            '<select class="form-select w-auto" data-path="spi.cs"></select>', 'spi.fake=false')
    ) . tmcAdvanced('tmcAdvHw' . $i,
        tmcRow('Simulator', 'Runs this stepper against a built-in simulator instead of real silicon &mdash; useful for building presets and trying the wizard before anything is wired. A simulated stepper takes no bus or chip select.',
            tmcSwitch('spi.fake'))
    );

    $commissioning = '<div class="alert alert-secondary py-2 small mb-3">'
        . '<strong>Finding the travel range</strong> &mdash; same steps first time or any later time:'
        . '<ol class="mb-1 mt-1 ps-3">'
        . '<li><strong>Probe</strong> to confirm the driver answers. This also forgets the far limit, so jog is unclamped for the steps below.</li>'
        . '<li><strong>Jog</strong> with the buttons below until the mechanism is against the <em>home</em> stop. '
        . 'If the jog refuses to go further (&ldquo;stopped at the soft limit&rdquo;), press <strong>Probe</strong> again and jog again.</li>'
        . '<li><strong>Set Home</strong> &mdash; that spot becomes position 0 and the old far limit is forgotten. The homing direction is taken from the way you jogged into the stop (backing off a step first is fine) and saved.</li>'
        . '<li><strong>Jog</strong> the other way until the mechanism is at the <em>far</em> end (stop a step short of the far stop).</li>'
        . '<li><strong>Set Far Limit</strong> &mdash; that spot becomes the top of the range. If it is on the wrong side of home, you are offered to flip the homing direction.</li>'
        . '</ol>'
        . '<div>Then mark the mechanism at both ends and watch <em>Test travel</em>: the last cycle should meet the same marks as the first. <em>Rehome</em> should end at the home mark. '
        . 'Rehome only re-finds the stop within the range set here &mdash; if the range is wrong, redo the steps above.</div>'
        . '</div>'
        . '<div class="d-flex flex-wrap gap-2 mb-3">'
        . '<button class="buttons btn-primary tmcTip" data-act="probe" data-stepper="' . $i . '" '
        . 'data-bs-title="Confirms the driver answers over SPI, is the chip selected in General, and DRV_ENN/SD_MODE are wired low. Also forgets the far limit so jog is unclamped; position 0 is kept.">1. Probe</button>'
        . '<span class="align-self-center small text-body-secondary">&rarr; jog to home stop &rarr;</span>'
        . '<button class="buttons btn-primary tmcTip" data-act="setHome" data-stepper="' . $i . '" '
        . 'data-bs-title="Declares the current position full step 0, after jogging to the home stop. Also forgets the old far limit, so jog is unclamped until Set Far Limit runs again.">2. Set Home</button>'
        . '<span class="align-self-center small text-body-secondary">&rarr; jog to far end &rarr;</span>'
        . '<button class="buttons btn-primary tmcTip" data-act="setMax" data-stepper="' . $i . '" '
        . 'data-bs-title="Declares the current position the far end of travel, after jogging to the far end.">3. Set Far Limit</button>'
        . '</div>'
        . '<div class="mb-2 small text-body-secondary">Jog (full steps) &mdash; <span id="jogClamp' . $i . '">unclamped until Set Far Limit has run; clamped to the soft limits after that</span></div>'
        . '<div class="d-flex flex-wrap gap-2 mb-3">';
    foreach (array('-10', '-1', '-0.1', '0.1', '1', '10') as $d) {
        $commissioning .= '<button class="buttons btn-secondary" data-act="jog" data-stepper="' . $i . '" data-delta="' . $d . '">'
            . ($d[0] === '-' ? '&minus;' . substr($d, 1) : '+' . $d) . '</button>';
    }
    $commissioning .= '<button class="buttons btn-secondary tmcTip" data-act="release" data-stepper="' . $i . '" '
        . 'data-bs-title="De-energises the coils right now. The stored position reference is kept.">Release coils (stops powering)</button>'
        . '</div>'
        . tmcTable(
            tmcRow('Test cycles', 'How many full range-and-back cycles Test travel runs. Commonly 5&ndash;20 for a routine check; 50+ only when chasing an intermittent fault.',
                '<input type="number" min="1" max="100" class="form-control" id="cycles' . $i . '" value="5">')
            . tmcRow('Speed (rev/s)', 'Commonly 0.05&ndash;0.3 rev/s.',
                '<input type="number" step="0.01" min="0.01" max="4" class="form-control" id="rps' . $i . '" value="0.10">', '',
                'Used by Test travel and by the jog buttons above.')
        )
        . '<div class="d-flex flex-wrap gap-2 mt-2">'
        . '<button class="buttons btn-primary tmcTip" data-act="testTravel" data-stepper="' . $i . '" '
        . 'data-bs-title="Runs the full range and back so you can watch it against marks at both ends. There is no shaft sensor: only the step counter is checked, and that cannot see lost steps.">Test travel</button>'
        . '<button class="buttons btn-warning tmcTip" data-act="rehome" data-stepper="' . $i . '" '
        . 'data-bs-title="Re-finds the mechanical stop and re-zeroes position 0 there, using the Rehome staging values.">Rehome</button>'
        . '<button class="buttons btn-danger ms-auto tmcTip" data-act="stop" data-stepper="' . $i . '" '
        . 'data-bs-title="Aborts whatever is running right now -- a jog, a test travel, a rehome, a preset -- regardless of who started it."><i class="fas fa-stop"></i> Stop</button>'
        . '</div>'
        . '<div class="mt-2" id="commWarn' . $i . '"></div>'
        . '<div class="mt-3" id="job' . $i . '"></div>';

    $motor = '<div class="alert alert-secondary py-2 small"><strong>Everything here follows the Global settings above unless you override it</strong> &mdash; an empty field, the &ldquo;(global)&rdquo; choice or an unticked switch means this stepper uses the global value, shown greyed as the placeholder. Override only what is different about this stepper; the register-level settings (IHOLDDELAY, CHOPCONF, vsense, GLOBALSCALER, SPI speed) are global only.</div>'
        . tmcMotorRows('stepper', $i);

    $rehome = '<div class="alert alert-secondary py-2 small">The homing direction is this stepper&rsquo;s own. The speeds and currents below follow the Global settings unless filled in; the staging distances, settle time, acceleration and auto-recovery are global only.</div>'
        . tmcRehomeRows('stepper', $i);

    return tmcSection($i, 'hardware', 'General', $hardware)
        . tmcSection($i, 'commissioning', 'Commissioning', $commissioning)
        . tmcSection($i, 'motor', 'Motor, current &amp; ramp', $motor)
        . tmcSection($i, 'rehome', 'Rehome staging', $rehome);
}
?>
<link rel="stylesheet" type="text/css" href="<?php echo tmcAsset('tmc.css'); ?>">
<div class="container-fluid px-0" id="tmcRoot">

  <div id="tmcAlerts"></div>
  <!-- Says when FPP's channel output entry for the plugin (co-other.json, the
       Channel Outputs page's Misc tab) is off, missing or doubled: at the top,
       because the steppers then ignore the show. fppd keeps its channels in
       step with the block (TmcManager::syncOutputEntry). -->
  <div class="alert alert-warning py-2 mb-3 d-none" id="tmcOutputEntry"></div>

  <!-- d-none/d-flex are toggled by updateDirtyUi(), not the hidden attribute:
       Bootstrap's ".d-flex { display: flex !important; }" always overrides the
       browser's native "[hidden] { display: none; }" (no !important there), so
       a hidden d-flex element still renders -- this banner showed on every
       load regardless of configDirty until this was caught. -->
  <div class="alert alert-warning py-2 flex-wrap align-items-center gap-2 mb-3 d-none"
       id="tmcUnsaved">
    <span><strong>Unsaved changes.</strong> fppd is still running the last <em>saved</em> motor
    and rehome settings. Jog, Probe and Test travel use the values on this page; presets and
    the show use the last saved ones. Run now is disabled until you save, since a preset that
    doesn't override a setting falls back to these. (Presets themselves save instantly, as soon
    as their own dialog closes.)</span>
    <button type="button" class="buttons btn-warning btn-sm ms-auto" id="tmcUnsavedSave">
      <i class="fas fa-save"></i> Save now</button>
  </div>

  <!-- -------------------------------------------------------- steppers -->
  <!-- One card for what each stepper is doing AND the controls that change
       it, so which physical prop a click moves is never hidden state.
       Rows are rendered by assets/status.js. -->
  <div class="card mb-3">
    <!-- The all-steppers Stop/Home/Rehome/Release live in the header, filled
         by status.js only when there is more than one stepper. -->
    <div class="card-header d-flex flex-wrap gap-2 justify-content-between align-items-center">
      <span class="fw-semibold">Steppers</span>
      <div class="d-flex flex-wrap gap-1" id="tmcAllRow"></div>
    </div>
    <div class="card-body py-2" id="tmcSteppers">
      <div class="text-body-secondary small">Not running.</div>
    </div>
  </div>

  <!-- --------------------------------------------------------- presets -->
  <div class="card mb-3">
    <div class="card-header d-flex flex-wrap gap-2 justify-content-between align-items-center">
      <span class="fw-semibold">Presets</span>
      <button type="button" class="buttons btn-outline-success btn-sm" id="tmcAddPreset">
        <i class="fas fa-plus"></i> Add preset</button>
    </div>
    <div class="card-body">
      <div class="table-responsive tmc-presets-wrap">
        <table class="table table-sm align-middle tmc-presets-table">
          <thead>
            <tr>
              <th style="width:5rem">Value<?php echo tmcHelp("The byte value (1-255) a stepper's channel must carry to trigger this preset. Each value may be defined once; 0 is built in and means return home."); ?></th>
              <th style="width:10rem">Name</th>
              <th style="width:10rem">Applies to<?php echo tmcHelp('Which stepper may run this preset. <b>All steppers</b> shares one definition across the lot: the same preset runs on whichever stepper\'s channel carries that value, so two steppers doing the same move do not need it authored twice -- right for a percentage-based move. Scope it to one stepper when the motion is specific to that mechanism, as a cycle\'s full-step waypoints always are.'); ?></th>
              <th style="width:6rem">Type</th>
              <th>Detail</th>
              <!-- Actions: Run now / Edit / delete come to about 13rem; the
                   fixed width keeps them from reflowing the other columns. -->
              <th style="width:16rem"></th>
            </tr>
          </thead>
          <tbody id="tmcPresets"></tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- --------------------------------------------------- preset editor -->
  <div class="modal fade" id="tmcPresetModal" tabindex="-1" aria-labelledby="tmcPresetModalLabel" aria-hidden="true">
    <div class="modal-dialog modal-lg modal-dialog-scrollable">
      <div class="modal-content">
        <div class="modal-header py-2">
          <h5 class="modal-title" id="tmcPresetModalLabel">Preset</h5>
          <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
        </div>
        <div class="modal-body">
          <div id="tmcPmAlert"></div>
          <div class="container-fluid settingsTable" id="tmcPmFields"></div>
        </div>
        <div class="modal-footer py-2">
          <button type="button" class="buttons" data-bs-dismiss="modal">Cancel</button>
          <button type="button" class="buttons btn-success" id="tmcPmSave"><i class="fas fa-check"></i> Save preset</button>
        </div>
      </div>
    </div>
  </div>


  <p class="text-muted small mb-3">How the plugin works, commissioning and wiring:
    press <strong>F1</strong> (or Help &rsaquo; Help) on this page.</p>

  <!-- ------------------------------------------------------- steppers -->
  <div class="card mb-3">
    <div class="card-header d-flex flex-wrap gap-2 justify-content-between align-items-center">
      <span class="fw-semibold">Stepper Configuration</span>
      <div class="d-flex align-items-center gap-2 flex-wrap">
        <button type="button" class="buttons btn-success btn-sm" id="tmcSave">
          <i class="fas fa-save"></i> Save configuration</button>
        <button type="button" class="buttons btn-secondary btn-sm" id="tmcReload">
          <i class="fas fa-undo"></i> Discard changes</button>
      </div>
    </div>
    <div class="card-body">
      <!-- Global, not per-stepper: one collapsible above the tabs, shut by
           default, its header digesting the sections inside (channels, SPI
           configuration, and the defaults block every stepper inherits).
           Filled and read back by assets/bind.js like the panes. -->
      <div class="settingsTable mb-3">
        <?php echo tmcSection('g', 'all', 'Global settings', tmcGlobalForm()); ?>
      </div>
      <!-- The tabs, then the Add and Remove buttons, on one bar whose bottom
           rule runs the full width. The buttons stay outside the tablist:
           FPP's page load makes every button in a [role="tablist"] li a
           Bootstrap Tab. -->
      <div class="d-flex flex-wrap align-items-end border-bottom" id="tmcTabBar">
        <ul class="nav nav-tabs border-0" id="tmcStepperTabs" role="tablist">
<?php for ($i = 0; $i < $tmcMaxAxes; $i++) { ?>
          <li class="nav-item d-none"><button class="nav-link" data-bs-toggle="tab" data-bs-target="#tmcStepper<?php echo $i; ?>" type="button"></button></li>
<?php } ?>
        </ul>
        <div class="d-flex align-items-center gap-1 ps-2 py-1">
          <!-- Why an Add is dead (the maximum) is the buttons' title, set by
               bind.js, rather than a line of text. -->
          <span id="tmcAddStepperWrap" class="d-inline-block">
            <button type="button" class="buttons btn-outline-success btn-sm" id="tmcAddStepper">
              <i class="fas fa-plus"></i> Add stepper</button>
          </span>
          <button type="button" class="buttons btn-outline-secondary btn-sm tmcTip" id="tmcAddSimulated"
                  data-bs-title="Adds a stepper that runs against the built-in simulator. It takes no bus or chip select.">
            <i class="fas fa-plus"></i> Add simulated</button>
        </div>
        <div class="ms-auto py-1">
          <button type="button" class="buttons btn-danger btn-sm tmcTip" id="tmcRemoveStepper"
                  data-bs-title="Removes the stepper shown. Its commissioning results are kept on disk, so re-adding it under the same name restores home and the travel range.">
            <i class="fas fa-trash"></i> Remove this stepper</button>
        </div>
      </div>
      <!-- One pane per possible stepper (MAX_AXES), hidden until a stepper
           occupies it. -->
      <div class="tab-content pt-3" id="tmcStepperPanes">
<?php for ($i = 0; $i < $tmcMaxAxes; $i++) { ?>
        <div class="tab-pane fade" id="tmcStepper<?php echo $i; ?>" data-stepper="<?php echo $i; ?>">
<?php echo tmcStepperPane($i); ?>
        </div>
<?php } ?>
      </div>
      <div id="tmcNoSteppers" class="text-body-secondary py-3">
        No steppers yet. <b>Add stepper</b> (or <b>Add simulated</b> to try the page without
        hardware), check its bus and chip-select pin against the wiring, set its channel, save,
        and use Commissioning to find home and the travel range.
      </div>
    </div>
  </div>
</div>

<script type="text/javascript">
  // What the modules cannot work out for themselves (a .js file served by
  // plugin.php gets no PHP pass).
  window.TMC = window.TMC || {};
  TMC.API = <?php echo json_encode($tmcApi); ?>;
  // The channel output's type in co-other.json: the plugin's directory name.
  TMC.OUTPUT_TYPE = <?php echo json_encode($tmcPlugin); ?>;
  // MAX_AXES from src/AxisConfig.h: how many panes exist above and where
  // "Add stepper" stops.
  TMC.MAX_AXES = <?php echo intval($tmcMaxAxes); ?>;
</script>
<?php
// Dependency order, not alphabetical. core defines the shared helpers and
// TMC.state; bind fills the static form; main boots the page and must be last.
foreach (array('core', 'bind', 'presets', 'status', 'actions', 'main') as $m) {
    printf("<script type=\"text/javascript\" src=\"%s\"></script>\n",
           htmlspecialchars(tmcAsset($m . '.js'), ENT_QUOTES));
}
?>
