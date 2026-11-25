/*
 * Stepper Motion -- presets.js
 *
 * The preset table and its modal editor.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";


  // Presets are keyed by channel value and shared unless scoped: preset.stepper is
  // absent or "all" for every stepper, or the name of the one stepper allowed
  // to run it. Scoping matters because a cycle's waypoints are absolute full
  // steps on one particular mechanism.
  TMC.presetScope = function presetScope(p) {
    return (p.stepper && p.stepper !== 'all') ? p.stepper : 'all';
  };


  TMC.stepperIndexByName = function stepperIndexByName(name) {
    for (let i = 0; i < TMC.state.config.axes.length; i++) {
      if (TMC.state.config.axes[i].name === name) return i;
    }
    return -1;
  };


  TMC.presetScopeKnown = function presetScopeKnown(scope) {
    return scope === 'all' || TMC.stepperIndexByName(scope) >= 0;
  };


  // The editor's Type select spells release out; the table has a Detail
  // column for that and keeps the Type column one word wide.
  TMC.PRESET_TYPE_LABELS = { goto: 'Go to', cycle: 'Cycle', rehome: 'Rehome', release: 'Release (stops powering)' };
  TMC.PRESET_TYPE_SHORT = { goto: 'Go to', cycle: 'Cycle', rehome: 'Rehome', release: 'Release' };


  // The one-line digest shown in the summary table.
  TMC.presetSummaryDetail = function presetSummaryDetail(p) {
    let s;
    if (p.type === 'cycle') {
      const n = (p.waypoints || []).length;
      const rep = p.repeat ? ('repeat ' + p.repeat + 'x') : 'repeats until the channel changes';
      s = n + ' waypoint' + (n === 1 ? '' : 's') + ', ' + rep;
    } else if (p.type === 'rehome') {
      s = 'Re-finds home and re-zeroes';
    } else if (p.type === 'release') {
      s = 'De-energises the coils';
    } else {
      s = (p.positionFullsteps !== undefined && p.positionPercent === undefined)
        ? p.positionFullsteps + ' full steps from home'
        : (p.positionPercent !== undefined ? p.positionPercent : 50) + '% of travel';
    }
    if (p.rps !== undefined && p.rps > 0) s += ' &middot; ' + p.rps + ' rev/s';
    if (p.rampTimeSec !== undefined && p.rampTimeSec > 0) {
      s += ' &middot; ' + p.rampTimeSec + ' s ramp';
    }
    return s;
  };


  TMC.renderPresets = function renderPresets() {
    const tbody = document.getElementById('tmcPresets');
    if (!tbody) return;
    TMC.disposeHelpTooltips(tbody);
    tbody.innerHTML = '';
    // Value 0 is built in (return home) and cannot be defined, renamed or
    // deleted; shown as a permanent first row.
    const home = document.createElement('tr');
    home.className = 'tmc-preset-builtin';
    home.innerHTML =
      '<td data-label="Value">0</td>' +
      '<td data-label="Name">Return home</td>' +
      '<td data-label="Applies to">All steppers</td>' +
      '<td data-label="Type">Home</td>' +
      // No actions on this row, so its detail spans the actions column too.
      '<td class="small text-body-secondary" colspan="2">Built in: a channel value of 0, or a silent channel, ' +
        'sends the stepper to position 0 &mdash; a plain move using the stored reference</td>';
    tbody.appendChild(home);
    Object.keys(TMC.state.config.presets).sort(function (x, y) { return x - y; }).forEach(function (k) {
      const p = TMC.state.config.presets[k];
      const scope = TMC.presetScope(p);
      const known = TMC.presetScopeKnown(scope);
      const tr = document.createElement('tr');
      tr.innerHTML =
        // data-label: the column name, shown beside the value when a phone
        // stacks the row (tmc.css).
        '<td data-label="Value">' + TMC.esc(k) + '</td>' +
        '<td data-label="Name">' + TMC.esc(p.name || '') + '</td>' +
        '<td data-label="Applies to">' + (scope === 'all' ? 'All steppers' : TMC.esc(scope)) +
          (known ? '' : ' <span class="badge text-bg-warning">no such stepper</span>') + '</td>' +
        '<td data-label="Type">' + TMC.esc(TMC.PRESET_TYPE_SHORT[p.type] || p.type) + '</td>' +
        '<td class="small text-body-secondary">' + TMC.presetSummaryDetail(p) + '</td>' +
        '<td class="text-end tmc-preset-actions"><div class="d-flex gap-1 justify-content-end align-items-center">' +
          '<button class="buttons btn-outline-success btn-sm tmc-run-btn" data-act="runPreset" ' +
            'style="min-width:6.5rem" data-value="' + TMC.esc(k) + '">' +
            '<i class="fas fa-play"></i> Run now</button>' +
          '<button class="buttons btn-secondary btn-sm" data-act="editPreset" data-value="' + TMC.esc(k) + '">' +
            '<i class="fas fa-pen"></i> Edit</button>' +
          '<button class="buttons btn-danger btn-sm tmcTip tmc-del-btn" data-act="delPreset" data-value="' + TMC.esc(k) + '" ' +
            'data-bs-title="Delete this preset.">&times;</button>' +
        '</div></td>';
      tbody.appendChild(tr);
    });
    TMC.renderPresetRunState();
    TMC.initHelpTooltips(tbody);
  };


  // ------------------------------------------------------ preset editor --
  // Modal-based editing, as FPP's other setup pages (Command Presets, GPIO)
  // do: the table is a read-only summary, Edit opens one form.

  TMC.state.pmEditingKey = null; // null while the modal is adding a new preset


  TMC.pmScopeOptionsHtml = function pmScopeOptionsHtml(scope) {
    let opts = '<option value="all"' + (scope === 'all' ? ' selected' : '') + '>All steppers</option>';
    TMC.state.config.axes.forEach(function (a) {
      opts += '<option value="' + TMC.esc(a.name) + '"' + (a.name === scope ? ' selected' : '') +
        '>' + TMC.esc(a.name) + '</option>';
    });
    if (scope !== 'all' && !TMC.presetScopeKnown(scope)) {
      opts += '<option value="' + TMC.esc(scope) + '" selected>' + TMC.esc(scope) + ' (no such stepper)</option>';
    }
    return opts;
  };


  // Type-specific fields, rebuilt into #tmcPmDetail whenever Type changes --
  // called both on modal open and from the Type select's change handler.
  TMC.renderPmDetail = function renderPmDetail(type, p) {
    p = p || {};
    const el = document.getElementById('tmcPmDetail');
    if (type === 'goto') {
      // usePercent is not in the JSON: AxisConfig.cpp derives it from which
      // of positionPercent/positionFullsteps is present, so the mode decides
      // which one goes out (see the tmcPmSave handler).
      const useSteps = p.positionFullsteps !== undefined && p.positionPercent === undefined;
      el.innerHTML = TMC.row('Position mode', 'Percent re-resolves against the current travel range every time this preset runs, so re-commissioning does not invalidate it. Full steps is an absolute position from home, like a cycle waypoint -- tied to this mechanism as currently commissioned, and silently wrong (not re-checked) if home is later re-declared elsewhere.',
        '<select class="form-select" id="tmcPmPositionMode">' +
          '<option value="percent"' + (useSteps ? '' : ' selected') + '>% of travel</option>' +
          '<option value="steps"' + (useSteps ? ' selected' : '') + '>Full steps from home</option>' +
        '</select>') +
        '<div id="tmcPmPositionPercentRow"' + (useSteps ? ' class="d-none"' : '') + '>' +
        TMC.row('Position', 'Where to move, as a percentage of the stepper&rsquo;s commissioned travel range. Fractions are accepted &mdash; the result is not rounded to a whole full step, so 50.5% is a real position.',
          '<div class="input-group">' +
            '<input type="number" min="0" max="100" step="0.1" class="form-control" ' +
              'id="tmcPmPositionPercent" ' +
              'value="' + (p.positionPercent !== undefined ? p.positionPercent : 50) + '">' +
            '<span class="input-group-text">% of travel</span></div>') +
        '</div>' +
        '<div id="tmcPmPositionFullstepsRow"' + (useSteps ? '' : ' class="d-none"') + '>' +
        TMC.row('Position', 'Full steps from home (0), like a cycle waypoint. <b>Full step is the unit, not a whole-number limit</b> &mdash; fractions are fine, and the jog buttons move in 0.1 of one. The driver resolves 1/256 of a full step, so anything this box accepts is reachable. Past the current far limit is refused when this preset runs, not when it is saved.',
          '<div class="input-group">' +
            '<input type="number" step="0.01" class="form-control" id="tmcPmPositionFullsteps" ' +
              'value="' + (p.positionFullsteps !== undefined ? p.positionFullsteps : 0) + '">' +
            '<span class="input-group-text">full steps</span>' +
            '<span class="input-group-text" id="tmcPmPositionDeg"></span></div>') +
        '</div>';
      // Degrees beside the box, so "full steps" is not misread as a
      // whole-step constraint.
      const fsInput = document.getElementById('tmcPmPositionFullsteps');
      const degOut = document.getElementById('tmcPmPositionDeg');
      const showDeg = function () {
        const v = parseFloat(fsInput.value);
        degOut.textContent = isNaN(v) ? '--' :
          '= ' + (v * TMC.DEG_PER_FULLSTEP).toFixed(2) + '\u00b0';
      };
      fsInput.addEventListener('input', showDeg);
      showDeg();

      document.getElementById('tmcPmPositionMode').addEventListener('change', function () {
        const steps = this.value === 'steps';
        document.getElementById('tmcPmPositionPercentRow').classList.toggle('d-none', steps);
        document.getElementById('tmcPmPositionFullstepsRow').classList.toggle('d-none', !steps);
      });
    } else if (type === 'cycle') {
      const wps = (p.waypoints || []).map(function (w) { return w.position + '@' + w.dwellMs; }).join(', ');
      el.innerHTML =
        TMC.row('Waypoints', 'Full steps on this mechanism, each held for its dwell time in milliseconds, then advancing to the next. Fractions are fine here too &mdash; 14.4 is a valid position. Since these are full steps rather than a percentage, a cycle only makes sense scoped to one stepper -- set Applies to above.',
          '<input class="form-control" id="tmcPmWaypoints" value="' + TMC.esc(wps) + '" placeholder="25@2000, 5@1000">' +
          '<div class="form-text">position@dwellMs, comma separated.</div>') +
        TMC.row('Repeat', 'How many times to run the full waypoint list. 0 means keep repeating for as long as the channel holds this preset&rsquo;s value.',
          '<input type="number" min="0" class="form-control" id="tmcPmRepeat" value="' + (p.repeat || 0) + '">');
    } else {
      el.innerHTML = '<div class="row"><div class="col-md-8 offset-md-4 col-lg-9 offset-lg-3 text-body-secondary small">' +
        (type === 'rehome'
          ? 'No parameters -- re-finds the mechanical stop and re-zeroes position 0, using that stepper&rsquo;s Rehome staging values.'
          : 'No parameters -- de-energises the coils right now. The stored position reference is kept.') +
        '</div></div>';
    }
    TMC.initHelpTooltips(el);
  };


  TMC.renderPmFields = function renderPmFields(k, p) {
    const scope = TMC.presetScope(p);
    TMC.disposeHelpTooltips(document.getElementById('tmcPmFields'));
    document.getElementById('tmcPmFields').innerHTML =
      TMC.row('Value', 'The byte value (1-255) a stepper&rsquo;s channel must carry to trigger this preset. Each value may be defined once.',
        '<input type="number" min="1" max="255" class="form-control" id="tmcPmValue" value="' + TMC.esc(k) + '">') +
      TMC.row('Name', '', '<input class="form-control" id="tmcPmName" value="' + TMC.esc(p.name || '') + '" placeholder="e.g. chop-fast">') +
      TMC.row('Applies to', 'Which stepper may run this preset. <b>All steppers</b> shares one definition across the lot -- right for a percentage-based move. Scope it to one stepper when the motion is specific to that mechanism, as a cycle&rsquo;s full-step waypoints always are.',
        '<select class="form-select" id="tmcPmScope">' + TMC.pmScopeOptionsHtml(scope) + '</select>') +
      TMC.row('Type', '',
        '<select class="form-select" id="tmcPmType">' +
          Object.keys(TMC.PRESET_TYPE_LABELS).map(function (t) {
            return '<option value="' + t + '"' + (p.type === t ? ' selected' : '') + '>' + TMC.PRESET_TYPE_LABELS[t] + '</option>';
          }).join('') +
        '</select>') +
      TMC.row('Speed override (rev/s)', 'Blank uses the stepper&rsquo;s own default speed.',
        '<input type="number" step="0.01" min="0" max="4" class="form-control" id="tmcPmRps" ' +
          'value="' + (p.rps !== undefined && p.rps > 0 ? p.rps : '') + '" placeholder="0.10">') +
      TMC.row('Ramp override (s)', 'How long this preset&rsquo;s moves get to reach their speed. Blank uses the stepper&rsquo;s own ramp time. Worth setting per preset because the right answer differs by move: a short hop needs a brief ramp to get anywhere at all, while a full-travel sweep is better off ramping gently. A shorter ramp costs torque margin, and a stepper that loses steps here says nothing about it &mdash; see Motor, current &amp; ramp.',
        '<input type="number" step="0.01" min="0.02" max="5" class="form-control" id="tmcPmRamp" ' +
          'value="' + (p.rampTimeSec !== undefined && p.rampTimeSec > 0 ? p.rampTimeSec : '') + '" ' +
          'placeholder="stepper default">') +
      '<div id="tmcPmDetail"></div>';
    TMC.initHelpTooltips(document.getElementById('tmcPmFields'));
    TMC.renderPmDetail(p.type || 'goto', p);
    document.getElementById('tmcPmType').addEventListener('change', function () {
      TMC.renderPmDetail(this.value, this.value === (p.type || 'goto') ? p : {});
    });
  };


  TMC.openPresetModal = function openPresetModal(k) {
    TMC.state.pmEditingKey = k || null;
    const p = k && TMC.state.config.presets[k] ? TMC.state.config.presets[k] : { type: 'goto' };
    document.getElementById('tmcPresetModalLabel').textContent = k ? 'Edit preset' : 'Add preset';
    document.getElementById('tmcPmAlert').innerHTML = '';
    TMC.renderPmFields(k || TMC.nextPresetValue(), p);
    bootstrap.Modal.getOrCreateInstance(document.getElementById('tmcPresetModal')).show();
  };


  TMC.nextPresetValue = function nextPresetValue() {
    let v = 1;
    while (TMC.state.config.presets[String(v)]) v++;
    return String(v);
  };


  TMC.pmAlert = function pmAlert(text) {
    document.getElementById('tmcPmAlert').innerHTML =
      '<div class="alert alert-warning py-2 small mb-2">' + TMC.esc(text) + '</div>';
  };


  document.getElementById('tmcPmSave').addEventListener('click', function () {
    const nv = parseInt(document.getElementById('tmcPmValue').value, 10);
    if (!(nv >= 1) || nv > 255) {
      TMC.pmAlert('Value must be between 1 and 255.'); return;
    }
    const nk = String(nv);
    if (TMC.state.config.presets[nk] && nk !== TMC.state.pmEditingKey) {
      TMC.pmAlert('Preset value ' + nk + ' is already used.'); return;
    }
    const type = document.getElementById('tmcPmType').value;
    const p = {
      name: document.getElementById('tmcPmName').value,
      type: type
    };
    const scope = document.getElementById('tmcPmScope').value;
    if (scope !== 'all') p.stepper = scope;
    const rps = parseFloat(document.getElementById('tmcPmRps').value);
    if (rps > 0) p.rps = Math.min(rps, 4);
    // Clamped to the same band AxisConfig.h enforces, so a value typed past the
    // input's advisory max is corrected here rather than refused on Save.
    const ramp = parseFloat(document.getElementById('tmcPmRamp').value);
    if (ramp > 0) p.rampTimeSec = Math.max(0.02, Math.min(ramp, 5));
    if (type === 'goto') {
      // Only one of these two may be present (see renderPmDetail()).
      if (document.getElementById('tmcPmPositionMode').value === 'steps') {
        p.positionFullsteps = parseFloat(document.getElementById('tmcPmPositionFullsteps').value) || 0;
      } else {
        let pct = parseFloat(document.getElementById('tmcPmPositionPercent').value) || 0;
        p.positionPercent = Math.max(0, Math.min(100, pct));
      }
    } else if (type === 'cycle') {
      p.waypoints = document.getElementById('tmcPmWaypoints').value.split(',').map(function (t) {
        const bits = t.split('@');
        return { position: parseFloat(bits[0]) || 0, dwellMs: parseInt(bits[1], 10) || 0 };
      }).filter(function (w) { return !isNaN(w.position); });
      p.repeat = parseInt(document.getElementById('tmcPmRepeat').value, 10) || 0;
    }
    // A preset commits on its own when this dialog closes. The WHOLE table
    // is saved, so a rename (a delete-and-add under a new key) round-trips
    // atomically.
    const candidate = Object.assign({}, TMC.state.config.presets);
    if (TMC.state.pmEditingKey && TMC.state.pmEditingKey !== nk) delete candidate[TMC.state.pmEditingKey];
    candidate[nk] = p;
    const btn = document.getElementById('tmcPmSave');
    btn.disabled = true;
    TMC.post(TMC.API + '?action=savePresets', { presets: candidate, revision: TMC.state.revision }).then(function (j) {
      btn.disabled = false;
      if (j.status !== 'OK') { TMC.pmAlert(TMC.saveErrorText(j, 'save failed')); return; }
      TMC.state.revision = j.revision;
      TMC.state.config.presets = candidate;
      TMC.renderPresets();
      TMC.notify('Preset saved.');
      bootstrap.Modal.getOrCreateInstance(document.getElementById('tmcPresetModal')).hide();
    });
  });


  // Which steppers this row's Run now drives: a scoped preset its one
  // stepper, an "All steppers" preset every one of them.
  TMC.presetTargets = function presetTargets(k) {
    const p = TMC.state.config.presets[k];
    if (!p) return [];
    const scope = TMC.presetScope(p);
    if (scope !== 'all') {
      const i = TMC.stepperIndexByName(scope);
      return i >= 0 ? [i] : [];
    }
    return TMC.state.config.axes.map(function (a, i) { return i; });
  };



  // Run now becomes Stop while the preset is running; the truth comes from
  // the status poll, so the button is right after a reload or a run
  // started from an FPP command.
  TMC.renderPresetRunState = function renderPresetRunState() {
    const st = TMC.state.status || {};

    // Which steppers are currently running each preset value. A shared preset
    // can be running on several at once, so this is a list per value.
    const running = {};
    (st.axes || []).forEach(function (snap) {
      const j = snap.job || {};
      const d = j.data || {};
      if (j.kind === 'runPreset' && j.state === 'running' && d.presetValue) {
        const key = String(d.presetValue);
        (running[key] = running[key] || []).push(snap.name);
      }
    });

    // The delete button sits in the row of Run now/Stop, so it is off while
    // anything, a manual run or the show, is running the preset.
    const active = {};
    (st.axes || []).forEach(function (snap) {
      if (snap.activePreset) active[String(snap.activePreset)] = true;
    });
    document.querySelectorAll('#tmcPresets .tmc-del-btn').forEach(function (btn) {
      const busy = !!(running[btn.dataset.value] || active[btn.dataset.value]);
      if (btn.disabled === busy) return;
      btn.disabled = busy;
      TMC.setTip(btn, busy ? 'Stop this preset before deleting it.' : 'Delete this preset.');
    });

    const list = function (names) {
      return names.length > 1
        ? names.slice(0, -1).join(', ') + ' and ' + names[names.length - 1]
        : names[0];
    };

    document.querySelectorAll('#tmcPresets .tmc-run-btn').forEach(function (btn) {
      const k = btn.dataset.value;
      const on = running[k] || [];
      // Only steppers fppd is actually running can be driven.
      const targets = on.length ? [] : TMC.presetTargets(k).filter(function (i) {
        return TMC.runningIndexFor(i) >= 0;
      });
      // Never ready while there are unsaved stepper edits: a preset that
      // leaves rps/irun unset falls back to whatever fppd last loaded.
      const ready = !TMC.state.configDirty && targets.length > 0;
      const names = targets.map(function (i) { return TMC.state.config.axes[i].name; });
      // Runs on every poll: touch the DOM only when the button changes
      // state, or its tooltip is torn down under the cursor.
      const state = on.length ? 'stop:' + on.join(',') :
        (ready ? 'run:' + names.join(',') : 'idle:' + TMC.state.configDirty);
      if (btn.dataset.state === state) return;
      btn.dataset.state = state;

      if (on.length) {
        btn.dataset.running = on.join(',');
        btn.className = 'buttons btn-danger btn-sm tmc-run-btn tmcTip';
        btn.innerHTML = '<i class="fas fa-stop"></i> Stop';
        btn.disabled = false;
        TMC.setTip(btn, 'Stop this preset on ' + list(on) + '.');
        return;
      }
      delete btn.dataset.running;
      btn.className = 'buttons btn-outline-success btn-sm tmc-run-btn tmcTip';
      btn.innerHTML = '<i class="fas fa-play"></i> Run now';
      btn.disabled = !ready;
      TMC.setTip(btn, ready
        ? 'Run this preset now on ' + list(names) + '.'
        : (TMC.state.configDirty
            ? 'Save the stepper configuration first -- this preset may fall back to motor or ' +
              'rehome settings that are not saved yet.'
            : (TMC.state.config.axes.length
                ? 'No stepper this preset applies to is running. Save the configuration and ' +
                  'restart fppd first.'
                : 'Add a stepper first -- a preset needs one to run on.')));
    });
  };


  document.getElementById('tmcAddPreset').addEventListener('click', function () {
    TMC.openPresetModal(null);
  });
})(window.TMC = window.TMC || {});
