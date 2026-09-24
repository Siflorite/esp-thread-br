/* Run with: node tools/ci/check_web_cli.js */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync('components/esp_ot_br_server/frontend/static/web-cli.js', 'utf8');
const flush = () => new Promise(resolve => setImmediate(resolve));
function harness(features = true) {
    const elements = {}, requests = [], replies = [{web_cli: features}], streams = [];
    const tasks = new Map();
    let timerId = 0;
    function target(extra = {}) {
        return Object.assign({listeners: {}, addEventListener(type, fn) { this.listeners[type] = fn; },
            emit(type, event = {}) { this.listeners[type]?.(Object.assign({preventDefault() {}}, event)); }}, extra);
    }
    function element(id) {
        return elements[id] ||= target({value: '', checked: true, disabled: false, textContent: '',
            scrollHeight: 100, scrollTop: 0,
            classList: {toggle(name, hidden) { this.hidden = hidden; }}, focus() {}});
    }
    class EventSource {
        constructor(url) { this.url = url; this.listeners = {}; this.closed = false; this.readyState = 0; streams.push(this); }
        addEventListener(type, fn) { this.listeners[type] = fn; }
        close() { this.closed = true; this.readyState = 2; }
        emit(type, data = {}, id) {
            const payload = Object.assign({session: '1', cursor: '0', ready: true}, data);
            this.listeners[type]?.({type, data: JSON.stringify(payload),
                lastEventId: id ?? payload.session + ':' + payload.cursor});
        }
        error() { this.onerror(); }
    }
    const document = target({getElementById: element, hidden: false}), window = target();
    vm.runInNewContext(script, {
        document, window, EventSource, AbortController, TextEncoder,
        setTimeout(fn, delay) { const id = ++timerId; tasks.set(id, {fn, delay}); return id; },
        clearTimeout(id) { tasks.delete(id); },
        fetch(url, options) {
            requests.push({url, options});
            assert.ok(url === '/web/features' || (url === '/cli' && options.method === 'POST'), 'no CLI GET/polling');
            assert.ok(replies.length, 'unexpected fetch');
            const reply = replies.shift();
            if (reply instanceof Error) return Promise.reject(reply);
            if (reply && typeof reply.then === 'function') return reply;
            return Promise.resolve({ok: true, json: async () => reply});
        }
    });
    return {element, document, window, streams, requests, replies, tasks};
}
async function main() {
    const h = harness(), {element: e, streams, replies, requests, tasks, document, window} = h;
    assert.equal(e('cliSend').disabled, true, 'disabled before feature discovery');
    await flush();
    let s = streams[0];
    assert.equal(s.url, '/cli/events', 'initial connection has no cursor');
    assert.equal(e('cliSend').disabled, true, 'disabled until status');
    s.emit('records', {records: []});
    assert.equal(e('cliSend').disabled, true, 'records cannot replace initial status');
    s.emit('status', {ready: false});
    assert.match(e('cliStatus').textContent, /unavailable/);
    s.emit('status');
    assert.equal(e('cliSend').disabled, false);
    s.emit('records', {cursor: '3', records: [
        {seq: '1', source: 'log', text: '\x1b[32mBR <script>alert(1)</script>\x1b[0m\x00\n'},
        {seq: '2', source: 'input', text: 'state'},
        {seq: '3', source: 'cli', text: 'leader\r\nDone\r\n'}
    ]});
    assert.match(e('cliOutput').textContent, /<script>/);
    assert.match(e('cliOutput').textContent, /> ot state/);
    assert.ok(!/[\x1b\x00]/.test(e('cliOutput').textContent));
    assert.equal(e('cliOutput').scrollTop, 100);
    assert.equal(e('cliOutput').innerHTML, undefined, 'only textContent used');
    e('cliLogs').checked = false; e('cliLogs').emit('change');
    assert.ok(!e('cliOutput').textContent.includes('BR'));
    assert.match(e('cliOutput').textContent, /leader/);
    e('cliFollow').checked = false; e('cliOutput').scrollTop = 0;
    s.emit('records', {cursor: '4', records: [{seq: '4', source: 'cli', text: 'tail\n'}]});
    assert.equal(e('cliOutput').scrollTop, 0);
    e('cliInput').value = 'ot state'; replies.push({accepted: true});
    e('cliForm').emit('submit');
    assert.equal(e('cliSend').disabled, true);
    await flush();
    assert.equal(requests.at(-1).options.body, '{"command":"ot state"}');
    assert.equal(requests.at(-1).options.headers['Content-Type'], 'application/json');
    e('cliInput').emit('keydown', {key: 'ArrowUp'});
    assert.equal(e('cliInput').value, 'ot state');
    e('cliInput').emit('keydown', {key: 'ArrowDown'});
    assert.equal(e('cliInput').value, '');
    const sentCount = requests.length;
    for (const invalid of ['', 'ot state\not reset', '界'.repeat(86), 'a\x7fb']) {
        e('cliInput').value = invalid; e('cliForm').emit('submit'); await flush();
        assert.equal(requests.length, sentCount, 'invalid command not sent');
    }
    replies.push(new Error('rejected')); e('cliInput').value = 'state'; e('cliForm').emit('submit');
    await flush(); assert.match(e('cliOutput').textContent, /Send failed: rejected/);
    s.emit('gap', {cursor: '9007199254740993'});
    assert.match(e('cliOutput').textContent, /overwritten/);
    e('cliClear').emit('click'); assert.equal(e('cliOutput').textContent, '');
    e('cliPause').emit('click');
    assert.equal(s.closed, true); assert.equal(e('cliSend').disabled, true);
    assert.equal(e('cliStatus').textContent, 'Paused');
    s.emit('status', {cursor: '99'}); s.error();
    assert.equal(e('cliStatus').textContent, 'Paused', 'stale events ignored');
    e('cliPause').emit('click'); s = streams.at(-1);
    assert.equal(s.url, '/cli/events?after=1%3A9007199254740993', 'clear preserves exact string cursor');
    assert.equal(e('cliSend').disabled, true);
    s.emit('reset', {session: '2', cursor: '0'});
    assert.match(e('cliOutput').textContent, /restarted/);
    s.emit('status', {session: '2', cursor: '0'});
    s.emit('gap', {session: '2', cursor: '5'});
    const streamCount = streams.length;
    s.error();
    assert.equal(e('cliSend').disabled, true);
    assert.match(e('cliStatus').textContent, /reconnecting/);
    assert.equal(streams.length, streamCount, 'native reconnect uses same EventSource');
    assert.equal(s.closed, false, 'error must not close reconnecting source');
    e('cliInput').value = 'state'; const count = requests.length; e('cliForm').emit('submit');
    assert.equal(requests.length, count, 'cannot send while disconnected');
    s.emit('status', {session: '2', cursor: '5'});
    assert.equal(e('cliSend').disabled, false, 'status restores native reconnect');
    document.hidden = true; document.emit('visibilitychange');
    assert.equal(s.closed, true); assert.equal(e('cliSend').disabled, true);
    s.emit('records', {records: [{seq: '1', source: 'cli', text: 'STALE'}]});
    assert.ok(!e('cliOutput').textContent.includes('STALE'));
    e('cliPause').emit('click');
    document.hidden = false; document.emit('visibilitychange');
    assert.equal(streams.length, streamCount, 'visibility does not override pause');
    e('cliPause').emit('click'); s = streams.at(-1);
    assert.equal(s.url, '/cli/events?after=2%3A5', 'gap/reset updates resume ID');
    document.emit('visibilitychange'); assert.equal(streams.at(-1), s, 'no duplicate stream');
    s.emit('status', {session: '2', cursor: '5'});
    for (let i = 0; i < 801; i++) s.emit('records', {session: '2', cursor: String(i + 6),
        records: [{seq: String(i + 6), source: 'cli', text: 'row' + i + '\n'}]});
    assert.equal(e('cliOutput').textContent.split('\n').filter(Boolean).length, 800);
    assert.ok(!e('cliOutput').textContent.includes('row0\n'));
    const before = e('cliOutput').textContent;
    s.emit('records', {records: [{source: 'cli', text: 'malformed'}]});
    s.emit('status', {cursor: '1'}, 'bad-id');
    assert.equal(e('cliOutput').textContent, before);
    window.emit('pagehide'); assert.equal(s.closed, true);
    s.error(); s.emit('status'); assert.equal(e('cliSend').disabled, true);
    replies.push({web_cli: true}); window.emit('pageshow', {persisted: true}); await flush();
    s = streams.at(-1); assert.equal(s.url, '/cli/events?after=2%3A806');
    assert.equal(e('cliSend').disabled, true);
    s.emit('status', {session: '2', cursor: '806'});
    assert.equal(e('cliSend').disabled, false);
    window.emit('pagehide'); replies.push({web_cli: false});
    window.emit('pageshow', {persisted: true}); await flush();
    assert.equal(e('webCli').classList.hidden, true);
    assert.equal(e('cliSend').disabled, true);
    document.emit('visibilitychange'); assert.equal(streams.at(-1), s);
    assert.equal(tasks.size, 0, 'no polling/reconnect timers after completed requests');
    const disabled = harness(false); await flush();
    assert.equal(disabled.streams.length, 0); assert.equal(disabled.requests.length, 1);
    assert.equal(disabled.element('webCli').classList.hidden, true);
    assert.equal(disabled.tasks.size, 0);
    // A feature response from a pagehide-era request must not reopen a stream.
    const race = harness(); race.window.emit('pagehide'); await flush();
    assert.equal(race.streams.length, 0);
    race.replies.push({web_cli: true}); race.window.emit('pageshow', {persisted: true}); await flush();
    assert.equal(race.streams.length, 1);
    const fatal = harness(); await flush();
    let closed = fatal.streams[0];
    closed.emit('status', {cursor: '7'}); closed.readyState = 2; closed.error();
    assert.equal(closed.closed, true);
    assert.equal(fatal.element('cliSend').disabled, true);
    assert.equal(fatal.tasks.size, 1, 'CLOSED response schedules one SSE retry');
    const [retryId, retry] = [...fatal.tasks.entries()][0];
    assert.equal(retry.delay, 3000);
    fatal.document.emit('visibilitychange');
    assert.equal(fatal.streams.length, 1, 'backoff cannot be bypassed by visibility event');
    fatal.tasks.delete(retryId); retry.fn();
    assert.equal(fatal.streams.length, 2);
    assert.equal(fatal.streams[1].url, '/cli/events?after=1%3A7');
    closed.emit('status', {cursor: '999'}); closed.error();
    assert.equal(fatal.tasks.size, 0, 'old fatal stream cannot schedule retry');
    closed = fatal.streams[1]; closed.readyState = 2; closed.error();
    fatal.element('cliPause').emit('click');
    assert.equal(fatal.tasks.size, 0, 'pause cancels fatal retry');
    fatal.element('cliPause').emit('click');
    closed = fatal.streams.at(-1); closed.readyState = 2; closed.error();
    fatal.document.hidden = true; fatal.document.emit('visibilitychange');
    assert.equal(fatal.tasks.size, 0, 'hidden cancels fatal retry');
    fatal.document.hidden = false; fatal.document.emit('visibilitychange');
    closed = fatal.streams.at(-1); closed.readyState = 2; closed.error();
    fatal.window.emit('pagehide');
    assert.equal(fatal.tasks.size, 0, 'pagehide cancels fatal retry');
    console.log('Web CLI frontend checks passed: SSE rendering, POST, validation/history, no polling, ID resume, gap/reset, native reconnect, stale events, pause/visibility, bfcache, feature switch, 800-record limit.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
