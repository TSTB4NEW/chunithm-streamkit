// ==UserScript==
// @name         CHUNITHM StreamKit 弹幕转发
// @namespace    chunithm-streamkit
// @version      1.0.0
// @description  从 blc.huixinghao.cn 的弹幕页提取弹幕，转发给本机的 chunithm-streamkit 插件
// @match        https://blc.huixinghao.cn/*
// @run-at       document-start
// @grant        none
// ==/UserScript==

(function () {
  'use strict';

  // 插件地址（游戏开着才有响应；没开时静默失败）
  var ENDPOINT = 'http://127.0.0.1:8770/dm';

  // ---- 极简 protobuf 读取：只处理 varint / length-delimited ----
  function pbFields(u8) {
    var out = [], p = 0;
    function varint() {
      var v = 0, shift = 0, b;
      do {
        if (p >= u8.length) return null;
        b = u8[p++];
        v += (b & 0x7f) * Math.pow(2, shift);
        shift += 7;
      } while (b & 0x80);
      return v;
    }
    while (p < u8.length) {
      var tag = varint();
      if (tag === null) break;
      var f = Math.floor(tag / 8), wt = tag % 8;
      if (f === 0) break;
      if (wt === 0) {
        var v = varint();
        if (v === null) break;
        out.push({ f: f, wt: 0, val: v });
      } else if (wt === 2) {
        var n = varint();
        if (n === null || p + n > u8.length) break;
        out.push({ f: f, wt: 2, val: u8.subarray(p, p + n) });
        p += n;
      } else if (wt === 5) { p += 4; }
      else if (wt === 1) { p += 8; }
      else break;
    }
    return out;
  }

  function utf8(u8) {
    try { return new TextDecoder('utf-8', { fatal: false }).decode(u8); }
    catch (e) { return ''; }
  }

  // 从内层消息里找「像弹幕」的字段：field3 是文本
  function extract(u8) {
    var fields = pbFields(u8);
    var msg = null, user = '';
    for (var i = 0; i < fields.length; ++i) {
      var x = fields[i];
      if (x.wt !== 2) continue;
      var s = utf8(x.val);
      if (x.f === 3 && s.length > 0 && s.length < 500) msg = s;
      else if (x.f === 1 && x.wt === 0) { /* uid */ }
    }
    // 如果 field3 没找到，退而求其次：取最长的可读串
    if (!msg) {
      var best = '';
      for (var j = 0; j < fields.length; ++j) {
        if (fields[j].wt !== 2) continue;
        var t = utf8(fields[j].val);
        if (t.length > best.length && /^[\x20-\x7e\u4e00-\u9fff\uff00-\uffef\u3040-\u30ff]+$/.test(t)) {
          best = t;
        }
      }
      msg = best;
    }
    return msg;
  }

  function forward(text, user) {
    if (!text) return;
    try {
      fetch(ENDPOINT, {
        method: 'POST',
        mode: 'cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user: user || '', text: text })
      }).catch(function () {});
    } catch (e) {}
  }

  // ---- 劫持 WebSocket ----
  var NativeWS = window.WebSocket;
  window.WebSocket = function (url, protocols) {
    var ws = protocols ? new NativeWS(url, protocols) : new NativeWS(url);
    try {
      if (String(url).indexOf('ws.huixinghao.cn') >= 0) {
        ws.addEventListener('message', function (e) {
          try {
            if (typeof e.data === 'string') {
              // 纯文本帧：直接找 JSON 里的 content
              var m = e.data.match(/"content"\s*:\s*"((?:[^"\\]|\\.)*)"/);
              if (m) forward(JSON.parse('"' + m[1] + '"'));
              return;
            }
            var buf = new Uint8Array(e.data);
            var outer = pbFields(buf);
            var data = null;
            for (var i = 0; i < outer.length; ++i) {
              if (outer[i].f === 2 && outer[i].wt === 2) data = outer[i].val;
            }
            if (!data) return;
            var text = extract(data);
            if (text) forward(text);
          } catch (err) {}
        });
      }
    } catch (err) {}
    return ws;
  };
  window.WebSocket.prototype = NativeWS.prototype;
  ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'].forEach(function (k) {
    window.WebSocket[k] = NativeWS[k];
  });

  console.log('[CSK] 弹幕转发已启用 → ' + ENDPOINT);
})();
