(() => {
  const script = document.currentScript;
  const video = document.getElementById('live-video');
  const stream = document.getElementById('live-stream');
  const status = document.getElementById('live-status');
  const enable = document.getElementById('manual-enable');
  const controlStatus = document.getElementById('control-status');
  const rate = document.getElementById('live-rate');
  const rateValue = document.getElementById('live-rate-value');
  const zoom = document.getElementById('live-zoom');
  const zoomValue = document.getElementById('live-zoom-value');
  const attitudeDial = document.getElementById('attitude-dial');
  const attitudeWorld = document.getElementById('attitude-world');
  const attitudeHeading = document.getElementById('attitude-heading');
  const attitudeRoll = document.getElementById('attitude-roll');
  const attitudePitch = document.getElementById('attitude-pitch');
  const attitudeYaw = document.getElementById('attitude-yaw');
  const attitudeRollRate = document.getElementById('attitude-roll-rate');
  const attitudePitchRate = document.getElementById('attitude-pitch-rate');
  const attitudeYawRate = document.getElementById('attitude-yaw-rate');
  const attitudeStatus = document.getElementById('attitude-status');
  if (!script || !video || !stream || !status || !enable || !controlStatus || !rate || !rateValue || !zoom || !zoomValue || !attitudeDial || !attitudeWorld || !attitudeHeading || !attitudeRoll || !attitudePitch || !attitudeYaw || !attitudeRollRate || !attitudePitchRate || !attitudeYawRate || !attitudeStatus) return;
  let retry = null;
  let startedAt = 0;
  let session = null;
  // Not streaming: the tab is hidden, or the viewer paused (userPaused).
  let stopped = false, userPaused = false;
  const streamUrl = () => '/live/video' + (Number(stream.value) + 1) + '.mp4?start=' + Date.now();
  // The stream is one fragment per frame, so MSE can play it as it arrives.
  // A plain <video src> buffers ~0.7 s of it before Chrome starts playing.
  const useMse = script.dataset.mse === '1' && !!(window.MediaSource && window.ReadableStream && MediaSource.isTypeSupported('video/mp4; codecs="avc1.640033"'));
  const MSE_TARGET = 0.1, MSE_SEEK = 0.6, MSE_KEEP = 10;
  const boxEnd = (bytes, type) => {
    for (let at = 0; at + 8 <= bytes.length;) {
      const size = ((bytes[at] << 24) | (bytes[at + 1] << 16) | (bytes[at + 2] << 8) | bytes[at + 3]) >>> 0;
      if (size < 8) return -1;
      if (String.fromCharCode(bytes[at + 4], bytes[at + 5], bytes[at + 6], bytes[at + 7]) === type)
        return at + size <= bytes.length ? at + size : 0;
      at += size;
    }
    return 0;
  };
  const avcCodec = bytes => {
    for (let at = 0; at + 8 <= bytes.length; at++) {
      if (bytes[at] === 0x61 && bytes[at + 1] === 0x76 && bytes[at + 2] === 0x63 && bytes[at + 3] === 0x43)
        return 'avc1.' + [bytes[at + 5], bytes[at + 6], bytes[at + 7]].map(b => b.toString(16).padStart(2, '0')).join('');
    }
    return 'avc1.640033';
  };
  const join = (a, b) => { const out = new Uint8Array(a.length + b.length); out.set(a); out.set(b, a.length); return out; };
  const playMse = async current => {
    const response = await fetch(streamUrl(), {cache: 'no-store', signal: current.abort.signal});
    // The session is gone (a reboot clears them); retrying cannot succeed.
    if (response.status === 401) { window.location.replace('/login'); return; }
    if (!response.ok || !response.body) throw new Error('HTTP ' + response.status);
    const reader = response.body.getReader();
    let head = new Uint8Array(0), end = 0;
    while (!(end = boxEnd(head, 'moov'))) {
      const {value, done} = await reader.read();
      if (done) throw new Error(L.streamEnded);
      head = join(head, value);
    }
    if (end < 0) throw new Error(L.errorDecode);
    const source = new MediaSource();
    video.src = URL.createObjectURL(source);
    await new Promise(resolve => source.addEventListener('sourceopen', resolve, {once: true}));
    URL.revokeObjectURL(video.src);
    if (session !== current) return;
    const buffer = source.addSourceBuffer('video/mp4; codecs="' + avcCodec(head.subarray(0, end)) + '"');
    const queue = [head];
    let ended = false, trimmed = false;
    const buffered = () => buffer.buffered.length > 0;
    const pump = () => {
      if (buffer.updating || session !== current || source.readyState !== 'open') return;
      if (buffered() && video.currentTime - buffer.buffered.start(0) > 2 * MSE_KEEP) {
        buffer.remove(0, video.currentTime - MSE_KEEP); return;
      }
      if (!queue.length) { if (ended) source.endOfStream(); return; }
      const chunk = queue.length === 1 ? queue[0] : queue.reduce(join);
      queue.length = 0; queue.push(chunk);
      try {
        buffer.appendBuffer(chunk);
      } catch (error) {
        // A full buffer: drop all but the last second played, then retry once.
        const keep = video.currentTime - 1;
        if (error.name !== 'QuotaExceededError' || trimmed || !buffered() || keep <= buffer.buffered.start(0)) throw error;
        trimmed = true; buffer.remove(0, keep); return;
      }
      trimmed = false; queue.length = 0;
    };
    buffer.addEventListener('updateend', () => {
      try { chase(); pump(); } catch (error) { fail(current, error); }
    });
    buffer.addEventListener('error', () => fail(current, new Error(L.errorDecode)));
    video.play().catch(() => {});
    pump();
    for (;;) {
      const {value, done} = await reader.read();
      if (session !== current) return;
      if (done) { ended = true; pump(); return; }
      queue.push(value); pump();
    }
  };
  const start = () => {
    if (retry !== null) { window.clearTimeout(retry); retry = null; }
    if (session) session.abort.abort();
    stopped = userPaused = false;
    startedAt = performance.now();
    status.textContent = L.connecting;
    if (!useMse) { session = null; video.src = streamUrl(); video.load(); video.play().catch(() => {}); return; }
    const current = session = {abort: new AbortController()};
    playMse(current).catch(error => { if (error.name !== 'AbortError') fail(current, error); });
  };
  // Ends the session at its first error, so its reader stops appending.
  const fail = (current, error) => {
    if (session !== current) return;
    session = null; current.abort.abort();
    reconnect(L.liveUnavailable + (error.message || L.unknownError));
  };
  const reconnect = message => {
    status.textContent = message + L.retrying;
    if (retry === null && !stopped) retry = window.setTimeout(start, 1500);
  };
  // Stop downloading while nobody watches. MSE keeps the last frame on
  // screen; a native player is emptied, as it would otherwise keep buffering.
  const stop = () => {
    if (retry !== null) { window.clearTimeout(retry); retry = null; }
    if (session) { session.abort.abort(); session = null; }
    else if (!useMse) { video.removeAttribute('src'); video.load(); }
    stopped = true;
  };
  video.addEventListener('pause', () => {
    if (!useMse || document.hidden || video.ended || stopped) return;
    userPaused = true; stop();
  });
  video.addEventListener('play', () => { if (stopped && !document.hidden) start(); });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { if (!stopped) stop(); }
    else if (stopped && !userPaused) start();
  });
  video.addEventListener('loadedmetadata', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });
  video.addEventListener('playing', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });
  video.addEventListener('ended', () => reconnect(L.streamEnded));
  video.addEventListener('error', () => {
    const names = ['', L.errorAborted, L.errorNetwork, L.errorDecode, L.errorUnsupported];
    const detail = video.error ? (names[video.error.code] || (L.errorCode + video.error.code)) + (video.error.message ? ': ' + video.error.message : '') : L.unknownError;
    reconnect(L.liveUnavailable + detail);
  });
  // MSE holds only what has arrived, so it can seek forward to the live edge.
  // Leave an intentional pause alone.
  const chase = () => {
    if (video.paused || video.seeking || !video.buffered.length) return;
    const last = video.buffered.length - 1, from = video.buffered.start(last), end = video.buffered.end(last);
    if (video.currentTime < from || end - video.currentTime > MSE_SEEK) {
      video.currentTime = Math.max(from, end - MSE_TARGET); return;
    }
    video.playbackRate = end - video.currentTime > MSE_TARGET + 0.05 ? 1.1 : 1.0;
  };
  // Native players can keep playing an old buffer after an underrun.
  // Reopen at a fresh IDR: seeking a growing HTTP MP4 can fail or decode
  // the old backlog again.
  const catchUp = () => {
    if (useMse) { chase(); return; }
    if (stopped || video.paused || video.seeking || video.readyState < 2 || !video.buffered.length) return;
    if (performance.now() - startedAt < 5000) return; // let a new decoder settle
    const last = video.buffered.length - 1;
    const lag = video.buffered.end(last) - video.currentTime;
    if (lag > 1.5) { start(); return; }
    // Native players hold ~0.7 s of a live MP4; play slightly fast to drain it.
    // Below ~0.25 s Chrome underruns and stutters, so stop well short of that.
    if (lag > 0.45) video.playbackRate = 1.1;
    else if (lag < 0.3) video.playbackRate = 1.0;
  };
  window.setInterval(catchUp, 250);
  stream.addEventListener('change', start); start();
  let lease = '', held = null, renewing = false, leaving = false;
  const controls = [...document.querySelectorAll('[data-direction]'), document.getElementById('live-center'), rate, zoom];
  const refreshControls = () => { controls.forEach(control => { control.disabled = !lease; }); };
  enable.checked = false; refreshControls();
  const controlRequest = async (action, token, value) => {
    const body = new URLSearchParams({csrf: script.dataset.csrf, action, lease: token});
    if (value !== undefined) body.set('value', value);
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 1500);
    try {
      const response = await fetch('/live/control', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body, signal: controller.signal});
      const message = (await response.text()).trim();
      if (!response.ok) throw new Error(message || ('HTTP ' + response.status));
      return message;
    } finally { window.clearTimeout(timeout); }
  };
  const dropControl = () => {
    const previous = lease; lease = ''; held = null; enable.checked = false; refreshControls();
    if (previous) fetch('/live/control', {method: 'POST', keepalive: true,
      body: new URLSearchParams({csrf: script.dataset.csrf, action: 'release', lease: previous})}).catch(() => {});
  };
  enable.addEventListener('change', async () => {
    enable.disabled = true;
    try {
      if (enable.checked) {
        const token = await controlRequest('acquire', '');
        if (!/^[0-9a-f]{32}$/.test(token)) throw new Error(L.unknownError);
        lease = token;
        if (leaving) dropControl();
        else controlStatus.textContent = L.manualEnabled;
      } else {
        const previous = lease; lease = ''; held = null; refreshControls();
        if (previous) await controlRequest('release', previous);
        controlStatus.textContent = L.manualDisabled;
      }
    } catch (error) {
      dropControl(); controlStatus.textContent = L.controlFailed + error.message;
    } finally { enable.checked = !!lease; enable.disabled = false; refreshControls(); }
  });
  window.setInterval(async () => {
    if (!lease || renewing) return;
    const previous = lease; renewing = true;
    try { await controlRequest('renew', previous); }
    catch (error) { if (lease === previous) { dropControl(); controlStatus.textContent = L.controlFailed + error.message; } }
    finally { renewing = false; }
  }, 1000);
  window.addEventListener('pagehide', () => { leaving = true; dropControl(); });
  window.addEventListener('pageshow', () => { leaving = false; });
  const sendControl = async (action, value) => {
    if (!lease) { controlStatus.textContent = L.enableFirst; return false; }
    const previous = lease;
    try {
      const message = await controlRequest(action, previous, value);
      controlStatus.textContent = message || L.commandSent;
      // Refresh before the 180 ms manual pulse and 150 ms native watchdog.
      // Keep requests serialized; a slow or throttled browser stops safely.
      await new Promise(resolve => window.setTimeout(resolve, 50));
      return lease === previous;
    } catch (error) {
      if (lease === previous) dropControl();
      controlStatus.textContent = L.controlFailed + error.message; return false;
    }
  };
  const release = () => { held = null; };
  document.querySelectorAll('[data-direction]').forEach(button => button.addEventListener('pointerdown', async event => {
    event.preventDefault(); if (!enable.checked || held) return; held = button.dataset.direction; button.setPointerCapture(event.pointerId);
    const direction = held; while (held === direction && await sendControl(direction, Number(rate.value).toFixed(0))) {}
  }));
  window.addEventListener('pointerup', release); window.addEventListener('pointercancel', release); window.addEventListener('blur', release);
  document.getElementById('live-center').addEventListener('click', () => sendControl('center'));
  rate.addEventListener('input', () => { rateValue.textContent = Number(rate.value).toFixed(0) + '°/s'; });
  zoom.addEventListener('input', () => { zoomValue.textContent = Number(zoom.value).toFixed(1) + 'x'; });
  zoom.addEventListener('change', () => sendControl('zoom', Number(zoom.value).toFixed(1)));
  let attitudePending = false;
  const attitudeUnavailable = message => {
    attitudeRoll.textContent = attitudePitch.textContent = attitudeYaw.textContent = '—';
    attitudeRollRate.textContent = attitudePitchRate.textContent = attitudeYawRate.textContent = '—';
    attitudeWorld.style.transform = 'translateY(0) rotate(0)';
    attitudeHeading.style.transform = 'rotate(0)';
    attitudeDial.setAttribute('aria-label', L.attitudeUnavailable);
    attitudeStatus.textContent = message;
  };
  const refreshAttitude = async () => {
    if (attitudePending) return; attitudePending = true;
    try {
      const response = await fetch('/live/attitude.json', {cache: 'no-store'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      if (data.roll_deg === null || data.pitch_deg === null || data.yaw_deg === null) { attitudeUnavailable(L.attitudeUnavailable); return; }
      const roll = Number(data.roll_deg), pitch = Number(data.pitch_deg), yaw = Number(data.yaw_deg);
      attitudeRoll.textContent = roll.toFixed(1) + '°'; attitudePitch.textContent = pitch.toFixed(1) + '°'; attitudeYaw.textContent = yaw.toFixed(1) + '°';
      attitudeRollRate.textContent = data.roll_rate_dps === null ? '—' : Number(data.roll_rate_dps).toFixed(1) + '°/s';
      attitudePitchRate.textContent = data.pitch_rate_dps === null ? '—' : Number(data.pitch_rate_dps).toFixed(1) + '°/s';
      attitudeYawRate.textContent = data.yaw_rate_dps === null ? '—' : Number(data.yaw_rate_dps).toFixed(1) + '°/s';
      const pitchPixels = Math.max(-55, Math.min(55, pitch * 1.15));
      attitudeWorld.style.transform = 'translateY(' + pitchPixels + 'px) rotate(' + (-roll) + 'deg)';
      attitudeHeading.style.transform = 'rotate(' + yaw + 'deg)';
      attitudeDial.setAttribute('aria-label', L.attitudeLabel.replace('%s', roll.toFixed(1)).replace('%s', pitch.toFixed(1)).replace('%s', yaw.toFixed(1)));
      attitudeStatus.textContent = L.liveAttitude;
    } catch (error) { attitudeUnavailable(L.attitudeFailed + (error && error.message ? error.message : L.unknownError)); }
    finally { attitudePending = false; }
  };
  if (script.dataset.gimbal !== '0') { refreshAttitude(); window.setInterval(refreshAttitude, 250); }
})();
