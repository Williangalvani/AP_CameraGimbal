// The Status page's microSD script (webroot/storage.js) against a fake page,
// XMLHttpRequest and clock: /storage/state answers come from each test.
const fs = require('fs');
const vm = require('vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const L = {
  ready: 'ready', readySize: 'ready (%s, %s free)', failed: 'failed: %s. card: %s', timeout: 'format timeout',
  retrying: 'retrying', formatting: 'formatting', benchTimeout: 'bench timeout', card: 'Card: %s', bus: 'Bus: %s',
};

class Element {
  constructor(id) {
    this.id = id;
    this.hidden = false;
    this.textContent = '';
    this.attributes = {};
    this.listeners = {};
    this.buttons = [];
    const classes = new Set();
    this.classList = {
      toggle: (name, on) => (on ? classes.add(name) : classes.delete(name)),
      contains: (name) => classes.has(name),
    };
  }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  removeAttribute(name) { delete this.attributes[name]; }
  getAttribute(name) { return name in this.attributes ? this.attributes[name] : null; }
  hasAttribute(name) { return name in this.attributes; }
  addEventListener(name, listener) { (this.listeners[name] = this.listeners[name] || []).push(listener); }
  dispatch(name) { (this.listeners[name] || []).forEach((listener) => listener({})); }
  querySelectorAll(selector) { return selector === 'button' ? this.buttons : []; }
}

// One Status page: script attributes, the query it was loaded with, and
// server(state) answering each poll as {status, body}.
function page({attributes = {}, search = '', server = () => ({status: 404, body: ''}), forms = true}) {
  let clock = 1000000;
  let timers = [];
  const requests = [];
  const elements = {};
  for (const id of ['sdcard-state', 'sdcard-status', 'sdcard-bench-row', 'sdcard-bench', 'sdcard-bench-card',
    'sdcard-bench-bus']) elements[id] = new Element(id);
  if (forms) {
    for (const id of ['sdcard-format', 'sdcard-bench-form']) {
      elements[id] = new Element(id);
      elements[id].buttons = [{disabled: false}];
    }
  }
  elements['sdcard-status'].hidden = true;
  const script = new Element('script');
  Object.entries(attributes).forEach(([name, value]) => script.setAttribute(name, value));
  const history = [];
  const windowListeners = {};
  const location = {search, replaced: null, replace(url) { this.replaced = url; }};
  class FakeRequest {
    open(method, url) { this.url = url; }
    send() {
      requests.push(this.url);
      const answer = server(requests.length, clock);
      if (answer === 'error') this.onerror();
      else {
        this.status = answer.status;
        this.responseText = typeof answer.body === 'string' ? answer.body : JSON.stringify(answer.body);
        this.onload();
      }
    }
  }
  const context = {
    L,
    Date: {now: () => clock},
    Math,
    Number,
    String,
    JSON,
    setTimeout: (fn, delay) => timers.push({at: clock + delay, fn}),
    XMLHttpRequest: FakeRequest,
    history: {replaceState: (state, title, url) => history.push([url, requests.length])},
    document: {currentScript: script, getElementById: (id) => elements[id] || null},
  };
  context.window = {location, addEventListener: (name, fn) => (windowListeners[name] = fn), history: context.history};
  vm.runInNewContext(source, context);
  return {
    elements, requests, history, location, windowListeners,
    status: elements['sdcard-status'],
    // advance the clock by ms, running the timers due
    run(ms) {
      const end = clock + ms;
      for (;;) {
        timers.sort((a, b) => a.at - b.at);
        if (!timers.length || timers[0].at > end) break;
        const timer = timers.shift();
        clock = timer.at;
        timer.fn();
      }
      clock = end;
    },
    pending: () => timers.length,
  };
}

let failures = 0;
let cases = 0;
function check(name, condition, detail) {
  cases++;
  if (!condition) {
    failures++;
    console.log('FAIL ' + name + (detail === undefined ? '' : ': ' + JSON.stringify(detail)));
  }
}
const ok = (body) => ({status: 200, body});
const ready = {state: 'ready', text: '1 GiB used', size: '59.5 GiB', free: '$& GiB'};

