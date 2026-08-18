/* Pulse client. No framework, no build step, no dependency.
 *
 * Four parts, in this order below:
 *   1. the state and the binding layer, which writes changed values into the document
 *   2. the decoders, one per wire format, behind one call
 *   3. the transports, one per protocol, behind one call
 *   4. the drawing, which builds the chart as SVG nodes
 */
'use strict';

const SVG_NS = 'http://www.w3.org/2000/svg';
const HISTORY = 90;

const el = (id) => document.getElementById(id);
const TRANSPORT_NAME = { ws: 'WebSocket', sse: 'SSE', poll: 'Polling' };

/* ------------------------------------------------------------------ 1. state */

const state = {
  transport: 'ws',
  format: 'json',
  series: 'cpu',
  history: [],       // the last HISTORY snapshots, oldest first
  received: 0,
  last: {},          // the previous snapshot, used to find what changed
};

/* The binding layer. Each entry names one path in the snapshot and the way that value
 * reaches the document. Only entries whose path changed are applied, which is the point
 * of holding the previous snapshot: at four updates a second, rewriting every node would
 * do far more work than there are changed values. */
const bindings = [
  { path: 'cpu',      node: 'cpu',      write: (n, v) => (n.textContent = v.toFixed(1)) },
  { path: 'rss',      node: 'rss',      write: (n, v) => (n.textContent = (v / 1048576).toFixed(1)) },
  { path: 'requests', node: 'requests', write: (n, v) => (n.textContent = v) },
  { path: 'rps',      node: 'rps',      write: (n, v) => (n.textContent = v.toFixed(2)) },
  { path: 'uptime',   node: 'uptime',   write: (n, v) => (n.textContent = Math.round(v)) },
  { path: 'ws',       node: 'ws',       write: (n, v) => (n.textContent = v) },
  { path: 'sse',      node: 'sse',      write: (n, v) => (n.textContent = v) },
  { path: 'p50',      node: 'p50',      write: (n, v) => (n.textContent = v.toFixed(0)) },
  { path: 'p90',      node: 'p90',      write: (n, v) => (n.textContent = v.toFixed(0)) },
  { path: 'p99',      node: 'p99',      write: (n, v) => (n.textContent = v.toFixed(0)) },
];

for (const binding of bindings) binding.node = el(binding.node);

function applySnapshot(snapshot) {
  for (const binding of bindings) {
    const value = snapshot[binding.path];
    if (value === undefined || value === state.last[binding.path]) continue;
    binding.write(binding.node, value);
    flash(binding.node);
  }
  state.last = snapshot;

  state.history.push(snapshot);
  if (state.history.length > HISTORY) state.history.shift();

  drawSeries();
  drawHistogram(snapshot.histogram);
}

function flash(node) {
  node.classList.remove('changed');
  void node.offsetWidth;            // restart the animation on a node that just changed
  node.classList.add('changed');
}

/* ------------------------------------------------------------------ 2. decoders */

/* Both decoders return the same object, so nothing past this point knows which format
 * arrived. The measured decode time is what the two representations cost the client. */
function decode(text, format) {
  const started = performance.now();
  const snapshot = format === 'xml' ? fromXml(text) : JSON.parse(text);
  const elapsed = performance.now() - started;
  return { snapshot, elapsed, bytes: new Blob([text]).size };
}

function fromXml(text) {
  const document_ = new DOMParser().parseFromString(text, 'application/xml');
  if (document_.querySelector('parsererror')) throw new Error('malformed XML');
  const root = document_.documentElement;
  const value = (name) => Number(root.querySelector(name).textContent);
  return {
    seq: value('seq'),
    ts: value('ts'),
    uptime: value('uptime'),
    cpu: value('cpu'),
    rss: value('rss'),
    requests: value('requests'),
    rps: value('rps'),
    bytes: value('bytes'),
    ws: value('ws'),
    sse: value('sse'),
    p50: value('p50'),
    p90: value('p90'),
    p99: value('p99'),
    histogram: Array.from(root.querySelectorAll('histogram > bucket')).map((bucket) => ({
      le: bucket.getAttribute('le') === 'inf' ? null : Number(bucket.getAttribute('le')),
      count: Number(bucket.textContent),
    })),
  };
}

