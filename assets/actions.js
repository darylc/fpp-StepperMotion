/*
 * Stepper Motion -- actions.js
 *
 * Requests that make a stepper move, and the page's click routing.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";


  // The running stepper is matched to the configured one by NAME, not
  // position: an index means something different after the list is edited.
  TMC.runningIndexFor = function runningIndexFor(i) {
    const st = TMC.state.status;
    if (!st || !st.axes || !TMC.state.config.axes[i]) return -1;
    for (let n = 0; n < st.axes.length; n++) {
      if (st.axes[n].name === TMC.state.config.axes[i].name) return st.axes[n].index;
    }
    return -1;
  };


  // The one way anything reaches a stepper: ?action=&axis= on a single
  // endpoint (see API_BASE_PATH in src/TmcApi.cpp for why no REST subpaths).
  //
  // opts.stopFirst  send a stop and wait before the real action
  // opts.retries    re-issue on ERROR, 250ms apart
  // opts.label      names the action in the failure alert ("Could not home X")
  // opts.quiet      no alert when the stepper simply is not running
  //
  // Home and Rehome need stopFirst + retries: park is refused while a job is
  // running (a repeat:0 cycle runs until stopped), and Stop takes effect at
  // the worker's next poll, so the action is retried until the axis has let
  // go.
  TMC.stepperAction = function stepperAction(i, action, body, opts) {
    opts = opts || {};
    const ri = TMC.runningIndexFor(i);
    const name = (TMC.state.config.axes[i] || {}).name || ('stepper ' + i);
    if (ri < 0) {
      if (!opts.quiet && (TMC.state.config.axes[i] || {}).enabled === false) {
        TMC.alertBox('warning', 'Stepper "' + name + '" is disabled. Turn on Enabled in its ' +
          'General section, save and restart fppd first.');
      } else if (!opts.quiet) {
        TMC.alertBox('warning', 'Stepper "' + name + '" is not running. Save the ' +
          'configuration and restart fppd first.');
      }
      return Promise.resolve({});
    }
    const url = TMC.API + '?action=' + encodeURIComponent(action) + '&axis=' + ri;
    function attempt(triesLeft) {
      return TMC.post(url, body).then(function (j) {
        if (j.status !== 'ERROR') return j;
        if (triesLeft > 0) {
          return new Promise(function (resolve) {
            setTimeout(function () { resolve(attempt(triesLeft - 1)); }, 250);
          });
        }
        TMC.alertBox('danger',
          (opts.label ? 'Could not ' + opts.label + ' ' + name + ': ' : '') +
          (j.message || 'request failed'));
        return j;
      });
    }
    const retries = opts.retries || 0;
    if (!opts.stopFirst) return attempt(retries);
    return TMC.post(TMC.API + '?action=stop&axis=' + ri, {})
      .then(function () { return attempt(retries); });
  };

  // The show-facing buttons. Both stop whatever is running first.
  TMC.homeStepper = function homeStepper(i) {
    return TMC.stepperAction(i, 'park', {},
      { stopFirst: true, retries: 8, quiet: true, label: 'home' });
  };

  TMC.rehomeStepper = function rehomeStepper(i) {
    return TMC.stepperAction(i, 'rehome', {},
      { stopFirst: true, retries: 8, quiet: true, label: 'rehome' });
  };

  // One jog, used by both the Commissioning buttons and the Manual Control
  // card (which differ only in whether an unrunnable stepper is skipped
  // quietly). fppd keeps track of where the jogs went, for Set Home.
  TMC.jog = function jog(i, delta, opts) {
    // The effective currents: the stepper's own or the global ones.
    const a = TMC.effectiveStepper(i);
    if (!a) return Promise.resolve({});
    const rps = parseFloat((document.getElementById('rps' + i) || {}).value) || TMC.DEFAULT_RPS;
    return TMC.stepperAction(i, 'jog', {
      deltaFullsteps: delta, rps: rps, irun: a.motor.irun, ihold: a.motor.ihold
    }, opts);
  };


  // Resolves with the finished job (the axis's job record once it is no
  // longer running), or null if it has not finished in timeoutMs. For the
  // quick jobs whose result the page acts on: Set Home and Set Far Limit.
  TMC.waitJob = function waitJob(i, jobId, timeoutMs) {
    const ri = TMC.runningIndexFor(i);
    if (ri < 0 || !jobId) return Promise.resolve(null);
    const until = Date.now() + (timeoutMs || 15000);
    function poll() {
      return fetch(TMC.API + '?action=status&axis=' + ri)
        .then(function (r) { return r.json(); })
        .catch(function () { return {}; })
        .then(function (s) {
          const j = s.job || {};
          if (j.id === jobId && j.state !== 'running') return j;
          if (Date.now() > until) return null;
          return new Promise(function (resolve) { setTimeout(resolve, 300); }).then(poll);
        });
    }
    return poll();
  };


  // Show the homing direction fppd saved (Set Home, or a Set Far Limit flip)
  // in the form. Already in config.json, so this does not mark the page
  // dirty.
  TMC.showSavedHomingDir = function showSavedHomingDir(i, dir, d) {
    const a = TMC.state.config.axes[i];
    if (!a || (dir !== 'ccw' && dir !== 'cw')) return;
    a.rehome = a.rehome || {};
    a.rehome.dir = dir;
    // That save gave the file a new revision. Adopt it only if the page held
    // the one it replaced; otherwise the next save is refused as stale,
    // which is right.
    if (d && d.revisionBefore === TMC.state.revision && typeof d.revision === 'number')
      TMC.state.revision = d.revision;
    const sel = document.querySelector('#tmcStepper' + i + ' [data-path="rehome.dir"]');
    if (sel) sel.value = dir;
    TMC.refreshSummaries(i);
  };

  TMC.dirName = function dirName(dir) {
    return dir === 'cw' ? 'Clockwise' : 'Counter Clockwise';
  };



  document.addEventListener('click', function (ev) {
    const btn = ev.target.closest('[data-act]');
    if (!btn) return;
    const act = btn.dataset.act;

    if (act === 'stopAll' || act === 'homeAll' || act === 'rehomeAll' || act === 'releaseAll') {
      const targets = TMC.state.config.axes
        .map(function (a, i) { return i; })
        .filter(function (i) { return TMC.runningIndexFor(i) >= 0; });
      if (!targets.length) {
        TMC.alertBox('warning', 'No stepper is running. Save the configuration and restart fppd first.');
        return;
      }
      const fn = {
        stopAll: function (i) { return TMC.stepperAction(i, 'stop', {}, { quiet: true }); },
        homeAll: TMC.homeStepper,
        rehomeAll: TMC.rehomeStepper,
        releaseAll: function (i) { return TMC.stepperAction(i, 'release', {}, { quiet: true }); },
      }[act];
      targets.forEach(fn);
      return;
    }
    if (act === 'delPreset') {
      const k = btn.dataset.value;
      const p = TMC.state.config.presets[k];
      if (!p) return;
      const what = [p.name, TMC.PRESET_TYPE_SHORT[p.type] || p.type,
        TMC.presetSummaryDetail(p).replace(/&middot;/g, '-')].filter(Boolean).join(', ');
      TMC.confirm('Delete preset ' + k, 'Delete preset ' + k + ' (' + what + ')?\n\n' +
        'This is saved immediately and cannot be undone.',
        'btn-danger', 'Delete', 'Cancel').then(function (yes) {
        if (!yes) return;
        // Commits immediately, like a preset save.
        const candidate = Object.assign({}, TMC.state.config.presets);
        delete candidate[k];
        TMC.post(TMC.API + '?action=savePresets', { presets: candidate, revision: TMC.state.revision }).then(function (j) {
          if (j.status !== 'OK') { TMC.alertBox('danger', TMC.saveErrorText(j, 'delete failed')); return; }
          TMC.state.revision = j.revision;
          TMC.state.config.presets = candidate;
          TMC.renderPresets();
          // Its delete button is disabled while a preset runs, but a run may
          // have started behind the question. Stop a manual "Run now" of the
          // deleted preset, or its Stop button
          // vanishes while the axis keeps running it.
          const st = TMC.state.status || {};
          (st.axes || []).forEach(function (snap) {
            const rj = snap.job || {};
            const d = rj.data || {};
            if (rj.kind === 'runPreset' && rj.state === 'running' && String(d.presetValue) === k) {
              const ai = TMC.stepperIndexByName(snap.name);
              if (ai >= 0) TMC.stepperAction(ai, 'stop');
            }
          });
        });
      });
      return;
    }
    if (act === 'editPreset') { TMC.openPresetModal(btn.dataset.value); return; }
    if (act === 'runPreset') {
      // The same button stops every run it started.
      if (btn.dataset.running) {
        btn.dataset.running.split(',').forEach(function (name) {
          const ai = TMC.stepperIndexByName(name);
          if (ai >= 0) TMC.stepperAction(ai, 'stop', {}, { quiet: true });
        });
        return;
      }
      const targets = TMC.presetTargets(btn.dataset.value)
        .filter(function (i) { return TMC.runningIndexFor(i) >= 0; });
      if (!targets.length) {
        TMC.alertBox('warning', 'No running stepper this preset applies to. Add one, point the ' +
          'preset at a stepper that exists, or restart fppd.');
        return;
      }
      // A shared preset runs on every stepper at once -- the same thing that
      // happens in a show when several channels carry its value.
      targets.forEach(function (i) {
        TMC.stepperAction(i, 'runPreset', { value: parseInt(btn.dataset.value, 10) },
                          { quiet: targets.length > 1 });
      });
      return;
    }

    const i = parseInt(btn.dataset.stepper, 10);
    const a = TMC.state.config.axes[i];
    if (!a) return;
    const rps = parseFloat((document.getElementById('rps' + i) || {}).value) || TMC.DEFAULT_RPS;

    if (act === 'removeStepper') {
      TMC.confirm('Remove stepper', 'Remove stepper "' + a.name + '" from the configuration?',
                  'btn-danger').then(function (yes) {
        if (!yes) return;
        TMC.state.config.axes.splice(i, 1);
        TMC.markDirty();
        TMC.renderSteppers();
        TMC.alertBox('info', 'Stepper removed. Save, then restart fppd for it to stop running.');
      });
      return;
    }

    if (act === 'stop') { TMC.stepperAction(i, 'stop'); return; }
    const e = TMC.effectiveStepper(i); // the currents this stepper actually gets
    if (act === 'probe') { TMC.stepperAction(i, 'probe', { irun: e.motor.irun, ihold: e.motor.ihold }); return; }
    if (act === 'jog') { TMC.jog(i, parseFloat(btn.dataset.delta)); return; }
    if (act === 'setHome') {
      // No direction sent: fppd works it out from where the jogs went and
      // saves it as this stepper's homing direction if it differs.
      TMC.stepperAction(i, 'setHome', { minFullsteps: 0, maxFullsteps: 0 }).then(function (r) {
        if (r.status !== 'OK') return;
        return TMC.waitJob(i, r.jobId).then(function (j) {
          if (!j || j.state !== 'done') return; // a failure is raised by the status poll
          const d = j.data || {};
          let msg = 'Home set.';
          if (d.approachDir) {
            TMC.showSavedHomingDir(i, d.approachDir, d);
            msg += ' Homing direction: ' + TMC.dirName(d.approachDir) +
              (d.approachFrom === 'jogs' ? ', from the way you jogged into the stop'
                : d.approachFrom === 'config' ? ' (the configured one: no jogs to judge by)' : '') +
              (d.configUpdated ? ' -- saved.' : '.');
          }
          // Cycle waypoints are absolute full steps from position 0, so
          // re-declaring home leaves this axis's cycles pointing somewhere
          // else; nothing re-checks that.
          const hasCycles = Object.keys(TMC.state.config.presets || {}).some(function (k) {
            const p = TMC.state.config.presets[k];
            return p.type === 'cycle' && TMC.presetScope(p) === a.name;
          });
          if (hasCycles) {
            msg += ' This stepper has Cycle presets -- their waypoints are absolute ' +
              'full steps from home, so re-check them against the new zero.';
          }
          TMC.alertBox('info', msg + ' Now jog to the other end and press Set Far Limit.');
        });
      });
      return;
    }
    if (act === 'setMax') {
      const setMax = function (flip) {
        return TMC.stepperAction(i, 'setMax', flip ? { flip: 1 } : {}).then(function (r) {
          if (r.status !== 'OK') return null;
          return TMC.waitJob(i, r.jobId);
        });
      };
      setMax(false).then(function (j) {
        if (!j) return null;
        const d = j.data || {};
        if (j.state === 'failed' && d.wrongSide) {
          // Asked here instead of alerted by the status poll.
          TMC.seenJobs[a.name + ':' + j.id + ':' + j.state] = true;
          return TMC.confirm('Far limit is behind home', j.message + '.\n\nHome was set at a stop, so the far end ' +
              'cannot be past it: the direction home was approached in was recorded the ' +
              'wrong way round. Flip it and set the far limit here?').then(function (yes) {
            return yes ? setMax(true) : null;
          });
        }
        return j;
      }).then(function (j) {
        if (!j || j.state !== 'done') return;
        const d = j.data || {};
        let msg = 'Far limit set. Jog is now clamped to the range; run Test travel, then Rehome.';
        if (d.flipped) {
          TMC.showSavedHomingDir(i, d.approachDir, d);
          msg = 'Homing direction flipped to ' + TMC.dirName(d.approachDir) +
            (d.configUpdated ? ' and saved' : '') + '. ' + msg;
        }
        TMC.alertBox('info', msg);
      });
      return;
    }
    if (act === 'release') { TMC.stepperAction(i, 'release'); return; }
    if (act === 'home') { TMC.homeStepper(i); return; }
    // Stop-first, same as the all-steppers button.
    if (act === 'rehome') { TMC.rehomeStepper(i); return; }
    if (act === 'testTravel') {
      const cycles = parseInt((document.getElementById('cycles' + i) || {}).value, 10) || 1;
      TMC.stepperAction(i, 'testTravel', { cycles: cycles, rps: rps,
                                    irun: e.motor.irun, ihold: e.motor.ihold });
      return;
    }
  });

})(window.TMC = window.TMC || {});
