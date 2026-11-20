(() => {
  // The microSD forms redirect here with a query saying what to follow;
  // reloading must not follow it again once it is over.
  if (window.location.search) history.replaceState(null, '', '/');
  const script = document.currentScript;
  const row = document.getElementById('sdcard-state');
  const status = document.getElementById('sdcard-status');
  const form = document.getElementById('sdcard-format');
  const benchForm = document.getElementById('sdcard-bench-form');
  const benchRow = document.getElementById('sdcard-bench-row');
  const benchText = document.getElementById('sdcard-bench');
  const benchCard = document.getElementById('sdcard-bench-card');
  const benchBus = document.getElementById('sdcard-bench-bus');
  const forms = [form, benchForm].filter((element) => element);
  const setBusy = (busy) => {
    forms.forEach((element) => {
      if (busy) element.setAttribute('aria-busy', 'true');
      else element.removeAttribute('aria-busy');
      element.querySelectorAll('button').forEach((button) => {
        button.disabled = busy;
      });
    });
  };
  // One click starts one format or benchmark; the page coming back from the
  // history gets its buttons back.
  forms.forEach((element) => element.addEventListener('submit', () => setBusy(true)));
  window.addEventListener('pageshow', () => setBusy(false));
  if (!script || !row || !status) return;
  // data-started: a format to follow until sdcard.sh writes a result other
  // than data-format, the one before it. data-bench: a benchmark.
  const formatting = script.hasAttribute('data-started');
  const benchmarking = !formatting && script.hasAttribute('data-bench');
  if (!formatting && !benchmarking) return;
  const before = Number(script.getAttribute('data-format') || '0');
  const started = Date.now();
  let state = null;
  let delay = 1500;
  const show = (text, error) => {
    status.textContent = text;
    status.classList.toggle('error', error);
    status.hidden = false;
  };
  const showForms = (card) => {
    if (form) form.hidden = card !== 'ready' && card !== 'unformatted';
    if (benchForm) benchForm.hidden = card !== 'ready';
  };
  const detail = (element, template, value) => {
    if (!element) return;
    element.textContent = value ? template.replace('%s', () => value) : '';
    element.hidden = !value;
  };
  const showBench = (card, bench) => {
    if (bench.status === 'running') {
      show(bench.text, false);
      return false;
    }
    showForms(card.state);
    show(bench.text, bench.status === 'failed');
    return true;
  };
  const showFormat = (card, result) => {
    showForms(card.state);
    if (result.result === 'ok' && card.state === 'ready') {
      show(card.size ? L.readySize.replace('%s', () => card.size).replace('%s', () => card.free) : L.ready, false);
    } else if (result.result === 'ok') {
      show(card.text, true);
    } else {
      const reason = typeof result.text === 'string' ? result.text : String(result.reason);
      show(L.failed.replace('%s', () => reason).replace('%s', () => card.text), true);
    }
  };
  const poll = () => {
    if (Date.now() - started >= 600000) {
      if (benchmarking) {
        show(L.benchTimeout, true);
        showForms(state || 'ready');
      } else {
        show(L.timeout, true);
      }
      return;
    }
    const check = new XMLHttpRequest();
    check.open('GET', '/storage/state?t=' + Date.now());
    check.timeout = 5000;
    const retry = () => {
      show(L.retrying, true);
      delay = Math.min(delay * 2, 15000);
      setTimeout(poll, delay);
    };
    check.onload = () => {
      if (check.status === 401) {
        window.location.replace('/');
        return;
      }
      let card = null;
      try {
        card = check.status === 200 ? JSON.parse(check.responseText) : null;
      } catch (error) {
        card = null;
      }
      if (!card || typeof card.state !== 'string' || typeof card.text !== 'string') {
        retry();
        return;
      }
      delay = 1500;
      state = card.state;
      row.textContent = card.text;
      if (benchmarking) {
        const bench = card.bench && typeof card.bench.status === 'string' &&
          typeof card.bench.text === 'string' ? card.bench : null;
        if (!bench) {
          showForms(card.state);
          status.hidden = true;
          return;
        }
        if (benchRow && benchText) {
          benchText.textContent = bench.text;
          benchRow.hidden = false;
        }
        detail(benchCard, L.card, typeof bench.card === 'string' ? bench.card : '');
        detail(benchBus, L.bus, typeof bench.bus === 'string' ? bench.bus : '');
        if (!showBench(card, bench)) setTimeout(poll, delay);
        return;
      }
      // camera-app formats in the background, and the card can read "ready"
      // before the format starts: only sdcard.sh's new result ends it.
      const result = card.format && typeof card.format.result === 'string' &&
        typeof card.format.at === 'number' ? card.format : null;
      if (result && result.at !== before) {
        showFormat(card, result);
        return;
      }
      show(L.formatting, false);
      setTimeout(poll, delay);
    };
    check.onerror = check.ontimeout = retry;
    check.send();
  };
  setTimeout(poll, delay);
})();