function onMessage(text) {
  let decoded;
  try {
    decoded = decode(text, state.format);
  } catch (error) {
    setStatus('waiting', 'undecodable message: ' + error.message);
    return;
  }
  state.received += 1;
  setStatus('live', TRANSPORT_NAME[state.transport] + ' connected');

  el('w-count').textContent = state.received;
  el('w-bytes').textContent = decoded.bytes;
  el('w-parse').textContent = decoded.elapsed.toFixed(3);
  el('w-age').textContent = (Date.now() - decoded.snapshot.ts / 1000).toFixed(1);
  el('w-raw').textContent = text;

  applySnapshot(decoded.snapshot);
}

/* ------------------------------------------------------------------ 3. transports */

let active = null;   // the object returned by the transport currently running

const transports = {
  /* Bidirectional. The format is changed by a command over the same socket, with no new
   * connection and no request. */
  ws() {
    const socket = new WebSocket('ws://' + location.host + '/ws');
    socket.addEventListener('open', () => {
      setStatus('live', 'WebSocket connected');
      socket.send(JSON.stringify({ cmd: 'format', value: state.format }));
    });
    socket.addEventListener('message', (event) => {
      if (event.data.startsWith('{"type":')) return;    // an acknowledgement, not a snapshot
      onMessage(event.data);
    });
    socket.addEventListener('close', () => setStatus('down', 'WebSocket closed'));
    socket.addEventListener('error', () => setStatus('down', 'WebSocket error'));
    return {
      stop: () => socket.close(),
      setFormat: (format) => socket.readyState === WebSocket.OPEN &&
        socket.send(JSON.stringify({ cmd: 'format', value: format })),
    };
  },

  /* One way. The format is part of the request, so changing it needs a new connection,
   * which is exactly the limitation the report compares against WebSocket. */
  sse() {
    const source = new EventSource('/api/stream?format=' + state.format);
    source.addEventListener('snapshot', (event) => onMessage(event.data));
    source.addEventListener('open', () => setStatus('live', 'SSE connected'));
    source.addEventListener('error', () => setStatus('waiting', 'SSE reconnecting'));
    return { stop: () => source.close(), setFormat: () => restart() };
  },

  /* Request and response, once per interval. Every update costs a full exchange of
   * headers, and the value is as old as the time since the server sampled it. */
  poll() {
    const path = state.format === 'xml' ? '/api/metrics.xml' : '/api/metrics.json';
    let stopped = false;
    const tick = async () => {
      if (stopped) return;
      try {
        const response = await fetch(path, { cache: 'no-store' });
        onMessage(await response.text());
      } catch (error) {
        setStatus('down', 'poll failed');
      }
      if (!stopped) timer = setTimeout(tick, interval);
    };
    let timer = setTimeout(tick, 0);
    return { stop: () => { stopped = true; clearTimeout(timer); }, setFormat: () => restart() };
  },
};

let interval = 250;

function restart() {
  if (active) active.stop();
  state.received = 0;
  state.last = {};
  setStatus('waiting', 'connecting');
  el('w-transport').textContent = TRANSPORT_NAME[state.transport];
  el('w-format').textContent = state.format.toUpperCase();
  active = transports[state.transport]();
}

function setStatus(kind, text) {
  const node = el('status');
  if (node.dataset.state === kind && el('status-text').textContent === text) return;
  node.dataset.state = kind;
  el('status-text').textContent = text;
}

/* ------------------------------------------------------------------ 4. drawing */

function drawSeries() {
  const values = state.history.map((snapshot) => pick(snapshot, state.series));
  const line = el('line');
  const area = el('area');
  const points = el('points');
  const grid = el('grid-lines');
  points.replaceChildren();
  grid.replaceChildren();
  if (values.length < 2) { line.setAttribute('d', ''); area.setAttribute('d', ''); return; }

  const low = Math.min(...values);
  const high = Math.max(...values);
  const span = high - low < 1e-9 ? 1 : high - low;
  const bottom = low - span * 0.15;
  const top = high + span * 0.15;
  // The newest sample sits at the right edge and the series grows to the left, so the
  // horizontal scale never changes under a value that is already drawn.
  const step = 1000 / (HISTORY - 1);
  const x = (i) => 1000 - (values.length - 1 - i) * step;
  const y = (v) => 260 - ((v - bottom) / (top - bottom)) * 250;

  for (let i = 1; i < 4; i += 1) {
    const rule = document.createElementNS(SVG_NS, 'line');
    rule.setAttribute('x1', 0);
    rule.setAttribute('x2', 1000);
    rule.setAttribute('y1', (260 / 4) * i);
    rule.setAttribute('y2', (260 / 4) * i);
    grid.appendChild(rule);
  }

  const path = values.map((v, i) => (i === 0 ? 'M' : 'L') + x(i).toFixed(1) + ' ' + y(v).toFixed(1));
  line.setAttribute('d', path.join(' '));
  area.setAttribute('d', path.join(' ') + ' L1000 275 L' + x(0).toFixed(1) + ' 275 Z');

  /* Every sample becomes a circle element. The nodes are replaced on each update, so no
   * handler is attached to them: the figure below delegates instead. */
  const fragment = document.createDocumentFragment();
  values.forEach((value, index) => {
    const dot = document.createElementNS(SVG_NS, 'circle');
    dot.setAttribute('cx', x(index).toFixed(1));
    dot.setAttribute('cy', y(value).toFixed(1));
    dot.setAttribute('r', values.length > 45 ? 2 : 3);
    dot.dataset.value = value;
    dot.dataset.seq = state.history[index].seq;
    fragment.appendChild(dot);
  });
  points.appendChild(fragment);

  el('axis-min').textContent = format(bottom);
  el('axis-max').textContent = format(top);
}

