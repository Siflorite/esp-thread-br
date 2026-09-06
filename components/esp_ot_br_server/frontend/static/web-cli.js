/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
'use strict';
(function() {
    var output = document.getElementById('cliOutput');
    var input = document.getElementById('cliInput');
    var send = document.getElementById('cliSend');
    var status = document.getElementById('cliStatus');
    var logs = document.getElementById('cliLogs');
    var follow = document.getElementById('cliFollow');
    var pause = document.getElementById('cliPause');
    var cursor = 0, records = [], history = [], historyIndex = 0;
    var session = null;
    var paused = false, ready = false, sending = false, stopped = false, timer;

    function render() {
        var text = records.filter(function(r) { return logs.checked || r.source !== 'log'; })
            .map(function(r) {
                return r.source === 'input' ? '\n> ot ' + r.text + '\n' : r.text;
            }).join('');
        // Device text is never HTML; remove terminal color/control sequences.
        output.textContent = text.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
            .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, '');
        if (follow.checked) output.scrollTop = output.scrollHeight;
    }
    function add(items) {
        records = records.concat(items).slice(-800);
        render();
    }
    function notice(text) { add([{source: 'cli', text: '\n[' + text + ']\n'}]); }
    function schedule(delay) {
        clearTimeout(timer);
        if (!stopped) timer = setTimeout(poll, delay);
    }
    function poll() {
        if (paused || document.hidden) { schedule(1000); return; }
        var controller = new AbortController();
        var timeout = setTimeout(function() { controller.abort(); }, 8000);
        var delay = 1000;
        fetch('/cli?after=' + cursor, {cache: 'no-store', signal: controller.signal})
            .then(function(r) {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.json();
            }).then(function(data) {
                if (stopped) return;
                if (session !== null && session !== data.session) {
                    notice('Device restarted');
                    session = data.session;
                    cursor = 0;
                    delay = 100;
                    return;
                }
                session = data.session;
                if (data.reset) notice('Device output restarted');
                if (data.dropped) notice('Older device output was overwritten');
                add(data.records || []);
                cursor = data.cursor;
                ready = data.ready;
                send.disabled = !ready || sending;
                status.textContent = paused ? 'Paused' : ready ? 'Connected' : 'Logs connected; OT CLI unavailable';
                if (data.more) delay = 100;
            }).catch(function() {
                ready = false;
                send.disabled = true;
                status.textContent = 'Disconnected; reconnecting...';
                delay = 3000;
            }).finally(function() { clearTimeout(timeout); schedule(delay); });
    }
    document.getElementById('cliForm').addEventListener('submit', function(event) {
        event.preventDefault();
        var command = input.value.trim();
        if (!command || sending || !ready) return;
        if (new TextEncoder().encode(command).length > 255 || /[\x00-\x1f\x7f]/.test(command)) {
            notice('Enter one command, at most 255 UTF-8 bytes');
            return;
        }
        sending = true;
        send.disabled = true;
        var controller = new AbortController();
        var timeout = setTimeout(function() { controller.abort(); }, 8000);
        fetch('/cli', {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({command: command}), signal: controller.signal
        }).then(function(r) {
            if (!r.ok) return r.text().then(function(text) { throw new Error(text || 'HTTP ' + r.status); });
            return r.json();
        }).then(function() {
            history.push(command);
            history = history.slice(-50);
            historyIndex = history.length;
            input.value = '';
            status.textContent = 'Queued; waiting for CLI output';
        }).catch(function(err) {
            notice(err.name === 'AbortError' ? 'Request timed out; execution is uncertain. Check output before retrying.' : 'Send failed: ' + err.message);
        }).finally(function() {
            clearTimeout(timeout);
            sending = false;
            send.disabled = !ready;
            input.focus();
        });
    });
    input.addEventListener('keydown', function(event) {
        if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
        event.preventDefault();
        historyIndex = Math.max(0, Math.min(history.length, historyIndex + (event.key === 'ArrowUp' ? -1 : 1)));
        input.value = history[historyIndex] || '';
    });
    pause.addEventListener('click', function() {
        paused = !paused;
        pause.textContent = paused ? 'Resume' : 'Pause';
        status.textContent = paused ? 'Paused' : 'Reconnecting...';
    });
    document.getElementById('cliClear').addEventListener('click', function() { records = []; render(); });
    logs.addEventListener('change', render);
    window.addEventListener('pagehide', function() { stopped = true; clearTimeout(timer); });
    function loadFeatures() {
        var controller = new AbortController();
        var timeout = setTimeout(function() { controller.abort(); }, 8000);
        fetch('/web/features', {cache: 'no-store', signal: controller.signal})
            .then(function(r) {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.json();
            }).then(function(features) {
                if (stopped) return;
                document.getElementById('webCli').classList.toggle('hidden', !features.web_cli);
                if (features.web_cli) poll();
            }).catch(function() {
                if (!stopped) timer = setTimeout(loadFeatures, 3000);
            }).finally(function() { clearTimeout(timeout); });
    }
    window.addEventListener('pageshow', function(event) {
        if (event.persisted) { stopped = false; loadFeatures(); }
    });
    loadFeatures();
})();
