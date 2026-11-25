/*
 * Stepper Motion -- main.js
 *
 * Page state, load/save, and boot.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";


  TMC.state.config = { idleTimeoutMs: 500, channelBlock: { source: 'channel', start: 1 },
                       defaults: TMC.builtinDefaults(), axes: [], presets: {} };

  TMC.state.pollTimer = null;

  // Pixel overlay model names, from FPP. null until fetched (or if fppd is not
  // answering), which the model picker degrades to a plain text field for.
  TMC.state.overlayModels = null;
  // name -> {StartChannel (1-based), ChannelCount}, for the channel block.
  TMC.state.overlayModelInfo = null;

  // The SPI buses this FPP knows and whether the box has each, from
  // ?action=config. null until loaded (bind.js assumes SPI0 only).
  TMC.state.spiBuses = null;


  // Stepper names as they were when the pane was last rendered, so a rename can
  // carry the presets scoped to the old name with it.
  TMC.state.prevNames = [];


  // Whether per-axis config holds edits the page Save button has not yet
  // sent (presets commit on their own via ?action=savePresets). Shown as a
  // flag because Run now uses fppd's copy, i.e. whatever was last saved.
  TMC.state.configDirty = false;


  TMC.markDirty = function markDirty() {
    if (TMC.state.configDirty) return;
    TMC.state.configDirty = true;
    TMC.updateDirtyUi();
  };


  TMC.markClean = function markClean() {
    if (!TMC.state.configDirty) return;
    TMC.state.configDirty = false;
    TMC.updateDirtyUi();
  };


  TMC.updateDirtyUi = function updateDirtyUi() {
    const banner = document.getElementById('tmcUnsaved');
    if (banner) {
      // Bootstrap's display utilities, not the hidden attribute: .d-flex is
      // !important and beats [hidden].
      banner.classList.toggle('d-none', !TMC.state.configDirty);
      banner.classList.toggle('d-flex', TMC.state.configDirty);
    }
    TMC.renderPresetRunState();
  };


  // -------------------------------------------------------- load / save --

  // The full list, not ?simple=true: the channel block shows where a model
  // starts and warns when it is too short.
  TMC.loadModels = function loadModels() {
    return fetch('api/models')
      .then(function (r) { return r.json(); })
      .then(function (list) {
        if (!Array.isArray(list)) return;
        TMC.state.overlayModels = list.map(function (m) { return m.Name; }).sort();
        TMC.state.overlayModelInfo = {};
        list.forEach(function (m) {
          TMC.state.overlayModelInfo[m.Name] = { StartChannel: m.StartChannel, ChannelCount: m.ChannelCount };
        });
      })
      .catch(function () { /* fppd down: the picker falls back to a text field */ });
  };


  // ------------------------------------------------ FPP's output entry --
  // FPP builds the plugin's channel output from its entry in co-other.json
  // (the Channel Outputs page's Misc tab). fppd keeps that entry's channels
  // in step with the saved channel block itself (TmcManager::
  // syncOutputEntry), but leaves its Active switch to the user, and puts a
  // deleted entry back only at the next save or fppd start -- so the page
  // says when it is off, missing or doubled. no-store: an older FPP can
  // leave a browser holding a stale copy of this response.
  TMC.checkOutputEntry = function checkOutputEntry() {
    const el = document.getElementById('tmcOutputEntry');
    return fetch('api/channel/output/co-other', { cache: 'no-store' }).then(function (r) { return r.json(); }).catch(function () { return null; }).then(function (d) {
      // A failed read says nothing either way; 404 is a box with no Misc outputs.
      if (!d || (!Array.isArray(d.channelOutputs) && !/not found/i.test(d.status || ''))) { el.classList.add('d-none'); return; }
      const outs = Array.isArray(d.channelOutputs) ? d.channelOutputs : [];
      const mine = outs.filter(function (o) { return o.type === TMC.OUTPUT_TYPE; });
      const where = 'FPP&rsquo;s channel output entry for this plugin (Channel Outputs page, <em>Misc</em> tab)';
      const haveSteppers = ((TMC.state.config || {}).axes || []).length > 0;
      let msg = '';
      if (mine.length > 1) msg = 'There is more than one ' + where.replace('FPP&rsquo;s channel output entry', 'channel output entry') + '; only the first works. Delete the others.';
      else if (!mine.length && haveSteppers) msg = where + ' is missing, so the steppers will not follow a show. Save configuration (or restart fppd) puts it back.';
      else if (mine.length && !mine[0].enabled) msg = where + ' is switched off, so the steppers will not follow a show.';
      el.innerHTML = msg;
      el.classList.toggle('d-none', !msg);
    });
  };


  // The page loads and saves the configuration only through fppd, so when it is
  // not answering there is nothing to load and nowhere to save: say so once
  // and keep Save shut until it comes back.
  TMC.daemonDown = function daemonDown() {
    TMC.alertBox('warning', 'fppd is not running \u2014 start it to configure steppers.');
    document.getElementById('tmcSave').disabled = true;
    document.getElementById('tmcUnsavedSave').disabled = true;
  };


  TMC.load = function load() {
    fetch(TMC.API + '?action=config').then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (j) {
      if (j.status !== 'OK') { TMC.alertBox('danger', j.message || 'could not load configuration'); return; }
      const raw = j.config || {};
      TMC.state.config = raw;
      // Sent back with every save: fppd refuses one made against an older
      // configuration (another tab, or a homing direction fppd stored).
      TMC.state.revision = j.revision;
      if (!TMC.state.config.idleTimeoutMs) TMC.state.config.idleTimeoutMs = 500;
      TMC.state.config.presets = TMC.state.config.presets || {};
      TMC.state.config.channelBlock = raw.channelBlock || { source: 'channel', start: 1 };
      // The defaults block, resolved by the daemon, and the steppers with
      // only their own fields.
      TMC.state.config.defaults = TMC.normaliseDefaults(raw.defaults, j.defaultsEffective);
      TMC.state.config.axes = (raw.axes || []).map(TMC.normaliseStepper);
      TMC.state.spiBuses = Array.isArray(j.spiBuses) ? j.spiBuses : null;
      document.getElementById('tmcIdleTimeoutMs').value = TMC.state.config.idleTimeoutMs;
      document.getElementById('tmcSave').disabled = false;
      document.getElementById('tmcUnsavedSave').disabled = false;
      TMC.renderDefaults();
      TMC.renderSteppers();
      TMC.fillBlockForm();
      TMC.checkOutputEntry();
      TMC.markClean();
    }).catch(TMC.daemonDown);
  };


  document.getElementById('tmcReload').addEventListener('click', TMC.load);

  document.getElementById('tmcIdleTimeoutMs').addEventListener('input', TMC.markDirty);

  // The Channels section: page state, then every slot list follows.
  function onBlockEdit() {
    const b = TMC.state.config.channelBlock;
    b.source = document.getElementById('tmcBlockSource').value;
    b.start = parseInt(document.getElementById('tmcBlockStart').value, 10) || 1;
    b.model = document.getElementById('tmcBlockModel').value;
    TMC.markDirty();
    TMC.refreshChannels();
    TMC.refreshStepperPanel();
  }
  ['tmcBlockSource', 'tmcBlockStart', 'tmcBlockModel'].forEach(function (id) {
    document.getElementById(id).addEventListener('input', onBlockEdit);
    document.getElementById(id).addEventListener('change', onBlockEdit);
  });


  // The document to save, from a copy of the page state. Only the channel
  // fields that mean something for the chosen mode go out (the form leaves
  // the other mode's value behind). The defaults block goes out whole and
  // each stepper with only its own fields, as page state holds them.
  TMC.saveDocument = function saveDocument() {
    const cfg = JSON.parse(JSON.stringify(TMC.state.config));
    const b = cfg.channelBlock || {};
    cfg.channelBlock = b.source === 'model'
      ? { source: 'model', model: b.model || '' }
      : { source: 'channel', start: parseInt(b.start, 10) || 1 };
    (cfg.axes || []).forEach(function (a) {
      const c = a.channel || {};
      if (c.mode === 'model') {
        a.channel = { mode: 'model', model: c.model || '', offset: parseInt(c.offset, 10) || 0 };
      } else if (c.mode === 'absolute') {
        a.channel = { mode: 'absolute', number: parseInt(c.number, 10) || 1 };
      } else {
        a.channel = { mode: 'slot', slot: parseInt(c.slot, 10) || 0 };
      }
    });
    return cfg;
  };


  // A refused save comes back with every problem the daemon found; shown
  // as one alert.
  TMC.saveErrorText = function saveErrorText(j, fallback) {
    const errs = Array.isArray(j.errors) ? j.errors : [];
    if (errs.length > 1) return errs.length + ' problems:\n\u2022 ' + errs.join('\n\u2022 ');
    return errs[0] || j.message || fallback;
  };


  document.getElementById('tmcSave').addEventListener('click', function () {
    TMC.state.config.idleTimeoutMs = parseInt(document.getElementById('tmcIdleTimeoutMs').value, 10) || 500;
    const body = { config: TMC.saveDocument(), revision: TMC.state.revision };
    TMC.post(TMC.API + '?action=saveConfig', body).then(function (j) {
      if (j.httpStatus && !j.errors) { TMC.daemonDown(); return; }
      if (j.status !== 'OK') { TMC.alertBox('danger', TMC.saveErrorText(j, 'save failed')); return; }
      TMC.state.revision = j.revision;
      TMC.markClean();
      TMC.checkOutputEntry();
      if (j.message) TMC.alertBox('warning', j.message);
      // shownRestart is reset before the alert so a save that clears a
      // pending change lets a later one alert again, while a save that
      // needs a restart shows the one alert and stops the poll repeating it.
      TMC.state.shownRestart = false;
      TMC.state.shownErrors = false;
      if (j.restartNeeded) {
        TMC.restartAlert();
      } else {
        TMC.notify('Saved. Tuning values applied live.');
      }
    }).catch(TMC.daemonDown);
  });


  document.getElementById('tmcUnsavedSave').addEventListener('click', function () {
    document.getElementById('tmcSave').click();
  });


  TMC.initHelpTooltips(document.getElementById('tmcRoot'));

  TMC.loadModels().then(TMC.load);

  TMC.poll();
})(window.TMC = window.TMC || {});