// Reloading: the query is dropped before anything else, and a page with
// nothing to follow never polls; a submit disables both forms' buttons.
{
  const p = page({search: '?sdcard=bench'});
  check('query replaced at once', p.history.length === 1 && p.history[0][0] === '/' && p.history[0][1] === 0,
    p.history);
  p.run(700000);
  check('idle page does not poll', p.requests.length === 0, p.requests);
  p.elements['sdcard-format'].dispatch('submit');
  const buttons = [p.elements['sdcard-format'].buttons[0], p.elements['sdcard-bench-form'].buttons[0]];
  check('submit disables the buttons', buttons.every((button) => button.disabled));
  check('submit marks the forms busy', p.elements['sdcard-format'].getAttribute('aria-busy') === 'true');
  check('pageshow handler', typeof p.windowListeners.pageshow === 'function');
  if (p.windowListeners.pageshow) p.windowListeners.pageshow();
  check('pageshow enables the buttons', buttons.every((button) => !button.disabled));
  check('pageshow clears busy', !p.elements['sdcard-format'].hasAttribute('aria-busy'));
  const plain = page({});
  check('no query, no replace', plain.history.length === 0);
}

// A format: "ready" and the result from before it are not success, however
// long they last; only a new result is.
{
  let result = {result: 'ok', at: 100.5};
  let state = ready;
  const p = page({
    attributes: {'data-started': '', 'data-format': '100.5'},
    search: '?sdcard=format&since=100.5',
    server: () => ok(Object.assign({}, state, {format: result})),
  });
  p.elements['sdcard-format'].hidden = p.elements['sdcard-bench-form'].hidden = true;
  p.run(20000);
  check('old result: still formatting after 20 s', p.status.textContent === 'formatting' && !p.status.hidden,
    p.status.textContent);
  check('old result: forms stay hidden', p.elements['sdcard-format'].hidden && p.elements['sdcard-bench-form'].hidden);
  check('old result: polling continues', p.pending() === 1);
  state = {state: 'formatting', text: 'formatting…'};
  p.run(3000);
  check('formatting row', p.elements['sdcard-state'].textContent === 'formatting…');
  state = ready;
  result = {result: 'ok', at: 230.25};
  p.run(1500);
  check('new ok result: ready with sizes, $& literal', p.status.textContent === 'ready (59.5 GiB, $& GiB free)',
    p.status.textContent);
  check('new ok result: not an error', !p.status.classList.contains('error'));
  check('new ok result: forms shown', !p.elements['sdcard-format'].hidden && !p.elements['sdcard-bench-form'].hidden);
  const polls = p.requests.length;
  p.run(60000);
  check('new ok result: polling stops', p.requests.length === polls && p.pending() === 0);
}

// No result before ("0"), then a failed one: its reason and the card's state.
{
  let body = ready;
  const p = page({attributes: {'data-started': '', 'data-format': '0'}, server: () => ok(body)});
  p.run(1500);
  check('no result yet: formatting', p.status.textContent === 'formatting');
  body = {state: 'unformatted', text: 'not formatted $1',
    format: {result: 'failed', at: 7, reason: 'mkfs', text: 'the filesystem could not be created'}};
  p.run(1500);
  check('failed result shown with reason', p.status.textContent ===
    'failed: the filesystem could not be created. card: not formatted $1', p.status.textContent);
  check('failed result is an error', p.status.classList.contains('error'));
  check('failed: format form for an unformatted card', !p.elements['sdcard-format'].hidden);
  check('failed: no benchmark form for an unformatted card', p.elements['sdcard-bench-form'].hidden);
  check('failed: polling stops', p.pending() === 0);
}

// A failed result that is not new after all is ignored.
{
  const p = page({attributes: {'data-started': '', 'data-format': '7'},
    server: () => ok({state: 'ready', text: 'x', format: {result: 'failed', at: 7, reason: 'mkfs'}})});
  p.run(30000);
  check('old failed result ignored', p.status.textContent === 'formatting' && p.pending() === 1);
}

// A format that never reports gives up after 10 minutes.
{
  const p = page({attributes: {'data-started': '', 'data-format': '5'},
    server: () => ok({state: 'formatting', text: 'formatting…', format: {result: 'ok', at: 5}})});
  p.run(599000);
  check('format before timeout', p.status.textContent === 'formatting');
  p.run(5000);
  check('format timeout message', p.status.textContent === 'format timeout' && p.status.classList.contains('error'),
    p.status.textContent);
  const polls = p.requests.length;
  p.run(60000);
  check('format timeout stops polling', p.requests.length === polls && p.pending() === 0);
}

