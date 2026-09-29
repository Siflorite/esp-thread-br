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
    var lastEventId = '', records = [], history = [], historyIndex = 0;
    var paused = false, ready = false, sending = false, stopped = false;
    var enabled = false, stream = null, featureGeneration = 0, featureTimer, reconnectTimer;
    var renderFrame = null;
    send.disabled = true;

    function render() {
        var text = records.filter(function(r) { return logs.checked || r.source !== 'log'; })
            .map(function(r) {
                return r.source === 'input' ? '\n> ' + r.text + '\n' : r.text;
            }).join('');
        // Device text is never HTML; remove terminal color/control sequences.
        output.textContent = text.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
            .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, '');
        if (follow.checked) output.scrollTop = output.scrollHeight;
    }
    function add(items) {
        records = records.concat(items).slice(-800);
        if (renderFrame === null) {
            renderFrame = requestAnimationFrame(function() {
                renderFrame = null;
                render();
            });
        }
    }
    function notice(text) { add([{source: 'cli', text: '\n[' + text + ']\n'}]); }
    function disconnect() {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
        var old = stream;
        stream = null; // Ignore already queued events from the old connection.
        if (old) old.close();
        ready = false;
        send.disabled = true;
    }
    function connect() {
        if (stream || reconnectTimer || !enabled || stopped || paused || document.hidden) return;
        ready = false;
        send.disabled = true;
        status.textContent = 'Connecting...';
        if (typeof EventSource === 'undefined') {
            status.textContent = 'EventSource unavailable';
            return; // No polling or fallback GET transport.
        }
        // Native reconnect retains Last-Event-ID. Only manually recreated streams
        // use the query parameter, keeping decimal cursors as strings throughout.
        var source = new EventSource('/cli/events' + (lastEventId ? '?after=' + encodeURIComponent(lastEventId) : ''));
        stream = source;
        var receivedStatus = false;
        function receive(event) {
            if (stream !== source || stopped || paused || document.hidden || !enabled) return;
            var data;
            try { data = JSON.parse(event.data); } catch (err) { return; }
            if (!data || typeof data.session !== 'string' || !/^\d+$/.test(data.session) ||
                typeof data.cursor !== 'string' || !/^\d+$/.test(data.cursor) ||
                typeof data.ready !== 'boolean' || event.lastEventId !== data.session + ':' + data.cursor) return;
            if (event.type === 'records' && (!Array.isArray(data.records) || data.records.length > 16 ||
                !data.records.every(function(r) {
                    return r && typeof r.seq === 'string' && /^\d+$/.test(r.seq) &&
                        ['log', 'input', 'cli'].indexOf(r.source) !== -1 && typeof r.text === 'string';
                }))) return;
            lastEventId = event.lastEventId;
            if (event.type === 'gap') notice('Older device output was overwritten');
            if (event.type === 'reset') notice('Device output restarted');
            if (event.type === 'records') add(data.records);
            if (event.type === 'status') receivedStatus = true;
            ready = receivedStatus && data.ready;
            send.disabled = !ready || sending;
            status.textContent = ready ? 'Connected' : 'Logs connected; console unavailable';
        }
        ['status', 'records', 'gap', 'reset'].forEach(function(type) { source.addEventListener(type, receive); });
        source.onerror = function() {
            if (stream !== source || stopped || paused || document.hidden || !enabled) return;
            receivedStatus = false;
            ready = false;
            send.disabled = true;
            status.textContent = 'Disconnected; reconnecting...';
            // CONNECTING means native Last-Event-ID reconnect is still active.
            // A fatal HTTP response (e.g. 503) may instead leave it CLOSED.
            if (source.readyState === 2) {
                disconnect();
                reconnectTimer = setTimeout(function() {
                    reconnectTimer = null;
                    connect();
                }, 3000);
            }
        };
    }
    document.getElementById('cliForm').addEventListener('submit', function(event) {
        event.preventDefault();
        var command = input.value;
        if (!command.trim() || sending || !ready) return;
        if (new TextEncoder().encode(command).length > 255 || /[\x00-\x1f\x7f]/.test(command)) {
            notice('Enter one command, at most 255 UTF-8 bytes');
            return;
        }
        sending = true;
        send.disabled = true;
        var controller = new AbortController();
        var timeout = setTimeout(function() { controller.abort(); }, 8000);
        var commandStream = stream;
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
            if (stream === commandStream && ready) status.textContent = 'Accepted; waiting for console output';
        }).catch(function(err) {
            notice(err.name === 'AbortError' ? 'Request timed out; execution is uncertain. Check output before retrying.' : 'Send failed: ' + err.message);
        }).finally(function() {
            clearTimeout(timeout);
            sending = false;
            send.disabled = !ready;
            if (!stopped && !document.hidden) input.focus();
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
        if (paused) { disconnect(); status.textContent = 'Paused'; }
        else connect();
    });
    document.getElementById('cliClear').addEventListener('click', function() { records = []; render(); });
    logs.addEventListener('change', render);
    document.addEventListener('visibilitychange', function() {
        if (document.hidden) { disconnect(); status.textContent = paused ? 'Paused' : 'Hidden'; }
        else connect();
    });
    window.addEventListener('pagehide', function() {
        stopped = true;
        ++featureGeneration;
        clearTimeout(featureTimer);
        disconnect();
    });
    function loadFeatures() {
        var generation = ++featureGeneration;
        var controller = new AbortController();
        var timeout = setTimeout(function() { controller.abort(); }, 8000);
        fetch('/web/features', {cache: 'no-store', signal: controller.signal})
            .then(function(r) {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.json();
            }).then(function(features) {
                if (stopped || generation !== featureGeneration) return;
                enabled = !!features.web_cli;
                document.getElementById('webCli').classList.toggle('hidden', !enabled);
                if (enabled) connect();
                else disconnect();
            }).catch(function() {
                if (!stopped && generation === featureGeneration) featureTimer = setTimeout(loadFeatures, 3000);
            }).finally(function() { clearTimeout(timeout); });
    }
    window.addEventListener('pageshow', function(event) {
        if (event.persisted) { stopped = false; enabled = false; loadFeatures(); }
    });
    loadFeatures();
})();