function pick(snapshot, series) {
  if (series === 'rss') return snapshot.rss / 1048576;
  return snapshot[series];
}

function format(value) {
  return Math.abs(value) >= 100 ? value.toFixed(0) : value.toFixed(2);
}

function drawHistogram(buckets) {
  const svg = el('histogram');
  svg.replaceChildren();
  if (!buckets || buckets.length === 0) return;
  const highest = Math.max(1, ...buckets.map((bucket) => bucket.count));
  const width = 520 / buckets.length;

  buckets.forEach((bucket, index) => {
    const height = (bucket.count / highest) * 120;
    const bar = document.createElementNS(SVG_NS, 'rect');
    bar.setAttribute('x', (index * width + 3).toFixed(1));
    bar.setAttribute('width', (width - 6).toFixed(1));
    bar.setAttribute('y', (130 - height).toFixed(1));
    bar.setAttribute('height', height.toFixed(1));
    bar.setAttribute('rx', 3);
    const title = document.createElementNS(SVG_NS, 'title');
    title.textContent = (bucket.le === null ? 'over 10000' : 'up to ' + bucket.le) +
      ' us: ' + bucket.count + ' requests';
    bar.appendChild(title);
    svg.appendChild(bar);

    const label = document.createElementNS(SVG_NS, 'text');
    label.setAttribute('x', (index * width + width / 2).toFixed(1));
    label.setAttribute('y', 148);
    label.setAttribute('text-anchor', 'middle');
    label.textContent = bucket.le === null ? '10k+' : (bucket.le >= 1000 ? bucket.le / 1000 + 'k' : bucket.le);
    svg.appendChild(label);
  });
}

/* ------------------------------------------------------------------ events */

/* One handler for the whole figure rather than one per point. The circles are thrown
 * away and rebuilt several times a second, so per node handlers would be attached and
 * detached at the same rate. */
el('chart-figure').addEventListener('mouseover', (event) => {
  const dot = event.target.closest('circle');
  const readout = el('readout');
  if (!dot) { readout.classList.remove('active'); readout.textContent = 'hover a point for its value'; return; }
  readout.classList.add('active');
  readout.textContent = 'snapshot #' + dot.dataset.seq + '  ' + state.series + ' = ' +
    format(Number(dot.dataset.value));
});

document.querySelector('.controls').addEventListener('click', (event) => {
  const button = event.target.closest('button');
  if (!button) return;

  if (button.dataset.transport) {
    setActive(button, '[data-transport]');
    state.transport = button.dataset.transport;
    restart();
  } else if (button.dataset.format) {
    setActive(button, '[data-format]');
    state.format = button.dataset.format;
    el('w-format').textContent = state.format.toUpperCase();
    active.setFormat(state.format);
  } else if (button.id === 'theme') {
    const order = ['auto', 'light', 'dark'];
    const next = order[(order.indexOf(document.documentElement.dataset.theme) + 1) % order.length];
    document.documentElement.dataset.theme = next;
    button.textContent = 'Theme: ' + next;
  }
});

function setActive(button, selector) {
  for (const other of document.querySelectorAll(selector)) other.classList.remove('on');
  button.classList.add('on');
}

el('series').addEventListener('change', (event) => {
  state.series = event.target.value;
  drawSeries();
});

/* ------------------------------------------------------------------ start */

fetch('/api/info')
  .then((response) => response.json())
  .then((info) => { interval = info.interval_ms; })
  .catch(() => {})
  .finally(restart);
