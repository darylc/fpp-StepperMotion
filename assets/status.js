/*
 * Stepper Motion -- status.js
 *
 * Everything driven by the status poll: live rows, the gauge, job results.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";


  // ------------------------------------------------------------ status --

  // A small radial dial: the two hard stops drawn 180deg apart on screen
  // whatever the real travel, a filled arc from home to the current
  // position, and a needle. Colour reflects what the stepper is doing:
  // moving, holding, or in trouble. The position sits inside the arc.
  TMC.gaugeSvg = function gaugeSvg(snap) {
    const ref = snap.reference || {};
    const min = ref.minFullsteps, max = ref.maxFullsteps;
    const cx = 100, cy = 96, r = 78;
    if (max === undefined || min === undefined || max <= min) {
      return '<svg width="200" height="120" viewBox="0 0 200 120" class="tmc-gauge">' +
        '<text x="100" y="60" text-anchor="middle" class="tmc-gauge-muted" font-size="12">not commissioned</text>' +
        '</svg>';
    }
    const frac = Math.max(0, Math.min(1, (snap.positionFullsteps - min) / (max - min)));
    const angle = 180 - frac * 180; // 180deg = home (left), 0deg = far stop (right)
    const rad = angle * Math.PI / 180;
    // A short pointer inside the rim, not a spoke: the centre holds the
    // position text.
    const tipX = (cx + (r - 8) * Math.cos(rad)).toFixed(1);
    const tipY = (cy - (r - 8) * Math.sin(rad)).toFixed(1);
    const tailX = (cx + (r - 26) * Math.cos(rad)).toFixed(1);
    const tailY = (cy - (r - 26) * Math.sin(rad)).toFixed(1);
    const arcX = (cx + r * Math.cos(rad)).toFixed(1);
    const arcY = (cy - r * Math.sin(rad)).toFixed(1);

    let state = 'idle', label = 'idle';
    if (!snap.referenceValid) { state = 'unknown'; label = 'no reference'; }
    else if (snap.job && snap.job.state === 'running') { state = 'busy'; label = TMC.esc(snap.job.kind); }
    else if ((snap.activity || '').indexOf('home') >= 0) { state = 'busy'; label = TMC.esc(snap.activity); }
    if ((snap.problems || []).length) { state = 'fault'; label = 'fault'; }

    return '<svg width="200" height="120" viewBox="0 0 200 120" class="tmc-gauge tmc-gauge-' + state + '">' +
      '<path class="tmc-gauge-track" d="M ' + (cx - r) + ' ' + cy + ' A ' + r + ' ' + r + ' 0 0 1 ' + (cx + r) + ' ' + cy + '" />' +
      '<path class="tmc-gauge-fill" d="M ' + (cx - r) + ' ' + cy + ' A ' + r + ' ' + r + ' 0 0 1 ' + arcX + ' ' + arcY + '" />' +
      '<line class="tmc-gauge-needle" x1="' + tailX + '" y1="' + tailY + '" x2="' + tipX + '" y2="' + tipY + '" />' +
      '<text x="' + (cx - r) + '" y="' + (cy + 14) + '" text-anchor="middle" class="tmc-gauge-muted" font-size="10">home</text>' +
      '<text x="' + (cx + r) + '" y="' + (cy + 14) + '" text-anchor="middle" class="tmc-gauge-muted" font-size="10">far</text>' +
      '<text x="' + cx + '" y="' + (cy - 30) + '" text-anchor="middle" class="tmc-gauge-label" font-size="12">' + label + '</text>' +
      '<text x="' + cx + '" y="' + (cy - 6) + '" text-anchor="middle" class="tmc-gauge-pos" font-size="22">' +
        snap.positionDegrees.toFixed(1) + '&deg;</text>' +
      '<text x="' + cx + '" y="' + (cy + 8) + '" text-anchor="middle" class="tmc-gauge-muted" font-size="10">' +
        snap.positionFullsteps.toFixed(2) + ' fs of ' + min + '..' + max + '</text>' +
      '</svg>';
  };


  // ------------------------------------------------- the steppers panel --
  //
  // One card, one block per stepper: what it is doing and the controls that
  // change it, together.
  //
  // Structure is built ONCE and then only its volatile parts are updated:
  // the poll runs several times a second and rebuilding rows that carry
  // buttons would swap nodes under the pointer, drop focus and orphan
  // tooltips. renderPresetRunState() follows the same rule.

  const ACTS = [
    ['stop',    'fa-stop',      'btn-outline-danger',
     'Stop whatever this stepper is doing right now -- a jog, a preset, a rehome -- regardless of who started it.'],
    ['home',    'fa-house',     'btn-outline-success',
     'Send this stepper back to position 0 using its stored reference &mdash; the same place a channel value of 0 means. A plain move; it does <b>not</b> drive into the stop. Honours <em>Release when idle</em>.'],
    ['rehome',  'fa-crosshairs', 'btn-outline-warning',
     'Re-find the mechanical stop and redefine position 0 from it: coarse approach, back off, slow creep. Takes a few seconds and <b>drives deliberately into the hard stop</b>. Use it when the reference has been lost, or after something has moved the mechanism by hand.'],
    ['release', 'fa-power-off', 'btn-outline-secondary',
     'De-energise the coils right now. The stored position reference is kept &mdash; but on a sprung mechanism the shaft will be pulled off it, and the stepper will then refuse to move until it is re-homed.'],
  ];
  const JOGS = [-10, -1, -0.1, 0.1, 1, 10];

  function jogLabel(d) {
    const s = String(Math.abs(d));
    return (d < 0 ? '&minus;' : '+') + s;
  }

  function actionButtonsHtml(scope, i, inert) {
    // scope 'all' drives every running stepper; otherwise one stepper index.
    // inert omits data-act entirely, so a button with no action cannot be
    // wired up by mistake.
    return ACTS.map(function (a) {
      if (inert) {
        return '<button type="button" class="buttons btn-sm ' + a[2] + '" disabled>' +
          '<i class="fas ' + a[1] + '"></i></button>';
      }
      const act = (scope === 'all') ? a[0] + 'All' : a[0];
      const attr = (scope === 'all') ? '' : ' data-stepper="' + i + '"';
      const label = (scope === 'all')
        ? ' <span class="d-none d-sm-inline">' + a[0].charAt(0).toUpperCase() + a[0].slice(1) + '</span>'
        : '';
      return '<button type="button" class="buttons btn-sm ' + a[2] + ' tmcTip" data-bs-html="true" ' +
        'data-act="' + act + '"' + attr + ' data-bs-title="' + a[3] + '">' +
        '<i class="fas ' + a[1] + '"></i>' + label + '</button>';
    }).join('');
  }

  function jogButtonsHtml(i, inert) {
    return JOGS.map(function (d) {
      if (inert) {
        return '<button type="button" class="buttons btn-secondary btn-sm" disabled>' +
          jogLabel(d) + '</button>';
      }
      return '<button type="button" class="buttons btn-secondary btn-sm" data-act="jog" ' +
        'data-stepper="' + i + '" data-delta="' + d + '">' + jogLabel(d) + '</button>';
    }).join('');
  }

  // Rebuilt only when the stepper list itself changes.
  TMC.renderStepperPanel = function renderStepperPanel() {
    const el = document.getElementById('tmcSteppers');
    if (!el) return;
    TMC.disposeHelpTooltips(el);
    const axes = TMC.state.config.axes || [];

    // The all-steppers buttons sit in the card header (plugin_setup.php),
    // and only earn their place when there is more than one stepper.
    const allRow = document.getElementById('tmcAllRow');
    if (allRow) {
      TMC.disposeHelpTooltips(allRow);
      allRow.innerHTML = axes.length > 1 ? actionButtonsHtml('all') : '';
      TMC.initHelpTooltips(allRow);
    }
    if (!axes.length) {
      // Show the control surface anyway, inert, so the empty card still
      // says what the page is for.
      el.innerHTML =
        '<div class="border rounded px-2 py-2 mb-2 opacity-50" aria-hidden="true">' +
          '<div class="mb-1"><span class="fw-semibold me-2">No stepper configured</span>' +
            '<span class="badge text-bg-secondary">nothing to control yet</span></div>' +
          '<div class="d-flex flex-wrap align-items-center gap-2 mt-2">' +
            '<div class="btn-group btn-group-sm" role="group">' + jogButtonsHtml(0, true) + '</div>' +
            '<div class="d-flex flex-wrap gap-1">' + actionButtonsHtml('one', 0, true) + '</div>' +
          '</div>' +
        '</div>' +
        '<div class="small text-body-secondary">Add a stepper under <b>Stepper Configuration</b> ' +
        'below, save, then restart fppd. These controls become live once it is running.</div>';
      TMC.state.panelNames = [];
      return;
    }

    let html = '';
    // The stepper cards sit in a grid that packs as many across as the card
    // width allows (.tmc-stepper-grid); on a phone it collapses to one column.
    html += '<div class="tmc-stepper-grid">';
    // Card layout: name, badges and facts on the left, the gauge to their
    // right, the jog/action buttons on their own line underneath. The gauge
    // wraps beneath the detail block at phone width.
    axes.forEach(function (a, i) {
      // A flex column with the button row pinned to the bottom (mt-auto),
      // so the buttons line up across a row of cards of different heights.
      html += '<div class="border rounded px-2 py-2 d-flex flex-column" id="tmcRow' + i + '">' +
        '<div class="d-flex flex-wrap align-items-start gap-2">' +
          '<div class="flex-grow-1 tmc-live-detail">' +
            // The name on a line of its own, clipped with an ellipsis (full
            // name in the title) so a long one cannot shove the badges about.
            '<div class="fw-semibold text-truncate tmc-row-name" id="tmcRowName' + i + '" title="' + TMC.esc(a.name) + '">' + TMC.esc(a.name) + '</div>' +
            '<div class="mb-1" id="tmcBadges' + i + '"></div>' +
            '<div class="small font-monospace text-body-secondary" id="tmcFacts' + i + '"></div>' +
            // The status line: what the stepper is doing and, on the same
            // line, why (a lost reference, any problem).
            '<div class="small mt-1 tmc-row-status" id="tmcActivity' + i + '"></div>' +
          '</div>' +
          '<div class="flex-shrink-0 ms-auto" id="tmcGauge' + i + '"></div>' +
        '</div>' +
        '<div class="d-flex flex-wrap align-items-center gap-2 mt-auto pt-1">' +
          '<div class="btn-group btn-group-sm" role="group" id="tmcJogGroup' + i + '">' + jogButtonsHtml(i) + '</div>' +
          '<div class="d-flex flex-wrap gap-1">' + actionButtonsHtml('one', i) + '</div>' +
        '</div>' +
      '</div>';
    });
    html += '</div>';
    el.innerHTML = html;
    TMC.state.panelNames = axes.map(function (a) { return a.name; });
    TMC.initHelpTooltips(el);
    TMC.refreshStepperPanel();
  };

  // Called on every poll. Touches text and disabled flags only.
  TMC.refreshStepperPanel = function refreshStepperPanel() {
    const axes = TMC.state.config.axes || [];
    // The stepper list changed under us (added, removed, renamed): rebuild.
    const names = axes.map(function (a) { return a.name; });
    const prev = TMC.state.panelNames || [];
    if (names.length !== prev.length || names.some(function (n, k) { return n !== prev[k]; })) {
      TMC.renderStepperPanel();
      return;
    }

    const st = TMC.state.status;
    let anyRunning = false;

    axes.forEach(function (a, i) {
      const snap = st && st.axes
        ? st.axes.find(function (x) { return x.name === a.name; }) : null;
      if (TMC.runningIndexFor(i) >= 0) anyRunning = true;

      const set = function (id, html) {
        const n = document.getElementById(id + i);
        if (n && n.innerHTML !== html) n.innerHTML = html;
      };

      set('tmcGauge', snap ? TMC.gaugeSvg(snap) : '');

      // A stepper fppd is not running (saved, awaiting a restart) is dimmed
      // as a whole. Its buttons stay live: a press answers with the restart
      // message.
      const row = document.getElementById('tmcRow' + i);
      if (row) row.classList.toggle('tmc-row-inert', !snap);

      // The jog group's title states the clamp rule live.
      const jog = document.getElementById('tmcJogGroup' + i);
      if (jog) {
        const r = snap && snap.reference ? snap.reference : {};
        const t = !snap ? 'Jog in full steps' :
          (r.maxFullsteps > r.minFullsteps
            ? 'Jog in full steps, clamped to ' + r.minFullsteps + '..' + r.maxFullsteps
            : 'Jog in full steps -- unclamped until Set Far Limit has run; it will drive into the mechanical stops');
        if (jog.title !== t) jog.title = t;
      }

      if (!snap && a.enabled === false) {
        set('tmcBadges', '<span class="badge text-bg-secondary me-1">disabled</span>');
        set('tmcFacts', 'reading ' + TMC.esc(TMC.channelSourceText(a)));
        set('tmcActivity', '<span class="text-body-secondary">disabled &mdash; turn on Enabled in its General section, save and restart fppd to bring it up</span>');
      } else if (!snap) {
        set('tmcBadges', '<span class="badge text-bg-secondary me-1">not running</span>');
        set('tmcFacts', 'reading ' + TMC.esc(TMC.channelSourceText(a)));
        set('tmcActivity', '<span class="text-body-secondary">not running &mdash; save and restart fppd to bring this stepper up</span>');
      } else {
        const ref = snap.reference || {};
        let b = '<span class="badge ' + (snap.linkUp ? 'text-bg-success' : 'text-bg-danger') +
          ' me-1">' + (snap.linkUp ? 'link up' : 'link down') + '</span>';
        b += '<span class="badge ' + (snap.referenceValid ? 'text-bg-success' : 'text-bg-warning') +
          ' me-1" title="' + (snap.referenceValid
            ? 'Position reference valid: ' + TMC.esc(ref.homeMode || '') + (ref.homedAt ? ' at ' + TMC.esc(ref.homedAt) : '')
            : 'No position reference -- the stepper will not move until it is rehomed') + '">' +
          (snap.referenceValid ? 'sync' : 'no sync') + '</span>';
        if (snap.simulated) b += '<span class="badge text-bg-info me-1">simulator</span>';
        if (snap.manualMode) b += '<span class="badge text-bg-secondary me-1">manual</span>';
        if (snap.drvEnnHigh) b += '<span class="badge text-bg-danger me-1">DRV_ENN high</span>';
        if (snap.sdModeHigh) b += '<span class="badge text-bg-danger me-1">SD_MODE high</span>';
        // Channel-source state. A source change only takes effect when fppd
        // rebuilds its outputs, so say when one is waiting for a restart.
        if (a.enabled === false) {
          b += '<span class="badge text-bg-warning me-1">disabled, stops at the next fppd restart</span>';
        }
        if (!TMC.sameChannelSource(a, snap)) {
          b += '<span class="badge text-bg-warning me-1" title="Running: ' +
            TMC.esc(snap.channelSource) + '">saved, waiting for an fppd restart</span>';
        } else if (!snap.resolvedChannel) {
          b += '<span class="badge text-bg-danger me-1">channel unresolved</span>';
        }
        set('tmcBadges', b);

        // A model-backed channel says which model as well as the resolved
        // number: the number moves with the model.
        const src = (a.channel && a.channel.mode === 'model')
          ? ' (' + TMC.esc(TMC.channelSourceText(a)) + ')' : '';
        // Current: the coil current the chip is regulating to right now
        // (CS_ACTUAL through the datasheet formula), which steps down from
        // IRUN to IHOLD after IHOLDDELAY at standstill.
        const eff = TMC.effectiveStepper(i);
        const nowAmps = eff ? TMC.rmsCurrent(eff, snap.csActual) : NaN;
        set('tmcFacts',
          'ch ' + (snap.resolvedChannel ? snap.resolvedChannel : '?') + src +
            ' = ' + snap.channelValue +
            (snap.activePreset ? ' · preset ' + snap.activePreset : '') +
          ' · <span title="DRV_STATUS ' + TMC.esc(snap.drvStatus) + ' · CS_ACTUAL ' + snap.csActual +
            (snap.standstill ? ' · standstill' : '') + ' · GSTAT ' + TMC.esc(snap.gstat) + ' · IOIN ' + TMC.esc(snap.ioin) + '">current ' +
            (isNaN(nowAmps) ? 'CS&nbsp;' + snap.csActual : nowAmps.toFixed(2) + '&nbsp;A') + '</span>');

        // Status colour follows the gauge: red with a problem, yellow with
        // no reference. The reference reason is appended unless the activity
        // string ("idle -- <why>") already carries it.
        const probs = snap.problems || [];
        const cls = probs.length ? 'text-danger' : (!snap.referenceValid ? 'text-warning' : 'text-body-secondary');
        let line = TMC.esc(snap.activity);
        if (!snap.referenceValid && snap.referenceWhy &&
            (snap.activity || '').indexOf(snap.referenceWhy) < 0) {
          line += ' &mdash; ' + TMC.esc(snap.referenceWhy);
        }
        probs.forEach(function (p) { line += ' &mdash; ' + TMC.esc(p); });
        set('tmcActivity', '<span class="' + cls + '">' + line + '</span>');
      }

      // Per-stepper buttons stay ENABLED when the stepper is not running: a
      // click gets a specific alert from stepperAction().
    });

    // The all-steppers buttons ARE disabled when nothing is running.
    const allRow = document.getElementById('tmcAllRow');
    if (allRow) {
      allRow.querySelectorAll('button[data-act]').forEach(function (btn) {
        if (btn.disabled !== !anyRunning) btn.disabled = !anyRunning;
      });
    }
  };


  // The jog caption says live whether jogs are clamped, and to what.
  TMC.renderJogClamp = function renderJogClamp(i, snap) {
    const el = document.getElementById('jogClamp' + i);
    if (!el || !snap) return;
    const ref = snap.reference || {};
    if (ref.maxFullsteps > ref.minFullsteps) {
      el.innerHTML = 'clamped to ' + ref.minFullsteps + '..' + ref.maxFullsteps +
        ' full steps &mdash; press <strong>Probe</strong> to go past that';
    } else {
      el.innerHTML = '<strong>unclamped</strong> &mdash; the jog will drive into the mechanical stops; jog in small steps';
    }
  };

  TMC.renderJob = function renderJob(i, snap) {
    TMC.renderJogClamp(i, snap);
    const el = document.getElementById('job' + i);
    if (!el || !snap || !snap.job || !snap.job.id) return;
    const j = snap.job;
    let cls = 'secondary';
    if (j.state === 'running') cls = 'info';
    else if (j.state === 'done') cls = 'success';
    else if (j.state === 'failed') cls = 'danger';
    else if (j.state === 'stopped') cls = 'warning';

    let body = '<div class="alert alert-' + cls + ' py-2 mb-0">' +
      '<div class="d-flex gap-2 align-items-center">' +
        '<strong>' + TMC.esc(j.kind) + '</strong>' +
        (j.state === 'running' ? '<div class="spinner-border spinner-border-sm"></div>' : '') +
        '<span class="ms-auto small">' + TMC.esc(j.state) + '</span></div>';
    if (j.message) body += '<div class="small mt-1">' + TMC.esc(j.message) + '</div>';

    const d = j.data || {};
    if (d.commandedFullsteps !== undefined) {
      body += '<div class="small mt-1">commanded ' + d.commandedFullsteps.toFixed(1) +
        ' fs, counted ' + d.actualFullsteps.toFixed(2) + ' fs</div>';
    }
    if (d.counterErrorFullsteps !== undefined) {
      // The driver's own counter: it cannot see lost steps, so never say so.
      const ok = Math.abs(d.counterErrorFullsteps) < 0.01;
      body += '<div class="small mt-1">step counter after ' + d.cycles + ' cycles: <strong>' +
        (ok ? 'as commanded' : 'off by ' + d.counterErrorFullsteps.toFixed(3) + ' fs') + '</strong>' +
        (ok ? '' : ' (' + d.counterErrorDegrees.toFixed(2) + '&deg;): a move did not finish') + '</div>' +
        '<div class="small mt-1">The counter cannot see lost steps: check that the mechanism still meets your marks at both ends.</div>';
    }
    if (d.travelDegrees !== undefined) {
      body += '<div class="small mt-1">travel: ' + d.travelFullsteps + ' full steps (' +
        d.travelDegrees.toFixed(1) + '&deg;)</div>';
    }
    if (d.rmsCurrentAmps !== undefined) {
      body += '<div class="small mt-1">IRUN ' + d.irun + ' ≈ ' +
        d.rmsCurrentAmps.toFixed(2) + ' A RMS</div>';
    }
    (j.log || []).forEach(function (l) {
      body += '<div class="small font-monospace text-body-secondary">' + TMC.esc(l) + '</div>';
    });
    body += '</div>';
    el.innerHTML = body;
  };


  // The job panel lives inside Commissioning, shut by default, so a failed
  // job is also raised as an alert once, as it finishes.
  TMC.seenJobs = {};

  TMC.state.jobBaselineSeen = false;

  TMC.noteJobResult = function noteJobResult(a, snap) {
    if (!snap || !snap.job || !snap.job.id) return;
    const j = snap.job;
    const key = a.name + ':' + j.id + ':' + j.state;
    // The first poll after a page load is a baseline, not news: fppd
    // remembers the last job on each axis across page loads.
    if (!TMC.state.jobBaselineSeen) {
      TMC.seenJobs[key] = true;
      return;
    }
    if (j.state === 'running' || TMC.seenJobs[key]) return;
    TMC.seenJobs[key] = true;
    if (j.state !== 'failed') return;
    const msg = j.message || '';
    // A job that failed for the standing reason the status row already
    // shows in red needs no second voice.
    const alreadyShown = (snap.problems || []).some(function (p) {
      return p && msg && (p.indexOf(msg) >= 0 || msg.indexOf(p) >= 0);
    });
    if (alreadyShown) return;
    TMC.alertBox('danger', a.name + ': ' + j.kind + ' failed — ' + (msg || 'no reason given'));
  };


  // Every motion control in Commissioning refuses while the driver is not
  // answering, so the section says so up front.
  TMC.refreshCommWarning = function refreshCommWarning(i, snap) {
    const el = document.getElementById('commWarn' + i);
    if (!el) return;
    if (!snap || snap.linkUp) { el.innerHTML = ''; return; }
    el.innerHTML = '<div class="alert alert-danger py-2 mb-0 small">' +
      '<strong>This driver is not answering over SPI.</strong> Jog, Set Home, Test travel ' +
      'and Rehome will all refuse until it does. All-zero reads normally mean VS (motor ' +
      'supply) is off — the chip runs its own VCC from an internal regulator off VS, so ' +
      'logic-side power alone is not enough. Otherwise check MISO/MOSI/SCK/CS wiring and ' +
      'that the bus and chip select under General are the pins it is wired to, then press Probe.' +
      '</div>';
  };

  // The commissioning section's header digest comes from what the running
  // stepper reports, so it is refreshed from the poll rather than the form.
  TMC.refreshStatusSummaries = function refreshStatusSummaries(i) {
    const a = TMC.state.config.axes[i];
    if (!a) return;
    const st = TMC.state.status;
    const snap = st && st.axes ? st.axes.find(function (x) { return x.name === a.name; }) : null;
    if (!snap) { TMC.setSummary(i, 'commissioning', 'not running'); return; }
    const ref = snap.reference || {};
    TMC.setSummary(i, 'commissioning',
      (ref.maxFullsteps > ref.minFullsteps)
        ? 'range ' + ref.minFullsteps + '..' + ref.maxFullsteps + ' fs (' +
          (ref.travelDegrees || 0).toFixed(1) + '°)'
        : 'not commissioned');
  };


  TMC.poll = function poll() {
    fetch(TMC.API + '?action=status').then(function (r) { return r.json(); }).then(function (st) {
      TMC.state.status = st;
      if (st.configErrors && st.configErrors.length && !TMC.state.shownErrors) {
        TMC.state.shownErrors = true;
        st.configErrors.forEach(function (e) { TMC.alertBox('danger', 'Configuration: ' + e); });
      }
      if (st.structuralChangePending && !TMC.state.shownRestart) TMC.restartAlert();
      TMC.state.config.axes.forEach(function (a, i) {
        const snap = (st.axes || []).find(function (x) { return x.name === a.name; });
        TMC.renderJob(i, snap);
        TMC.refreshCommWarning(i, snap);
        TMC.noteJobResult(a, snap);
        TMC.refreshStatusSummaries(i);
      });
      TMC.state.jobBaselineSeen = true;
      TMC.refreshStepperPanel();
      TMC.renderPresetRunState();
    }).catch(function () {
      // fppd not answering (restarting, or not running). The page stays usable.
    }).finally(function () {
      TMC.state.pollTimer = setTimeout(TMC.poll, document.hidden ? 2000 : 300);
    });
  };


})(window.TMC = window.TMC || {});
