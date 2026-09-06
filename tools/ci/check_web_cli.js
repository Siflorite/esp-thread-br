/* Run with: node tools/ci/check_web_cli.js */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const elements = {};
function element(id) {
    return elements[id] ||= {
        value: '', checked: true, disabled: false, textContent: '',
        classList: {toggle(name, hidden) { this.hidden = hidden; }},
        listeners: {}, addEventListener(event, fn) { this.listeners[event] = fn; }, focus() {}
    };
}
let tasks = [], requests = [], replies = [];
let timerId = 0;
const context = {
    document: {getElementById: element, hidden: false},
    window: {addEventListener() {}},
    AbortController, TextEncoder,
    setTimeout(fn, delay) { const id = ++timerId; tasks.push({id, fn, delay}); return id; },
    clearTimeout(id) { tasks = tasks.filter(t => t.id !== id); },
    fetch(url, options) {
        requests.push({url, options});
        const data = replies.shift();
        if (data instanceof Error) return Promise.reject(data);
        return Promise.resolve({ok: true, json: () => Promise.resolve(data)});
    }
};
const flush = () => new Promise(resolve => setImmediate(resolve));
async function nextPoll(reply) {
    replies.push(reply);
    const task = tasks.find(t => t.delay <= 3000);
    assert.ok(task, 'poll scheduled');
    tasks = tasks.filter(t => t !== task);
    task.fn();
    await flush();
}
async function main() {
    replies.push({web_cli: true});
    replies.push({session: 1, cursor: 2, ready: true, records: [
        {source: 'log', text: '\x1b[32mBR <script>alert(1)</script>\x1b[0m\n'},
        {source: 'cli', text: 'leader\r\nDone\r\n'}
    ]});
    vm.runInNewContext(fs.readFileSync('components/esp_ot_br_server/frontend/static/web-cli.js', 'utf8'), context);
    await flush();
    assert.equal(element('cliSend').disabled, false);
    assert.match(element('cliOutput').textContent, /<script>/);
    assert.ok(!element('cliOutput').textContent.includes('\x1b'));
    element('cliLogs').checked = false;
    element('cliLogs').listeners.change();
    assert.ok(!element('cliOutput').textContent.includes('BR'));
    assert.match(element('cliOutput').textContent, /leader/);
    element('cliInput').value = 'ot state';
    replies.push({accepted: true});
    element('cliForm').listeners.submit({preventDefault() {}});
    await flush();
    assert.equal(requests.at(-1).options.body, '{"command":"ot state"}');
    element('cliInput').listeners.keydown({key: 'ArrowUp', preventDefault() {}});
    assert.equal(element('cliInput').value, 'ot state');
    const sentCount = requests.length;
    for (const invalid of ['ot state\not reset', '界'.repeat(86)]) {
        element('cliInput').value = invalid;
        element('cliForm').listeners.submit({preventDefault() {}});
        await flush();
        assert.equal(requests.length, sentCount, 'invalid command not sent');
    }
    await nextPoll({session: 1, cursor: 7, ready: true, dropped: true, records: []});
    assert.match(element('cliOutput').textContent, /overwritten/);
    await nextPoll({session: 2, cursor: 10, ready: true, records: []});
    assert.match(element('cliOutput').textContent, /Device restarted/);
    await nextPoll({session: 2, cursor: 1, ready: false, records: []});
    assert.equal(requests.at(-1).url, '/cli?after=0');
    assert.equal(element('cliSend').disabled, true);
    await nextPoll(new Error('offline'));
    assert.match(element('cliStatus').textContent, /Disconnected/);
    element('cliClear').listeners.click();
    assert.equal(element('cliOutput').textContent, '');
    tasks = []; requests = []; replies = [{web_cli: false}];
    vm.runInNewContext(fs.readFileSync('components/esp_ot_br_server/frontend/static/web-cli.js', 'utf8'), context);
    await flush();
    assert.equal(element('webCli').classList.hidden, true);
    assert.equal(requests.length, 1, 'disabled feature does not poll CLI');
    assert.equal(requests[0].url, '/web/features');
    assert.equal(tasks.length, 0, 'disabled feature has no recurring timer');
    console.log('Web CLI frontend checks passed: rendering, filtering, command, validation, history, overflow, reboot, disconnect, clear.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
