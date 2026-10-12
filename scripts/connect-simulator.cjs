#!/usr/bin/env node
/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
const fs = require('node:fs'), {spawn} = require('node:child_process'), {createInterface} = require('node:readline');
const WebSocket = require('ws');
const [binary, credentialsFile, resource, journal] = process.argv.slice(2);
if (!binary || !credentialsFile || !resource || !journal) throw new Error('Usage: node scripts/connect-simulator.cjs SIMULATOR CREDENTIALS RESOURCE JOURNAL');
const credentials = JSON.parse(fs.readFileSync(credentialsFile, 'utf8')), url = new URL(credentials.gatewayUrl);
if (url.username || url.password || url.search || url.hash || !['ws:', 'wss:'].includes(url.protocol) || (url.protocol === 'ws:' && !['127.0.0.1', '[::1]', 'localhost'].includes(url.hostname))) throw new Error('Use WSS for remote gateways');
const child = spawn(binary, [credentials.deviceId, resource, journal], {stdio: ['pipe', 'pipe', 'inherit']});
let hello, socket, timer, closed = false, retries = 0;
function connect() {
  if (closed) return;
  socket = new WebSocket(url, {headers: {Authorization: 'Bearer ' + credentials.deviceToken}, maxPayload: 4096});
  socket.on('open', () => {retries = 0; if (hello) socket.send(hello);});
  socket.on('message', data => {if (data.length <= 4096) child.stdin.write(data + '\n');});
  socket.on('error', () => {});
  socket.on('close', code => {if (closed) return; if (code === 1008) {console.error('Device authentication/policy rejected'); stop(); return;} timer = setTimeout(connect, Math.min(10000, 500 * 2 ** retries++));});
}
createInterface({input: child.stdout}).on('line', value => {if (Buffer.byteLength(value) > 4096) return stop(); if (!hello) {hello = value; connect();} else if (socket?.readyState === 1) socket.send(value);});
child.on('error', error => {console.error(error.message); stop();}); child.on('exit', () => stop());
function stop() {if (closed) return; closed = true; clearTimeout(timer); socket?.terminate(); child.stdin.end(); child.kill();}
process.once('SIGINT', stop); process.once('SIGTERM', stop);
