/*
 * Stepper Motion -- bind.js
 *
 * The stepper form is static markup in plugin_setup.php: a Global settings
 * form holding the SPI configuration (the wiring table) and the
 * configuration's defaults block, and MAX_AXES panes, one per possible
 * stepper, every control tagged data-path="motor.irun" and a row that only
 * applies sometimes tagged data-showif="path=value". This module fills
 * those controls from the configuration, reads them back into it on every
 * edit, applies the showIf rules, resolves what each stepper inherits from
 * the defaults, and adds/removes steppers. Bounds are the daemon's
 * business: it validates on save and returns every problem.
 *
 * SPI: a real stepper names its bus and chip-select pin (spi.bus, spi.cs, a
 * header pin name as FPP names them, "P1-24"); a simulated one has neither.
 * The bus list comes from the daemon (TMC.state.spiBuses, with whether this
 * box has the device node); the chip-select choices are the GPIO pins of
 * the header less the data lines of every bus, a pin another stepper holds
 * greyed out. The daemon refuses a duplicate or a data pin on save.
 *
 * Inheritance: a stepper's block carries only what differs from the
 * defaults -- an inheritable path is simply absent from
 * TMC.state.config.axes[i] while it inherits -- and TMC.resolve() gives the
 * effective value. In the pane an empty number input, the "(global: ...)"
 * driver option, the "(global)" hold mode or an unticked override switch
 * all mean inherit, with the effective value as the placeholder.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";

  // The daemon's MAX_AXES (src/AxisConfig.h), set by plugin_setup.php before
  // this module loads; the literal only covers a page that did not.
  TMC.MAX_AXES = TMC.MAX_AXES || 8;

  // The default chip-select pins, the order new steppers take them: SPI0 on
  // header pins 24, 26, 22, 18, then SPI1 on 12, 11, 36, 13. The README's
  // and the F1 help's wiring tables are written from this list.
  TMC.DEFAULT_CS = [
    { bus: 0, cs: 'P1-24' }, { bus: 0, cs: 'P1-26' }, { bus: 0, cs: 'P1-22' }, { bus: 0, cs: 'P1-18' },
    { bus: 1, cs: 'P1-12' }, { bus: 1, cs: 'P1-11' }, { bus: 1, cs: 'P1-36' }, { bus: 1, cs: 'P1-13' }
  ];
  // The buses of every 40-pin Pi, what the page assumes until the daemon's
  // list (?action=config spiBuses: the same shape plus whether the box has
  // the device node) arrives.
  TMC.BUSES = [
    { bus: 0, path: '/dev/spidev0.0', pins: { miso: 'P1-21', mosi: 'P1-19', sclk: 'P1-23' }, available: true },
    { bus: 1, path: '/dev/spidev1.0', pins: { miso: 'P1-35', mosi: 'P1-38', sclk: 'P1-40' }, available: false }
  ];
  // BCM GPIO of each GPIO pin of the 40-pin header, by header number, for
  // the labels ("P1-24 (GPIO 8)"). Pins missing here are power, ground or
  // the HAT ID EEPROM pair (27/28), which are never chip selects
  // (isChipSelectPin in src/AxisConfig.cpp); P1-3/5 (I2C1) are left out for
  // the same reason.
  TMC.HEADER_GPIO = {
    7: 4, 8: 14, 10: 15, 11: 17, 12: 18, 13: 27, 15: 22, 16: 23, 18: 24, 19: 10, 21: 9, 22: 25,
    23: 11, 24: 8, 26: 7, 29: 5, 31: 6, 32: 12, 33: 13, 35: 19, 36: 16, 37: 26, 38: 20, 40: 21
  };
  TMC.gpioOf = function gpioOf(pin) { return TMC.HEADER_GPIO[parseInt(String(pin).replace('P1-', ''), 10)]; };
  // "P1-24 / GPIO8"
  TMC.pinText = function pinText(pin) {
    const g = TMC.gpioOf(pin);
    return String(pin).replace('P1-', '') + (g === undefined ? '' : ' / GPIO' + g);
  };

  // Per-driver values (TmcDriverVariant in src/TmcRegs.h) that fclk and
  // rsense derive from when nothing sets them.
  TMC.DRIVER_DEFAULTS = {
    'motor.rsense': { TMC5130: 0.15, TMC5160: 0.075 },
    'motor.fclk': { TMC5130: 13200000, TMC5160: 12000000 }
  };

  // Field scopes, the field table of AxisConfig.cpp seen from the page:
  // 'inherit' lives in the defaults and a stepper may override it; 'derived'
  // is inherit plus the driver-derived fallback above; 'global' lives only
  // in the defaults. Anything not listed is per stepper.
  TMC.SCOPE = {
    driver: 'inherit', holdSeconds: 'inherit',
    'motor.irun': 'inherit', 'motor.ihold': 'inherit',
    'motor.rsense': 'derived', 'motor.fclk': 'derived',
    'motion.rampTimeSec': 'inherit',
    'rehome.coarseRps': 'inherit', 'rehome.coarseIrun': 'inherit',
    'rehome.creepRps': 'inherit', 'rehome.creepIrun': 'inherit',
    'spi.speedHz': 'global',
    'motor.iholdDelay': 'global', 'motor.chopconf': 'global', 'motor.vsense': 'global',
    'motor.globalScaler': 'global',
    'rehome.coarseOvertravelDeg': 'global', 'rehome.backoffSteps': 'global',
    'rehome.backoffRps': 'global', 'rehome.creepOvertravelSteps': 'global',
    'rehome.coarseIhold': 'global', 'rehome.creepIhold': 'global',
    'rehome.settleMs': 'global', 'rehome.amax': 'global',
    'rehome.autoRecoverOnDemand': 'global', 'rehome.autoRecoverMaxAttempts': 'global',
    'rehome.autoRecoverAtStartup': 'global'
  };
  TMC.scopePaths = function scopePaths(scopes) {
    return Object.keys(TMC.SCOPE).filter(function (p) { return scopes.indexOf(TMC.SCOPE[p]) >= 0; });
  };

  // The built-in defaults, as the defaults block: what the daemon reports
  // as defaultsEffective, and what a fresh configuration is seeded with.
  // fclk and rsense are absent: derived from the driver.
  TMC.builtinDefaults = function builtinDefaults() {
    return {
      driver: 'TMC5130',
      spi: { speedHz: 1000000 },
      motor: { irun: 16, ihold: 8, iholdDelay: 6, chopconf: '0x000100C3', vsense: false, globalScaler: 256 },
      holdSeconds: 30, // DEFAULT_HOLD_SECONDS in src/AxisConfig.h; null = hold indefinitely
      motion: { rampTimeSec: 0.15 },
      rehome: { autoRecoverOnDemand: true, autoRecoverMaxAttempts: 2, autoRecoverAtStartup: false,
                coarseRps: 0.15, coarseIrun: 10, backoffSteps: 3, creepRps: 0.02,
                creepIrun: 6, creepOvertravelSteps: 5, coarseOvertravelDeg: 10, coarseIhold: 4,
                backoffRps: 0.05, creepIhold: 4, settleMs: 500, amax: 200 }
    };
  };

  // A blank stepper: only what is its own -- which stepper this is (name,
  // channel, whether it is simulated, its bus and chip select: the first of
  // the default wiring no other stepper holds), its limits and homing
  // direction. Nothing inheritable is set, so a new stepper follows the
  // defaults for all of it.
  TMC.defaultStepper = function defaultStepper(index, fake) {
    const port = TMC.freeDefaultPort();
    // The first stepperN no other stepper is called: after a removal,
    // stepper(count + 1) can already be taken, and names must be unique.
    const taken = (TMC.state.config.axes || []).map(function (a) { return a.name; });
    let n = index + 1;
    while (taken.indexOf('stepper' + n) >= 0) n++;
    return {
      name: 'stepper' + n,
      channel: { mode: 'slot', slot: TMC.freeSlot(-1), number: '', model: '', offset: 0 },
      spi: { fake: !!fake, bus: port.bus, cs: port.cs },
      motor: {},
      motion: {},
      limits: { minFullsteps: 0, maxFullsteps: 0 },
      rehome: { dir: 'ccw' }
    };
  };

  // "motor.irun" -> a.motor.irun
  TMC.getPath = function getPath(obj, path) {
    return path.split('.').reduce(function (cur, k) {
      return (cur === null || cur === undefined) ? undefined : cur[k];
    }, obj);
  };
  TMC.deletePath = function deletePath(obj, path) {
    const parts = path.split('.');
    const parent = parts.length > 1 ? TMC.getPath(obj, parts.slice(0, -1).join('.')) : obj;
    if (parent && typeof parent === 'object') delete parent[parts[parts.length - 1]];
  };

  // A stepper from the file, made safe for the form: the per-stepper
  // sections it may omit filled from the blank stepper, its inheritable
  // fields kept as they are (absent = inherits), the global-only fields
  // dropped.
  TMC.normaliseStepper = function normaliseStepper(a, index) {
    const out = TMC.defaultStepper(index);
    Object.keys(a).forEach(function (k) {
      const v = a[k];
      out[k] = (v && typeof v === 'object' && !Array.isArray(v)) ? Object.assign({}, out[k] || {}, v) : v;
    });
    delete out.presets; // never a per-stepper block; the shared table is config.presets
    TMC.scopePaths(['global']).forEach(function (p) { TMC.deletePath(out, p); });
    return out;
  };

  // ---------------------------------------------------------- SPI ports --

  TMC.buses = function buses() {
    return Array.isArray(TMC.state.spiBuses) && TMC.state.spiBuses.length ? TMC.state.spiBuses : TMC.BUSES;
  };
  TMC.busOf = function busOf(n) {
    return TMC.buses().find(function (b) { return b.bus === parseInt(n, 10); }) || null;
  };
  // Every data line of every bus, which a chip select may not be.
  TMC.dataPins = function dataPins() {
    const out = [];
    TMC.buses().forEach(function (b) { out.push(b.pins.miso, b.pins.mosi, b.pins.sclk); });
    return out;
  };
  // The header pins a chip select may be driven from, in header order.
  TMC.csPins = function csPins() {
    const data = TMC.dataPins();
    return Object.keys(TMC.HEADER_GPIO).map(function (n) { return 'P1-' + n; })
      .filter(function (p) { return data.indexOf(p) < 0; })
      .sort(function (a, b) { return parseInt(a.slice(3), 10) - parseInt(b.slice(3), 10); });
  };
  TMC.realSteppers = function realSteppers() {
    return TMC.state.config.axes.filter(function (a) { return !(a.spi && a.spi.fake); });
  };
  // Which real stepper holds a pin, or null.
  TMC.holderOf = function holderOf(pin, except) {
    return TMC.realSteppers().find(function (a) { return a !== except && a.spi.cs === pin; }) || null;
  };
  // The first of the default wiring no real stepper holds; past eight, the
  // first free chip-select pin on SPI0.
  TMC.freeDefaultPort = function freeDefaultPort() {
    const free = TMC.DEFAULT_CS.find(function (d) { return !TMC.holderOf(d.cs, null); });
    if (free) return { bus: free.bus, cs: free.cs };
    const pin = TMC.csPins().find(function (p) { return !TMC.holderOf(p, null); });
    return { bus: 0, cs: pin || TMC.DEFAULT_CS[0].cs };
  };

  // The SPI configuration section: the wiring table (one row per real
  // stepper: its chip select and its bus's shared lines), the buses this
  // box has, and the summary on the shut header.
  TMC.renderSpi = function renderSpi() {
    const real = TMC.realSteppers();
    let rows = '';
    real.forEach(function (a) {
      const b = TMC.busOf(a.spi.bus);
      const p = b ? b.pins : { miso: '?', mosi: '?', sclk: '?' };
      rows += '<tr' + (b && !b.available ? ' class="text-warning"' : '') + '><td>' + TMC.esc(a.name) + (a.enabled === false ? ' (disabled)' : '') +
        '</td><td>SPI' + TMC.esc(a.spi.bus) + '</td><td>' + TMC.esc(TMC.pinText(a.spi.cs)) + '</td><td>' +
        TMC.esc(TMC.pinText(p.mosi)) + '</td><td>' + TMC.esc(TMC.pinText(p.miso)) + '</td><td>' +
        TMC.esc(TMC.pinText(p.sclk)) + '</td></tr>';
    });
    document.querySelector('#tmcSpiWiring tbody').innerHTML = rows ||
      '<tr><td colspan="6" class="text-body-secondary">No real steppers yet.</td></tr>';
    const onSpi1 = real.some(function (a) { return parseInt(a.spi.bus, 10) === 1; });
    document.getElementById('tmcSpiBuses').textContent = onSpi1
      ? ' SPI1 uses the I\u00b2S pins, so an I\u00b2S audio HAT or DAC cannot share this header.' : '';
    // Which buses this FPP has a device node for; a stepper on one it does
    // not is flagged (its row above, its pane, and here).
    const kernel = document.getElementById('tmcSpiKernel');
    const stale = document.getElementById('tmcSpiStale');
    kernel.textContent = 'Buses on this box: ' + TMC.buses().map(function (b) {
      return 'SPI' + b.bus + ' (' + b.path + (b.available ? ')' : ', not present)');
    }).join(', ') + '.';
    const missing = real.filter(function (a) { const b = TMC.busOf(a.spi.bus); return b && !b.available; });
    stale.textContent = missing.length
      ? missing.map(function (a) { return a.name; }).join(', ') + ' on SPI' + missing[0].spi.bus +
        ', which this box does not have: SPI1 needs FPP\u2019s fpp-spi1-nopins overlay, which upgrading FPP adds, and a reboot (press F1).'
      : '';
    stale.classList.toggle('d-none', !missing.length);
    const perBus = {};
    real.forEach(function (a) { (perBus[a.spi.bus] = perBus[a.spi.bus] || []).push(String(a.spi.cs).replace('P1-', '')); });
    TMC.setSummary('g', 'spi', real.length + ' real stepper' + (real.length === 1 ? '' : 's') +
      Object.keys(perBus).map(function (b) { return ' \u00b7 SPI' + b + ' CS ' + perBus[b].join('/'); }).join('') +
      (missing.length ? ' \u00b7 SPI1 not on this box' : ''));
  };

  // The bus and chip-select selects of one pane: the buses with whether
  // the box has them, the pins with the stepper holding each greyed out.
  function fillPortSelects(i) {
    const a = TMC.state.config.axes[i];
    const root = pane(i);
    const busSel = root.querySelector('[data-path="spi.bus"]');
    busSel.innerHTML = TMC.buses().map(function (b) {
      return '<option value="' + b.bus + '">SPI' + b.bus + ' \u2014 ' + TMC.esc(b.path) +
        (b.available ? '' : ' (not on this box)') + '</option>';
    }).join('');
    busSel.value = String(a.spi.bus === undefined ? 0 : a.spi.bus);
    const csSel = root.querySelector('[data-path="spi.cs"]');
    csSel.innerHTML = TMC.csPins().map(function (p) {
      const h = TMC.holderOf(p, a);
      return '<option value="' + p + '"' + (h ? ' disabled' : '') + '>' + p + ' (GPIO ' + TMC.gpioOf(p) + ')' +
        (h ? ' \u2014 ' + TMC.esc(h.name) : '') + '</option>';
    }).join('');
    // A pin outside the list (a hand-edited file) is kept and shown; the
    // daemon says what is wrong with it on save.
    if (a.spi.cs && !csSel.querySelector('option[value="' + a.spi.cs + '"]'))
      csSel.insertAdjacentHTML('beforeend', '<option value="' + TMC.esc(a.spi.cs) + '">' + TMC.esc(a.spi.cs) + '</option>');
    csSel.value = a.spi.cs || '';
    const b = TMC.busOf(a.spi.bus);
    const warn = document.getElementById('tmcBusWarn' + i);
    warn.textContent = (b && !b.available && !a.spi.fake)
      ? 'This box has no ' + b.path + ': SPI1 needs FPP\u2019s fpp-spi1-nopins overlay, which upgrading FPP adds, and a reboot (press F1). The stepper will not start until it does.' : '';
    warn.classList.toggle('d-none', !warn.textContent);
  }

  // The defaults block for the form: every inheritable and global field
  // set, from the daemon's resolved view (built-in values included) --
  // except fclk and rsense, which stay unset unless the file's block has
  // them, so the form shows them derived from the driver.
  TMC.normaliseDefaults = function normaliseDefaults(raw, effective) {
    const out = TMC.builtinDefaults();
    const eff = effective || {};
    TMC.scopePaths(['inherit', 'global']).forEach(function (p) {
      const v = TMC.getPath(eff, p);
      if (v !== undefined) TMC.setPath(out, p, v);
    });
    TMC.scopePaths(['derived']).forEach(function (p) {
      const v = TMC.getPath(raw || {}, p);
      if (v !== undefined && v !== null) TMC.setPath(out, p, v);
    });
    if (out.holdSeconds === undefined) out.holdSeconds = null;
    return out;
  };

  TMC.sameValue = function sameValue(x, y) {
    if (typeof x === 'number' && typeof y === 'number') return Math.abs(x - y) < 1e-9;
    return x === y;
  };

  // The effective value of one inheritable path on a stepper: its own if it
  // has one, else the defaults', else (fclk, rsense) the effective driver's.
  // Note that a null holdSeconds is a value (hold indefinitely), not absence.
  TMC.resolve = function resolve(a, path) {
    const own = TMC.getPath(a, path);
    if (own !== undefined) return own;
    const d = TMC.getPath(TMC.state.config.defaults, path);
    if (d !== undefined) return d;
    if (TMC.SCOPE[path] === 'derived') return TMC.DRIVER_DEFAULTS[path][TMC.resolve(a, 'driver')];
    return undefined;
  };
  TMC.inherits = function inherits(a, path) { return TMC.getPath(a, path) === undefined; };

  // The stepper with every inheritable and global field resolved -- what
  // the daemon runs it with, and what jog/probe/Test travel send.
  TMC.effectiveStepper = function effectiveStepper(i) {
    const a = TMC.state.config.axes[i];
    if (!a) return null;
    const e = JSON.parse(JSON.stringify(a));
    Object.keys(TMC.SCOPE).forEach(function (p) { TMC.setPath(e, p, TMC.resolve(a, p)); });
    return e;
  };

  // The datasheet current formula, so the slider can show amps as you drag
  // it. Kept in step with rmsCurrentAmps() in src/TmcRegs.h. Takes a
  // resolved stepper (effectiveStepper) or the defaults block.
  TMC.rmsCurrent = function rmsCurrent(stepper, cs) {
    const m = stepper.motor || {};
    const vfs = m.vsense ? 0.180 : 0.325;
    let rsense = parseFloat(m.rsense);
    if (!(rsense > 0)) rsense = TMC.DRIVER_DEFAULTS['motor.rsense'][stepper.driver] || 0.15;
    let scale = 1.0;
    if (stepper.driver === 'TMC5160') {
      let gs = parseInt(m.globalScaler, 10);
      if (!(gs > 0 && gs <= 256)) gs = 256;
      scale = gs / 256.0;
    }
    return scale * ((cs + 1) / 32.0) * (vfs / (rsense + 0.02)) / Math.SQRT2;
  };

  function pane(i) { return document.getElementById('tmcStepper' + i); }
  function tab(i) { return document.querySelectorAll('#tmcStepperTabs .nav-item')[i]; }
  function globalForm() { return document.getElementById('tmcGlobalForm'); }

  // How a control's value goes back into the config: a switch is a bool, a
  // number input is a float when its step has a decimal point and an int
  // otherwise, anything else (name, chopconf, selects) is a string.
  function readControl(el) {
    if (el.type === 'checkbox') return el.checked;
    if (el.type === 'number' || el.type === 'range') {
      return String(el.step).indexOf('.') >= 0 ? (parseFloat(el.value) || 0) : (parseInt(el.value, 10) || 0);
    }
    return el.value;
  }

  function setControl(el, v) {
    if (el.type === 'checkbox') el.checked = !!v;
    else el.value = (v === undefined || v === null) ? '' : v;
  }

  // What a value looks like as a placeholder or in a hint.
  function showValue(path, v) {
    if (path === 'holdSeconds') return v === null || v === undefined ? 'hold indefinitely' : (v === 0 ? 'release at once' : v + ' s');
    if (typeof v === 'boolean') return v ? 'on' : 'off';
    return (v === undefined || v === null) ? '' : String(v);
  }

  // ----------------------------------------------------- channel block --
  // The plugin reads a block of MAX_AXES channels (config.json's
  // channelBlock); each stepper takes a slot in it unless it has its own
  // channel (channel.mode absolute or model).

  // The block's first channel as far as the page can tell: the number, or
  // the model's start from FPP's model list (null when that model is not
  // loaded -- fppd resolves it again at startup).
  TMC.blockStart = function blockStart() {
    const b = TMC.state.config.channelBlock || {};
    if (b.source === 'model') {
      const m = (TMC.state.overlayModelInfo || {})[b.model || ''];
      return m ? m.StartChannel : null;
    }
    return parseInt(b.start, 10) || 1;
  };

  TMC.slotChannelText = function slotChannelText(slot) {
    const start = TMC.blockStart();
    if (start) return 'channel ' + (start + slot - 1);
    const b = TMC.state.config.channelBlock || {};
    return 'model ' + (b.model || '?') + ' + ' + (slot - 1);
  };

  // Index of the stepper (other than `except`) on slot n, or -1.
  TMC.slotHolder = function slotHolder(n, except) {
    const axes = TMC.state.config.axes || [];
    for (let k = 0; k < axes.length; k++) {
      const c = axes[k].channel || {};
      if (k !== except && c.mode === 'slot' && parseInt(c.slot, 10) === n) return k;
    }
    return -1;
  };

  // The lowest slot no other stepper holds (1 if, impossibly, none is free).
  TMC.freeSlot = function freeSlot(except) {
    for (let n = 1; n <= TMC.MAX_AXES; n++) {
      if (TMC.slotHolder(n, except) < 0) return n;
    }
    return 1;
  };

  // Each slot with the channel it reads, and who else holds it.
  function fillSlotSelect(i) {
    const sel = pane(i).querySelector('[data-path="channel.slot"]');
    if (!sel) return;
    const a = TMC.state.config.axes[i];
    let opts = '';
    for (let n = 1; n <= TMC.MAX_AXES; n++) {
      const k = TMC.slotHolder(n, i);
      opts += '<option value="' + n + '"' + (k >= 0 ? ' disabled' : '') + '>Slot ' + n + ' · ' +
        TMC.esc(TMC.slotChannelText(n)) +
        (k >= 0 ? ' — ' + TMC.esc(TMC.state.config.axes[k].name) : '') + '</option>';
    }
    sel.innerHTML = opts;
    sel.value = String(parseInt(a.channel.slot, 10) || '');
  }

  // The Channels section from page state, then every pane's slot list.
  TMC.fillBlockForm = function fillBlockForm() {
    const b = TMC.state.config.channelBlock;
    document.getElementById('tmcBlockSource').value = b.source === 'model' ? 'model' : 'channel';
    document.getElementById('tmcBlockStart').value = parseInt(b.start, 10) || 1;
    TMC.refreshChannels();
  };

  TMC.refreshChannels = function refreshChannels() {
    const b = TMC.state.config.channelBlock;
    const isModel = b.source === 'model';
    document.getElementById('tmcBlockStartRow').classList.toggle('d-none', isModel);
    document.getElementById('tmcBlockModelRow').classList.toggle('d-none', !isModel);

    // The model picker: FPP's models, and a name that is not among them kept
    // and flagged.
    const sel = document.getElementById('tmcBlockModel');
    const warn = document.getElementById('tmcBlockModelWarn');
    const info = TMC.state.overlayModelInfo || {};
    const name = b.model || '';
    let opts = '<option value="">-- pick a model --</option>';
    (TMC.state.overlayModels || []).forEach(function (m) {
      opts += '<option value="' + TMC.esc(m) + '">' + TMC.esc(m) + '</option>';
    });
    if (name && !info[name]) opts += '<option value="' + TMC.esc(name) + '">' + TMC.esc(name) + '</option>';
    sel.innerHTML = opts;
    sel.value = name;
    let w = '';
    if (isModel && name && !info[name]) {
      w = 'No model called ' + name + ' is loaded. Steppers on a slot will not follow any channel until it exists.';
    } else if (isModel && name && info[name].ChannelCount < TMC.MAX_AXES) {
      w = name + ' has ' + info[name].ChannelCount + ' channels; make it ' + TMC.MAX_AXES +
        ' in your sequencer, or the last slots read whatever follows it in the show.';
    }
    warn.textContent = w;
    warn.classList.toggle('d-none', !w);

    const start = TMC.blockStart();
    const range = start ? 'channels ' + start + '–' + (start + TMC.MAX_AXES - 1) : '';
    document.getElementById('tmcBlockInfo').textContent = start
      ? 'Slots 1–' + TMC.MAX_AXES + ' read ' + range + (isModel ? ' (where ' + name + ' is now).' : '.')
      : (isModel && !name ? 'Pick the model the block starts at.' : '');
    TMC.setSummary('g', 'channels', isModel ? 'model ' + (name || '?') + (range ? ' · ' + range : '') : range);

    TMC.state.config.axes.forEach(function (a, i) {
      fillSlotSelect(i);
      TMC.refreshSummaries(i);
    });
  };

  // The model picker lists FPP's pixel overlay models; a name that is not
  // among them is kept and flagged (the models may simply not be uploaded yet).
  function fillModelSelect(i, a) {
    const sel = pane(i).querySelector('[data-path="channel.model"]');
    const warn = document.getElementById('tmcModelWarn' + i);
    const name = a.channel.model || '';
    const models = TMC.state.overlayModels || [];
    let known = false, opts = '<option value="">-- pick a model --</option>';
    models.forEach(function (m) {
      if (m === name) known = true;
      opts += '<option value="' + TMC.esc(m) + '">' + TMC.esc(m) + '</option>';
    });
    if (name && !known) opts += '<option value="' + TMC.esc(name) + '">' + TMC.esc(name) + '</option>';
    sel.innerHTML = opts;
    sel.value = name;
    warn.textContent = (name && !known)
      ? 'No model called ' + name + ' is loaded. This stepper will not follow any channel until it exists.' : '';
    warn.classList.toggle('d-none', !warn.textContent);
  }

  // Every data-showif="path=value" in one form, against its current values.
  function applyShowIf(root, values) {
    root.querySelectorAll('[data-showif]').forEach(function (n) {
      const rule = n.dataset.showif.split('=');
      n.classList.toggle('d-none', String(TMC.getPath(values, rule[0])) !== rule[1]);
    });
  }
  TMC.applyVisibility = function applyVisibility(i) {
    applyShowIf(pane(i), TMC.state.config.axes[i]);
  };

  TMC.refreshAmps = function refreshAmps(i) {
    const e = TMC.effectiveStepper(i);
    document.getElementById('irunAmps' + i).textContent = TMC.rmsCurrent(e, e.motor.irun).toFixed(2) + ' A RMS';
    document.getElementById('iholdAmps' + i).textContent = TMC.rmsCurrent(e, e.motor.ihold).toFixed(2) + ' A RMS';
  };

  // The inherited state of one pane: placeholders, the hint under each
  // inheritable row, the driver's "(global: ...)" option, the hold mode and
  // the override switches. Run after any edit of the defaults too, since
  // that is where the placeholders come from.
  TMC.refreshInherited = function refreshInherited(i) {
    const a = TMC.state.config.axes[i];
    const root = pane(i);
    const d = TMC.state.config.defaults;
    root.querySelector('[data-path="driver"] option[value=""]').textContent = '(global: ' + d.driver + ')';
    root.querySelectorAll('[data-inherit]').forEach(function (hint) {
      const p = hint.dataset.inherit;
      const inherits = TMC.inherits(a, p);
      // What the stepper gets without a value of its own: the global
      // setting, or for fclk/rsense with no global value the driver's own.
      const derived = TMC.SCOPE[p] === 'derived' && TMC.getPath(d, p) === undefined;
      const base = derived ? TMC.DRIVER_DEFAULTS[p][TMC.resolve(a, 'driver')] : TMC.getPath(d, p);
      const source = derived ? 'the ' + TMC.resolve(a, 'driver') + '-BOB value' : 'the global setting';
      hint.textContent = (inherits ? 'Inherits ' : 'Overrides ') + source + ' (' + showValue(p, base) + ')';
      hint.classList.toggle('text-body-secondary', inherits);
    });
    root.querySelectorAll('input[type="number"][data-path]').forEach(function (el) {
      const p = el.dataset.path;
      if (TMC.SCOPE[p] === 'inherit' || TMC.SCOPE[p] === 'derived') {
        if (el.dataset.hold) return;
        el.placeholder = showValue(p, TMC.resolve(a, p));
      }
    });
    root.querySelectorAll('[data-override]').forEach(function (sw) {
      const p = sw.dataset.override;
      const own = !TMC.inherits(a, p);
      sw.checked = own;
      const range = root.querySelector('[data-path="' + p + '"]');
      range.disabled = !own;
      range.value = TMC.resolve(a, p);
    });
    // holdSeconds: the mode select and the seconds input beside it.
    const mode = root.querySelector('[data-hold="mode"]');
    const secs = root.querySelector('[data-hold="secs"]');
    const h = TMC.getPath(a, 'holdSeconds');
    mode.value = h === undefined ? '' : (h === null ? 'hold' : 'n');
    secs.classList.toggle('d-none', mode.value !== 'n');
    if (mode.value === 'n' && parseInt(secs.value, 10) !== h) secs.value = h;
    mode.querySelector('option[value=""]').textContent = '(global: ' + showValue('holdSeconds', d.holdSeconds) + ')';
  };

  // Fill one pane from its stepper.
  function fillPane(i) {
    const a = TMC.state.config.axes[i];
    pane(i).querySelectorAll('[data-path]').forEach(function (el) {
      const p = el.dataset.path;
      if (p === 'channel.model' || p === 'channel.slot' || p === 'spi.bus' || p === 'spi.cs' || el.dataset.hold) return;
      if (p === 'enabled') { el.checked = a.enabled !== false; return; } // absent = enabled
      // An inheritable field that inherits is absent, so left empty; the
      // placeholder (refreshInherited) shows what it gets.
      setControl(el, TMC.getPath(a, p));
    });
    fillPortSelects(i);
    fillModelSelect(i, a);
    fillSlotSelect(i);
    tab(i).querySelector('.nav-link').textContent = a.name;
    TMC.applyVisibility(i);
    TMC.refreshInherited(i);
    TMC.refreshAmps(i);
    TMC.refreshSummaries(i);
  }

  // Fill the Global settings form from the defaults block.
  TMC.renderDefaults = function renderDefaults() {
    const d = TMC.state.config.defaults;
    globalForm().querySelectorAll('[data-path]').forEach(function (el) {
      setControl(el, TMC.getPath(d, el.dataset.path));
    });
    TMC.refreshGlobal();
  };

  // The global form's derived placeholders, showIf rules, amps and
  // summaries, then every pane's inherited state, since it follows.
  TMC.refreshGlobal = function refreshGlobal() {
    const d = TMC.state.config.defaults;
    const root = globalForm();
    TMC.scopePaths(['derived']).forEach(function (p) {
      root.querySelector('[data-path="' + p + '"]').placeholder = String(TMC.DRIVER_DEFAULTS[p][d.driver]);
    });
    applyShowIf(root, d);
    document.getElementById('irunAmpsg').textContent = TMC.rmsCurrent(d, d.motor.irun).toFixed(2) + ' A RMS';
    document.getElementById('iholdAmpsg').textContent = TMC.rmsCurrent(d, d.motor.ihold).toFixed(2) + ' A RMS';
    const m = d.motor, r = d.rehome;
    TMC.setSummary('g', 'motor', d.driver + ' · IRUN ' + m.irun + ' (' + TMC.rmsCurrent(d, m.irun).toFixed(2) + ' A) · ' +
      'IHOLD ' + m.ihold + ' · ' + showValue('holdSeconds', d.holdSeconds) + ' · ramp ' + d.motion.rampTimeSec + ' s to speed');
    TMC.setSummary('g', 'rehome', 'coarse ' + r.coarseRps + ' rev/s · creep ' + r.creepRps + ' rev/s · AMAX ' + r.amax +
      (r.autoRecoverOnDemand ? ' · auto-recover on demand' : ' · auto-recover off') +
      (r.autoRecoverAtStartup ? ' + at startup' : ''));
    // The Global settings header digests the three sections inside it.
    const cnt = TMC.state.config.axes.length;
    TMC.setSummary('g', 'all', cnt + ' stepper' + (cnt === 1 ? '' : 's') + ' · ' + d.driver +
      ' · IRUN ' + m.irun + ' (' + TMC.rmsCurrent(d, m.irun).toFixed(2) + ' A) · IHOLD ' + m.ihold +
      ' · ' + showValue('holdSeconds', d.holdSeconds) + ' · rehome ' + r.coarseRps + '/' + r.creepRps + ' rev/s');
    TMC.state.config.axes.forEach(function (a, i) {
      TMC.refreshInherited(i);
      TMC.refreshAmps(i);
      TMC.refreshSummaries(i);
    });
  };

  // Show one pane per configured stepper, first one active, and fill them.
  TMC.renderSteppers = function renderSteppers() {
    const axes = TMC.state.config.axes;
    const atLimit = axes.length >= TMC.MAX_AXES;
    document.getElementById('tmcNoSteppers').style.display = axes.length ? 'none' : '';
    TMC.renderAddStepper();
    TMC.state.prevNames = axes.map(function (a) { return a.name; });
    for (let i = 0; i < TMC.MAX_AXES; i++) {
      const used = i < axes.length;
      tab(i).classList.toggle('d-none', !used);
      tab(i).querySelector('.nav-link').classList.toggle('active', i === 0);
      pane(i).classList.toggle('show', i === 0);
      pane(i).classList.toggle('active', i === 0);
      if (used) fillPane(i);
    }
    TMC.renderSpi();
    TMC.renderStepperPanel();
    TMC.renderPresets();
    TMC.updateRemoveStepperButton();
  };

  // The Add buttons: say why a button is dead as the title (a disabled
  // button gets no hover events, so it goes on the span around Add stepper
  // as well).
  TMC.renderAddStepper = function renderAddStepper() {
    const atLimit = TMC.state.config.axes.length >= TMC.MAX_AXES;
    const add = document.getElementById('tmcAddStepper');
    const sim = document.getElementById('tmcAddSimulated');
    add.disabled = atLimit;
    sim.disabled = atLimit;
    const note = atLimit ? TMC.MAX_AXES + ' steppers is the maximum' : '';
    add.title = note;
    document.getElementById('tmcAddStepperWrap').title = note;
  };

  // Kept in sync with whichever tab is active (the shown.bs.tab listener).
  TMC.updateRemoveStepperButton = function updateRemoveStepperButton() {
    const btn = document.getElementById('tmcRemoveStepper');
    const active = document.querySelector('#tmcStepperPanes .tab-pane.active');
    const i = active ? parseInt(active.dataset.stepper, 10) : -1;
    const a = TMC.state.config.axes[i];
    btn.dataset.act = 'removeStepper';
    btn.dataset.stepper = i;
    btn.disabled = !a;
    btn.innerHTML = '<i class="fas fa-trash"></i> Remove ' + (a ? TMC.esc(a.name) : 'this stepper');
  };
  document.getElementById('tmcStepperTabs').addEventListener('shown.bs.tab', TMC.updateRemoveStepperButton);

  // The one-line digest on each shut section header.
  TMC.setSummary = function setSummary(i, key, text) {
    document.getElementById('tmcSum' + i + key).textContent = text;
  };

  TMC.refreshSummaries = function refreshSummaries(i) {
    const a = TMC.state.config.axes[i];
    if (!a) return;
    const e = TMC.effectiveStepper(i);
    const s = a.spi, m = e.motor, n = e.motion, r = e.rehome;
    // What is inherited reads as such, so a shut section says whether this
    // stepper differs from the rest.
    const own = function (p, text) { return TMC.inherits(a, p) ? text : text + '*'; };
    TMC.setSummary(i, 'hardware', (a.enabled === false ? 'disabled · ' : '') + own('driver', e.driver) + ' · ' +
      (s.fake ? 'simulator' : 'SPI' + s.bus + ' CS ' + s.cs) + ' · ' + TMC.channelSourceText(a));
    TMC.setSummary(i, 'motor', own('motor.irun', 'IRUN ' + m.irun + ' (' + TMC.rmsCurrent(e, m.irun).toFixed(2) + ' A)') + ' · ' +
      own('motor.ihold', 'IHOLD ' + m.ihold) + ' · ' + own('holdSeconds', showValue('holdSeconds', e.holdSeconds)) +
      ' · ' + own('motion.rampTimeSec', 'ramp ' + n.rampTimeSec + ' s to speed'));
    TMC.setSummary(i, 'rehome', (r.dir === 'cw' ? 'CW' : 'CCW') + ' · ' + own('rehome.coarseRps', 'coarse ' + r.coarseRps +
      ' rev/s') + ' · ' + own('rehome.creepRps', 'creep ' + r.creepRps + ' rev/s') + ' · AMAX ' + r.amax +
      (r.autoRecoverOnDemand ? ' · auto-recover on demand' : ' · auto-recover off') +
      (r.autoRecoverAtStartup ? ' + at startup' : ''));
    TMC.refreshStatusSummaries(i);
  };

  TMC.channelSourceText = function channelSourceText(a) {
    const c = a.channel || {};
    if (c.mode === 'model') return 'model ' + (c.model || '?') + (c.offset ? ' + ' + c.offset : '');
    if (c.mode === 'absolute') return 'channel ' + (c.number || '?');
    const n = parseInt(c.slot, 10);
    return n ? 'slot ' + n + ' (' + TMC.slotChannelText(n) + ')' : 'slot ?';
  };

  // Compares the channel source in the form against the one the running
  // fppd reports, field by field rather than by its description text.
  TMC.sameChannelSource = function sameChannelSource(a, snap) {
    if (snap.channelMode === undefined) return true; // an older daemon: no structured source
    const c = a.channel || {};
    const mode = c.mode === 'model' || c.mode === 'absolute' ? c.mode : 'slot';
    if (snap.channelMode !== mode) return false;
    if (mode === 'slot') return (snap.channelSlot || 0) === (parseInt(c.slot, 10) || 0);
    if (mode === 'model') {
      return snap.channelModel === (c.model || '') &&
             (snap.channelModelOffset || 0) === (parseInt(c.offset, 10) || 0);
    }
    return (snap.channelNumber || 0) === (parseInt(c.number, 10) || 0);
  };

  // A driver change in either form: a derived field still equal to the OLD
  // chip's value falls back to deriving from the new one, so a number typed
  // for a non-BOB board is never clobbered. For the global form every
  // stepper inheriting the driver is walked as well.
  function followDriver(obj, oldDriver) {
    TMC.scopePaths(['derived']).forEach(function (p) {
      const cur = parseFloat(TMC.getPath(obj, p));
      if (!isNaN(cur) && TMC.sameValue(cur, TMC.DRIVER_DEFAULTS[p][oldDriver])) TMC.deletePath(obj, p);
    });
  }

  // An edit in any pane. The panes are static, so this is wired once.
  function onEdit(ev) {
    const el = ev.target.closest('[data-path], [data-override]');
    if (!el) return;
    const i = parseInt(el.closest('.tab-pane').dataset.stepper, 10);
    const a = TMC.state.config.axes[i];
    if (!a) return;
    TMC.markDirty();

    if (el.dataset.override) { // the IRUN/IHOLD switch: on copies the effective value, off inherits
      const p = el.dataset.override;
      if (el.checked) TMC.setPath(a, p, TMC.resolve(a, p)); else TMC.deletePath(a, p);
      TMC.refreshInherited(i);
      TMC.refreshAmps(i);
      TMC.refreshSummaries(i);
      return;
    }
    const p = el.dataset.path;
    if (p === 'enabled') {
      // The file only spells out a disabled stepper.
      if (el.checked) delete a.enabled; else a.enabled = false;
      TMC.refreshSummaries(i);
      TMC.refreshStepperPanel();
      return;
    }
    if (p === 'spi.fake' && !el.checked && !a.spi.cs) {
      // Real again means a port: a stepper saved as simulated has none.
      Object.assign(a.spi, TMC.freeDefaultPort());
    }
    if (p === 'holdSeconds') {
      // Two controls: the mode select (global / hold indefinitely / release
      // after) and the seconds for the last of those.
      const mode = pane(i).querySelector('[data-hold="mode"]').value;
      const secs = pane(i).querySelector('[data-hold="secs"]');
      if (mode === '') TMC.deletePath(a, 'holdSeconds');
      else if (mode === 'hold') a.holdSeconds = null;
      else a.holdSeconds = parseInt(secs.value, 10) || 0;
      TMC.refreshInherited(i);
      TMC.refreshSummaries(i);
      return;
    }
    if (p === 'name') {
      // A preset scoped to this stepper is scoped by NAME, so the rename
      // carries it.
      const old = TMC.state.prevNames[i];
      if (old && old !== el.value) {
        Object.keys(TMC.state.config.presets).forEach(function (k) {
          if (TMC.state.config.presets[k].stepper === old) TMC.state.config.presets[k].stepper = el.value;
        });
      }
      TMC.state.prevNames[i] = el.value;
      tab(i).querySelector('.nav-link').textContent = el.value;
    }
    const scope = TMC.SCOPE[p];
    const oldDriver = (p === 'driver') ? TMC.resolve(a, 'driver') : null;
    if ((scope === 'inherit' || scope === 'derived') && el.value === '') {
      TMC.deletePath(a, p); // empty = inherit
    } else if (p === 'spi.bus') {
      TMC.setPath(a, p, parseInt(el.value, 10) || 0);
    } else {
      TMC.setPath(a, p, readControl(el));
    }
    if (oldDriver) {
      const newDriver = TMC.resolve(a, 'driver');
      if (newDriver !== oldDriver) { followDriver(a, oldDriver); fillPane(i); }
    }
    if (el.dataset.amps) document.getElementById(el.dataset.amps).textContent = TMC.rmsCurrent(TMC.effectiveStepper(i), readControl(el)).toFixed(2) + ' A RMS';
    if (p === 'name') { TMC.renderPresets(); TMC.renderStepperPanel(); }
    if (p === 'spi.fake' || p === 'spi.bus' || p === 'spi.cs' || p === 'name') {
      // A pin taken here is greyed out in every other pane, and the wiring
      // table names the steppers.
      TMC.state.config.axes.forEach(function (x, k) { fillPortSelects(k); TMC.refreshSummaries(k); });
      TMC.renderSpi();
    }
    if (p === 'driver' || p.indexOf('motor.') === 0) TMC.refreshAmps(i);
    if (p === 'channel.mode' && a.channel.mode === 'slot') {
      // Back onto the block: keep the slot it had unless another stepper
      // has taken it meanwhile.
      const n = parseInt(a.channel.slot, 10);
      if (!n || TMC.slotHolder(n, i) >= 0) a.channel.slot = TMC.freeSlot(i);
    }
    if (p === 'channel.slot' || p === 'channel.mode') TMC.refreshChannels();
    if (p.indexOf('channel.') === 0) TMC.refreshStepperPanel();
    TMC.applyVisibility(i);
    if (scope) TMC.refreshInherited(i);
    TMC.refreshSummaries(i);
  }
  const panes = document.getElementById('tmcStepperPanes');
  panes.addEventListener('input', onEdit);
  panes.addEventListener('change', onEdit);

  // An edit in the Global settings form: into the defaults block, then every
  // pane's placeholders follow.
  function onEditDefaults(ev) {
    const el = ev.target.closest('[data-path]');
    if (!el) return;
    const d = TMC.state.config.defaults;
    const p = el.dataset.path;
    TMC.markDirty();
    const oldDriver = (p === 'driver') ? d.driver : null;
    if (p === 'holdSeconds' && el.value === '') d.holdSeconds = null; // empty = hold indefinitely
    else if (TMC.SCOPE[p] === 'derived' && el.value === '') TMC.deletePath(d, p); // empty = the driver's
    else TMC.setPath(d, p, readControl(el));
    if (oldDriver && oldDriver !== d.driver) {
      followDriver(d, oldDriver);
      TMC.state.config.axes.forEach(function (a) {
        if (TMC.inherits(a, 'driver')) followDriver(a, oldDriver);
      });
      TMC.renderDefaults();
      TMC.state.config.axes.forEach(function (a, i) { fillPane(i); });
      return;
    }
    if (el.dataset.amps) document.getElementById(el.dataset.amps).textContent = TMC.rmsCurrent(d, readControl(el)).toFixed(2) + ' A RMS';
    TMC.refreshGlobal();
  }
  globalForm().addEventListener('input', onEditDefaults);
  globalForm().addEventListener('change', onEditDefaults);

  function addStepper(fake) {
    if (TMC.state.config.axes.length >= TMC.MAX_AXES) return;
    TMC.state.config.axes.push(TMC.defaultStepper(TMC.state.config.axes.length, fake));
    TMC.renderSteppers();
    TMC.markDirty();
    // Straight to the new stepper's tab: its settings are the next thing to fill in.
    const btn = tab(TMC.state.config.axes.length - 1).querySelector('button');
    bootstrap.Tab.getOrCreateInstance(btn).show();
  }
  document.getElementById('tmcAddStepper').addEventListener('click', function () { addStepper(false); });
  document.getElementById('tmcAddSimulated').addEventListener('click', function () { addStepper(true); });

})(window.TMC = window.TMC || {});
