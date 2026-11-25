/*
 * Stepper Motion -- core.js
 *
 * Shared helpers: alerts, escaping, FPP's label/field row and section
 * chrome, tooltip lifecycle, and the one fetch wrapper.
 *
 * Loaded by plugin_setup.php in a fixed order; every cross-module symbol
 * hangs off the shared TMC object, and mutable page state lives in TMC.state
 * so a reassignment in one module is seen by the others.
 */
(function (TMC) {
  "use strict";

  // Mutable page state, declared here because core.js loads first; fields
  // are populated by main.js's load().
  TMC.state = TMC.state || {};
  // The status poll's most recent answer, and two "already said this once"
  // latches.
  TMC.state.status = null;
  TMC.state.shownErrors = false;
  TMC.state.shownRestart = false;

  TMC.DEG_PER_FULLSTEP = 1.8;

  // Speed for a move nothing gave one to (DEFAULT_MANUAL_RPS in src/AxisConfig.h).
  TMC.DEFAULT_RPS = 0.10;

  // The one restart alert, whether raised by a save that answered
  // restartNeeded or by a poll that finds structuralChangePending.
  TMC.restartAlert = function restartAlert() {
    TMC.state.shownRestart = true;
    TMC.alertBox('warning', 'A stepper\u2019s channel, SPI bus, chip select or driver type changed, or a ' +
      'stepper was added or removed. fppd builds its channel output at startup, so it needs ' +
      'to restart before that takes effect \u2014 the configuration is saved; use the restart ' +
      'prompt at the top of the page.');
    if (typeof SetRestartFlag === 'function') SetRestartFlag(1);
  };

  TMC.alertBox = function alertBox(kind, text) {
    const el = document.getElementById('tmcAlerts');
    // The same failure pressed four times is one alert with a count.
    // Matched on the stored text: a multi-line refusal is not expressible
    // in an attribute selector.
    const existing = Array.from(el.querySelectorAll('[data-alert-text]')).find(function (d) {
      return d.dataset.alertText === text;
    });
    if (existing) {
      const n = (parseInt(existing.dataset.alertCount, 10) || 1) + 1;
      existing.dataset.alertCount = n;
      const badge = existing.querySelector('.tmc-alert-count');
      badge.textContent = '×' + n;
      badge.classList.remove('d-none');
      return;
    }
    const div = document.createElement('div');
    div.dataset.alertText = text;
    div.dataset.alertCount = 1;
    div.className = 'alert alert-' + kind + ' alert-dismissible fade show';
    div.innerHTML = '<span class="tmc-alert-text"></span> <span class="badge text-bg-secondary tmc-alert-count d-none"></span>' +
      '<button type="button" class="btn-close" data-bs-dismiss="alert"></button>';
    div.querySelector('span').textContent = text;
    el.appendChild(div);
    if (kind === 'success' || kind === 'info') {
      setTimeout(function () { div.remove(); }, 6000);
    }
  };


  // Transient good news goes through FPP's own toast, the way every other page
  // reports a save. Anything the user may need to act on stays in the inline
  // alert box, where it survives until dismissed.
  TMC.notify = function notify(text, state) {
    if (window.jQuery && window.jQuery.jGrowl) {
      window.jQuery.jGrowl(text, { themeState: state || 'success' });
      return;
    }
    TMC.alertBox(state === 'danger' ? 'danger' : 'success', text);
  };


  TMC.esc = function esc(s) {
    const d = document.createElement('div');
    d.textContent = (s === undefined || s === null) ? '' : String(s);
    return d.innerHTML;
  };


  // ------------------------------------------------------ help tooltips --

  // The same glyph and markup PrintToolTip() emits in www/common.php. The
  // text goes in data-bs-title rather than title because this markup is
  // injected after fpp.js's page-load SetupToolTips() sweep.
  TMC.helpIcon = function helpIcon(text) {
    if (!text) return '';
    const t = String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/'/g, '&apos;');
    return " <span class='tmcHelpIcon' data-bs-html='true' " +
      "data-bs-placement='auto' data-bs-title='" + t + "'>" +
      "<img src='images/redesign/help-icon.svg' class='icon-help' alt='help icon'></span>";
  };


  // Bootstrap tracks a tooltip's popup outside the DOM node, so blowing away
  // innerHTML while one is showing leaves an orphan. Dispose before every
  // re-render, as DisposeArgHelpTooltips() in fpp.js does.
  TMC.disposeHelpTooltips = function disposeHelpTooltips(root) {
    if (typeof bootstrap === 'undefined' || !bootstrap.Tooltip || !root) return;
    root.querySelectorAll('.tmcHelpIcon, .tmcTip').forEach(function (el) {
      const existing = bootstrap.Tooltip.getInstance(el);
      if (existing) existing.dispose();
    });
  };


  // Retarget an existing tooltip. Setting the attribute alone does nothing once
  // Bootstrap has read it, so the instance is told directly.
  TMC.setTip = function setTip(el, text) {
    el.setAttribute('data-bs-title', text);
    if (typeof bootstrap === 'undefined' || !bootstrap.Tooltip) return;
    const existing = bootstrap.Tooltip.getInstance(el);
    if (existing) existing.setContent({ '.tooltip-inner': text });
    else {
      try { new bootstrap.Tooltip(el); } catch (e) {}
    }
  };


  TMC.initHelpTooltips = function initHelpTooltips(root) {
    if (typeof bootstrap === 'undefined' || !bootstrap.Tooltip) return;
    (root || document).querySelectorAll('.tmcHelpIcon, .tmcTip').forEach(function (el) {
      const existing = bootstrap.Tooltip.getInstance(el);
      if (existing) existing.dispose();
      try {
        const tip = new bootstrap.Tooltip(el);
        // Clicking a button leaves focus on it, which would keep the tooltip
        // up over whatever is next to it. Dismiss on click rather than
        // dropping the focus trigger, which keeps these reachable from the
        // keyboard.
        el.addEventListener('click', function () { tip.hide(); });
      } catch (e) {
        console.warn('StepperMotion: skipping invalid tooltip', el, e);
      }
    });
  };


  // ---------------------------------------------------- layout helpers --

  // FPP's label/field row: see PrintSetting() in www/common.php and the
  // hand-written equivalents in co-ledPanels.php.
  TMC.row = function row(label, help, fieldHtml) {
    return '<div class="row">' +
      '<div class="printSettingLabelCol col-md-4 col-lg-3"><b>' + label + ':</b>' + TMC.helpIcon(help) + '</div>' +
      '<div class="printSettingFieldCol col-md-8 col-lg-9">' + fieldHtml + '</div></div>';
  };


  // ------------------------------------------------------------- wiring --

  TMC.setPath = function setPath(obj, path, value) {
    const parts = path.split('.');
    let o = obj;
    for (let n = 0; n < parts.length - 1; n++) {
      if (o[parts[n]] === undefined) o[parts[n]] = {};
      o = o[parts[n]];
    }
    o[parts[parts.length - 1]] = value;
  };


  // A yes/no question in FPP's own modal dialog (DoModalDialog, as FPP's
  // pages use), resolving true on Yes. No, Escape or the close button settle
  // it false. yesText and noText rename the two buttons. text is plain; newlines become paragraph breaks.
  // FPP binds `close` only when it first builds a dialog id, so it calls
  // whichever question is open now rather than the first one asked.
  let openConfirm = null;
  TMC.confirm = function confirm(title, text, yesClass, yesText, noText) {
    if (openConfirm) openConfirm(false);
    return new Promise(function (resolve) {
      const settle = function (yes) {
        if (openConfirm !== settle) return;
        openConfirm = null;
        CloseModalDialog('tmcConfirm');
        resolve(yes);
      };
      openConfirm = settle;
      const buttons = {};
      buttons[yesText || 'Yes'] = { click: function () { settle(true); }, class: yesClass || 'btn-success' };
      buttons[noText || 'No'] = function () { settle(false); };
      DoModalDialog({
        id: 'tmcConfirm',
        class: 'modal-m',
        backdrop: true,
        keyboard: true,
        title: TMC.esc(title),
        body: String(text).split('\n\n').map(function (p) { return '<p>' + TMC.esc(p) + '</p>'; }).join(''),
        buttons: buttons,
        close: function () { if (openConfirm) openConfirm(false); }
      });
    });
  };


  // ---------------------------------------------------------- requests --

  TMC.post = function post(url, body) {
    return fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: body ? JSON.stringify(body) : '{}'
    }).then(function (r) {
      // A non-JSON answer is Apache speaking for a daemon that is not there
      // (502/503) or a plugin that is not loaded (404), never the plugin.
      return r.json().catch(function () {
        return { status: 'ERROR', httpStatus: r.status,
                 message: 'fppd is not answering (HTTP ' + r.status + ')' };
      });
    });
  };
})(window.TMC = window.TMC || {});
