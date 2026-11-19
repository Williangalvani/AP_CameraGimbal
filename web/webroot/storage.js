(() => {
  const script = document.currentScript;
  const row = document.getElementById('sdcard-state');
  const status = document.getElementById('sdcard-status');
  const form = document.getElementById('sdcard-format');
  if (!script || !row || !status) return;
  const started = Date.now();
  // camera-app runs the format in the background: the page answering the
  // button can load before sdcard.sh reports "formatting".
  let seen = !script.hasAttribute('data-started');
  let delay = 1500;
  const show = (text, error) => {
    status.textContent = text;
    status.classList.toggle('error', error);
    status.hidden = false;
  };
  const poll = () => {
    if (Date.now() - started >= 600000) {
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
      if (form) form.hidden = card.state !== 'ready' && card.state !== 'unformatted';
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