// A benchmark: its progress, then its result and details, and the forms back.
{
  let bench = {status: 'running', text: 'Benchmarking…'};
  const p = page({attributes: {'data-bench': ''}, server: () => ok(Object.assign({}, ready, {bench}))});
  p.elements['sdcard-format'].hidden = p.elements['sdcard-bench-form'].hidden = true;
  p.elements['sdcard-bench-card'].hidden = p.elements['sdcard-bench-bus'].hidden = true;
  p.run(1500);
  check('bench running shown', p.status.textContent === 'Benchmarking…' && !p.status.classList.contains('error'));
  check('bench running: forms hidden', p.elements['sdcard-format'].hidden && p.elements['sdcard-bench-form'].hidden);
  bench = {status: 'done', text: 'Benchmark: write 14.9 MB/s', card: 'SB64G $&', bus: 'sd high-speed'};
  p.run(1500);
  check('bench result shown', p.status.textContent === 'Benchmark: write 14.9 MB/s' &&
    p.elements['sdcard-bench'].textContent === 'Benchmark: write 14.9 MB/s');
  check('bench details', p.elements['sdcard-bench-card'].textContent === 'Card: SB64G $&' &&
    !p.elements['sdcard-bench-card'].hidden && p.elements['sdcard-bench-bus'].textContent === 'Bus: sd high-speed',
    p.elements['sdcard-bench-card'].textContent);
  check('bench done: forms shown', !p.elements['sdcard-format'].hidden && !p.elements['sdcard-bench-form'].hidden);
  check('bench done: polling stops', p.pending() === 0);
}

// A failed benchmark with no details.
{
  const p = page({attributes: {'data-bench': ''},
    server: () => ok(Object.assign({}, ready, {bench: {status: 'failed', text: 'Benchmark failed: busy'}}))});
  p.run(1500);
  check('bench failure is an error', p.status.textContent === 'Benchmark failed: busy' &&
    p.status.classList.contains('error'));
  check('bench failure hides details', p.elements['sdcard-bench-card'].hidden && p.elements['sdcard-bench-bus'].hidden);
}

// A benchmark that never ends gives up after 10 minutes and shows the forms.
{
  const p = page({attributes: {'data-bench': ''},
    server: () => ok(Object.assign({}, ready, {bench: {status: 'running', text: 'Benchmarking…'}}))});
  p.elements['sdcard-format'].hidden = p.elements['sdcard-bench-form'].hidden = true;
  p.run(605000);
  check('bench timeout message', p.status.textContent === 'bench timeout' && p.status.classList.contains('error'),
    p.status.textContent);
  check('bench timeout: forms shown', !p.elements['sdcard-format'].hidden && !p.elements['sdcard-bench-form'].hidden);
  const polls = p.requests.length;
  p.run(60000);
  check('bench timeout stops polling', p.requests.length === polls && p.pending() === 0);
}

// Errors back off and retry; an expired login goes to the login page.
{
  let answer = 'error';
  const p = page({attributes: {'data-started': '', 'data-format': '0'}, server: () => answer});
  p.run(1500);
  check('network error: retrying', p.status.textContent === 'retrying' && p.status.classList.contains('error'));
  p.run(2999);
  check('retry backs off', p.requests.length === 1, p.requests.length);
  answer = {status: 500, body: 'oops'};
  p.run(1);
  check('server error: retrying', p.requests.length === 2 && p.status.textContent === 'retrying');
  answer = {status: 401, body: ''};
  p.run(6000);
  check('401 goes to the login', p.location.replaced === '/' && p.pending() === 0, p.location.replaced);
  check('polls ask for fresh state', /^\/storage\/state\?t=\d+$/.test(p.requests[0]), p.requests[0]);
}

console.log(failures ? 'FAIL storage.js: ' + failures + ' of ' + cases + ' checks'
  : 'PASS storage.js: reload, submit, format result, timeouts, benchmark (' + cases + ' checks)');
process.exit(failures ? 1 : 0);
