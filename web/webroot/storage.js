(() => {
  const script = document.currentScript;
  const row = document.getElementById('sdcard-state');
  const status = document.getElementById('sdcard-status');
  const form = document.getElementById('sdcard-format');
  const benchForm = document.getElementById('sdcard-bench');
  const benchRow = document.getElementById('sdcard-bench-row');
  const benchText = document.getElementById('sdcard-bench');
  if (!script || !row || !status) return;
  const started = Date.now();
  // camera-app runs the format in the background: the page answering the
  // button can load before sdcard.sh reports "formatting".
  let seen = !script.hasAttribute('data-started');
  // The server reports a benchmark as running before the page loads.
  const benchmarking = script.hasAttribute('data-bench');
  let delay = 1500;
  const show = (text, error) => {
    status.textContent = text;
    status.classList.toggle('error', error);
    status.hidden = false;
  };
  const showForms = (state) => {
    if (form) form.hidden = state !== 'ready' && state !== 'unformatted';
    if (benchForm) benchForm.hidden = state !== 'ready';
  };
  const poll = () => {
    if (!benchmarking && Date.now() - started >= 600000) {
      show(L.timeout, true);
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
      if (benchmarking) {
        const bench = card.bench && typeof card.bench.status === 'string' &&
          typeof card.bench.text === 'string' ? card.bench : null;
        row.textContent = card.text;
        if (bench && benchRow && benchText) {
          benchText.textContent = bench.text;
          benchRow.hidden = false;
        }
        if (bench && bench.status === 'running') {
          show(bench.text, false);
          setTimeout(poll, delay);
          return;
        }
        showForms(card.state);
        if (bench) show(bench.text, bench.status === 'failed');
        else status.hidden = true;
        return;
      }
      if (card.state === 'formatting') seen = true;
      else if (!seen && Date.now() - started < 15000) {
        setTimeout(poll, delay);
        return;
      }
      row.textContent = card.text;
      if (card.state === 'formatting') {
        show(L.formatting, false);
        setTimeout(poll, delay);
        return;
      }
      showForms(card.state);
      if (card.state === 'ready') {
        show(card.size ? L.readySize.replace('%s', card.size).replace('%s', card.free) : L.ready, false);
      } else {
        show(L.failed.replace('%s', card.text), true);
      }
    };
    check.onerror = check.ontimeout = retry;
    check.send();
  };
  setTimeout(poll, delay);
})();
