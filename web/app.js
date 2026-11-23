'use strict';
// Qwen3-TTS 本機介面前端：呼叫 t2v-server 的 /api/*

const SR = 24000;
const FPS = 12.5; // codec frames per second
const $ = (s, el = document) => el.querySelector(s);

const LANG_ZH = {
  auto: '自動偵測', chinese: '中文', english: '英文', japanese: '日文', korean: '韓文',
  german: '德文', french: '法文', russian: '俄文', portuguese: '葡萄牙文', spanish: '西班牙文', italian: '義大利文',
};

const PRESETS = {
  '沉穩男性旁白': 'A calm, low-pitched middle-aged male narrator, slow pacing, warm and steady tone.',
  '活潑年輕女聲': 'A cheerful young female voice, bright timbre, fast and lively pacing, smiling tone.',
  '溫柔講故事': 'A gentle, soft female storyteller voice, slightly breathy, slow and soothing, like reading a bedtime story.',
  '新聞主播': 'A professional news anchor, clear articulation, neutral and confident tone, medium pace.',
  '低語耳語': 'A whispering voice, very soft and intimate, close to the microphone.',
  '老爺爺': 'An elderly man in his seventies, slightly hoarse, slow and kind, with a gentle laugh in his voice.',
  '台灣腔年輕男生': '一位二十多歲的台灣年輕男生，語氣輕鬆自然，語速中等，帶一點笑意。',
};

// ------------------------------------------------------------ 共用
async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  if (!r.ok) {
    let msg = `${r.status} ${r.statusText}`;
    try { msg = (await r.json()).error || msg; } catch (_) { /* not json */ }
    throw new Error(msg);
  }
  return r;
}

function wavFromFloat(f32, sr = SR) {
  const buf = new ArrayBuffer(44 + f32.length * 2);
  const v = new DataView(buf);
  const w = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  w(0, 'RIFF'); v.setUint32(4, 36 + f32.length * 2, true); w(8, 'WAVE');
  w(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, sr, true); v.setUint32(28, sr * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  w(36, 'data'); v.setUint32(40, f32.length * 2, true);
  for (let i = 0; i < f32.length; i++) {
    const x = Math.max(-1, Math.min(1, f32[i] || 0));
    v.setInt16(44 + i * 2, Math.round(x * 32767), true);
  }
  return new Blob([buf], { type: 'audio/wav' });
}

// 任意格式（mp3 / m4a / webm 錄音…）→ 24 kHz 單聲道 WAV
async function toWav24k(blob) {
  const ab = await blob.arrayBuffer();
  const ac = new AudioContext();
  let dec;
  try { dec = await ac.decodeAudioData(ab); } finally { ac.close(); }
  const len = Math.max(1, Math.ceil(dec.duration * SR));
  const off = new OfflineAudioContext(1, len, SR);
  const src = off.createBufferSource();
  src.buffer = dec; src.connect(off.destination); src.start();
  const out = await off.startRendering();
  return { blob: wavFromFloat(out.getChannelData(0)), secs: dec.duration };
}

const wavSecs = (blob) => Math.max(0, blob.size - 44) / 2 / SR;

function persist() {
  document.querySelectorAll('[data-persist]').forEach((el) => {
    const key = 't2v:' + el.id;
    try {
      const v = localStorage.getItem(key);
      if (v !== null) { if (el.type === 'checkbox') el.checked = v === '1'; else el.value = v; }
    } catch (_) { /* storage unavailable */ }
    el.addEventListener('change', () => {
      try { localStorage.setItem(key, el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value); } catch (_) { /* ignore */ }
    });
  });
}

// ------------------------------------------------------------ 參數面板
function buildParams(host) {
  host.appendChild($('#params-tpl').content.cloneNode(true));
  const inputs = host.querySelectorAll('input');
  const key = 't2v:params:' + host.id;
  const show = () => host.querySelectorAll('input[type=range]').forEach((i) => {
    i.parentElement.querySelector('.v').textContent = i.name === 'max_sec' ? `${i.value} 秒` : i.value;
  });
  try {
    const saved = JSON.parse(localStorage.getItem(key) || '{}');
    inputs.forEach((i) => { if (saved[i.name] !== undefined) i.value = saved[i.name]; });
  } catch (_) { /* ignore */ }
  const save = () => {
    show();
    const o = {}; inputs.forEach((i) => { o[i.name] = i.value; });
    try { localStorage.setItem(key, JSON.stringify(o)); } catch (_) { /* ignore */ }
  };
  inputs.forEach((i) => i.addEventListener('input', save));
  $('.reset', host).addEventListener('click', () => { inputs.forEach((i) => { i.value = i.defaultValue; }); save(); });
  show();
  return {
    get() {
      const g = (n) => host.querySelector(`[name=${n}]`).value;
      return {
        temperature: +g('temperature'), top_p: +g('top_p'), top_k: +g('top_k'),
        repetition_penalty: +g('repetition_penalty'),
        max_new_tokens: Math.round(+g('max_sec') * FPS),
        seed: parseInt(g('seed'), 10),
      };
    },
    setSeed(s) { host.querySelector('[name=seed]').value = s; save(); host.querySelector('details').open = true; },
  };
}

// ------------------------------------------------------------ 結果區
function makeResult(root, historyEl) {
  const audio = $('audio', root), info = $('.info', root), dl = $('.dl', root);
  const keep = $('.keep-seed', root), toRef = $('.to-ref', root);
  let last = null;
  return {
    get last() { return last; },
    msg(t, err = false) { info.textContent = t; info.classList.toggle('err', err); },
    show(blob, meta) {
      const url = URL.createObjectURL(blob);
      last = { ...meta, blob, url };
      audio.src = url;
      if (!meta.streamed) audio.play().catch(() => {});
      const rtf = meta.secs > 0 ? (meta.gen / meta.secs).toFixed(2) : '-';
      this.msg(`音訊 ${meta.secs.toFixed(1)} 秒｜耗時 ${meta.gen.toFixed(1)} 秒｜RTF ${rtf}｜seed ${meta.seed}`);
      dl.href = url; dl.download = meta.file || 'tts.wav'; dl.hidden = false;
      keep.hidden = false; if (toRef) toRef.hidden = false;
      const li = document.createElement('li');
      li.innerHTML = '<div class="t"></div><div class="muted"></div><audio controls preload="none"></audio>';
      $('.t', li).textContent = meta.text;
      $('.muted', li).textContent = `${meta.secs.toFixed(1)} 秒｜seed ${meta.seed}${meta.voice ? '｜' + meta.voice : ''}`;
      $('audio', li).src = url;
      historyEl.prepend(li);
      while (historyEl.children.length > 20) {
        const old = historyEl.lastChild; URL.revokeObjectURL($('audio', old).src); old.remove();
      }
    },
  };
}

// ------------------------------------------------------------ 生成（含串流播放）
let busy = null; // { ctrl, ctx }

async function playStream(res, onProgress) {
  const ctx = new AudioContext({ sampleRate: SR });
  busy.ctx = ctx;
  if (ctx.state === 'suspended') await ctx.resume().catch(() => {});
  const reader = res.body.getReader();
  const parts = []; let total = 0; let carry = null;
  let t = ctx.currentTime + 0.3; // 先緩衝 0.3 秒，避免一開始斷斷續續
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    let bytes = value;
    if (carry) { const m = new Uint8Array(carry.length + bytes.length); m.set(carry); m.set(bytes, carry.length); bytes = m; carry = null; }
    const n = bytes.length >> 1;
    if (bytes.length & 1) carry = bytes.slice(bytes.length - 1);
    if (!n) continue;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, n * 2);
    const f = new Float32Array(n);
    for (let i = 0; i < n; i++) f[i] = dv.getInt16(i * 2, true) / 32768;
    parts.push(f); total += n;
    if (ctx.state !== 'closed') {
      const buf = ctx.createBuffer(1, n, SR); buf.copyToChannel(f, 0);
      const src = ctx.createBufferSource(); src.buffer = buf; src.connect(ctx.destination);
      if (t < ctx.currentTime + 0.02) t = ctx.currentTime + 0.05;
      src.start(t); t += n / SR;
    }
    onProgress(total / SR);
  }
  const remain = Math.max(0, t - ctx.currentTime);
  setTimeout(() => { if (ctx.state !== 'closed') ctx.close(); }, remain * 1000 + 500);
  const all = new Float32Array(total); let o = 0;
  for (const p of parts) { all.set(p, o); o += p.length; }
  return all;
}

async function generate(kind, body, ui) {
  if (busy) return;
  busy = { ctrl: new AbortController(), ctx: null };
  ui.setBusy(true);
  ui.result.msg(body.stream ? '生成中…（會邊生成邊播放）' : '生成中…');
  const t0 = performance.now();
  try {
    const res = await api(`/api/${kind}`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body), signal: busy.ctrl.signal,
    });
    const seed = res.headers.get('X-Seed');
    const file = decodeURIComponent(res.headers.get('X-File') || 'tts.wav');
    let blob, secs;
    if (body.stream) {
      const f = await playStream(res, (s) => ui.result.msg(`生成中… 已產生 ${s.toFixed(1)} 秒`));
      blob = wavFromFloat(f); secs = f.length / SR;
    } else {
      blob = await res.blob(); secs = wavSecs(blob);
    }
    const gen = (performance.now() - t0) / 1000;
    ui.result.show(blob, { seed, file, secs, gen, text: body.text, voice: body.voice, streamed: body.stream });
  } catch (e) {
    if (e.name === 'AbortError' || /已取消/.test(e.message)) ui.result.msg('已停止');
    else ui.result.msg('錯誤：' + e.message, true);
  } finally {
    ui.setBusy(false);
    busy = null;
  }
}

function stopGeneration() {
  if (!busy) return;
  fetch('/api/cancel', { method: 'POST' }).catch(() => {});
  busy.ctrl.abort();
  if (busy.ctx && busy.ctx.state !== 'closed') busy.ctx.close();
}

function makeUi(prefix) {
  const go = $(`#${prefix}-go`), stop = $(`#${prefix}-stop`);
  stop.addEventListener('click', stopGeneration);
  return {
    go,
    result: makeResult($(`#${prefix}-result`), $(`#${prefix}-history`)),
    params: buildParams($(`#${prefix}-params`)),
    setBusy(b) {
      document.querySelectorAll('#d-go, #c-go').forEach((x) => { x.disabled = b; });
      stop.disabled = !b;
    },
  };
}

// ------------------------------------------------------------ 分頁
function switchTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('active', p.id === 'tab-' + name));
}
document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => switchTab(t.dataset.tab)));

// ------------------------------------------------------------ VoiceDesign
const D = makeUi('d');
const presetSel = $('#d-preset');
for (const k of Object.keys(PRESETS)) presetSel.add(new Option(k, k));
presetSel.addEventListener('change', () => {
  if (presetSel.value) { $('#d-inst').value = PRESETS[presetSel.value]; $('#d-inst').dispatchEvent(new Event('change')); }
});
if (!$('#d-inst').value) $('#d-inst').value = PRESETS['沉穩男性旁白'];

D.go.addEventListener('click', () => generate('design', {
  text: $('#d-text').value, instruct: $('#d-inst').value, lang: $('#d-lang').value,
  stream: $('#d-stream').checked, ...D.params.get(),
}, D));
$('#d-result .keep-seed').addEventListener('click', () => D.result.last && D.params.setSeed(D.result.last.seed));
$('#d-result .to-ref').addEventListener('click', () => {
  const l = D.result.last; if (!l) return;
  setRef(l.blob, l.secs, '來自「設計聲音」');
  $('#c-ref-text').value = l.text;
  $('#c-xvec').checked = false;
  switchTab('clone');
  $('#c-name').focus();
});

// ------------------------------------------------------------ Clone：參考音
let refWav = null; // 24 kHz mono WAV Blob
function setRef(wavBlob, secs, label) {
  refWav = wavBlob;
  const a = $('#c-ref');
  if (a.src) URL.revokeObjectURL(a.src);
  a.src = URL.createObjectURL(wavBlob); a.hidden = false;
  const warn = secs < 3 ? '（偏短，建議 3 秒以上）' : secs > 20 ? '（偏長，建議 15 秒內）' : '';
  $('#c-ref-info').textContent = `${label}｜${secs.toFixed(1)} 秒${warn}`;
}

$('#c-file').addEventListener('change', async (e) => {
  const f = e.target.files[0]; if (!f) return;
  try {
    const { blob, secs } = await toWav24k(f);
    setRef(blob, secs, f.name);
    if (!$('#c-name').value) $('#c-name').value = f.name.replace(/\.[^.]+$/, '');
  } catch (err) { $('#c-ref-info').textContent = '無法讀取這個音檔：' + err.message; }
  e.target.value = '';
});

let rec = null;
$('#c-rec').addEventListener('click', async () => {
  const btn = $('#c-rec');
  if (rec) { rec.stop(); return; }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1 } });
    const chunks = [];
    rec = new MediaRecorder(stream);
    rec.ondataavailable = (ev) => { if (ev.data.size) chunks.push(ev.data); };
    rec.onstop = async () => {
      stream.getTracks().forEach((t) => t.stop());
      btn.textContent = '● 錄音'; btn.classList.remove('recording'); rec = null;
      try {
        const { blob, secs } = await toWav24k(new Blob(chunks, { type: chunks[0] ? chunks[0].type : 'audio/webm' }));
        setRef(blob, secs, '錄音');
      } catch (err) { $('#c-ref-info').textContent = '錄音轉換失敗：' + err.message; }
    };
    rec.start();
    btn.textContent = '■ 停止錄音'; btn.classList.add('recording');
    $('#c-ref-info').textContent = '錄音中… 念完逐字稿上的句子後按停止';
  } catch (err) { $('#c-ref-info').textContent = '無法使用麥克風：' + err.message; }
});

$('#c-create').addEventListener('click', async () => {
  const msg = $('#c-msg');
  if (!refWav) { msg.textContent = '請先提供參考音'; return; }
  const name = $('#c-name').value.trim();
  if (!name) { msg.textContent = '請替聲音取名字'; return; }
  const fd = new FormData();
  fd.append('name', name);
  fd.append('ref_text', $('#c-ref-text').value);
  fd.append('xvec', $('#c-xvec').checked ? '1' : '0');
  fd.append('audio', refWav, 'ref.wav');
  const btn = $('#c-create'); btn.disabled = true; msg.textContent = '擷取聲音特徵中…';
  try {
    const r = await (await api('/api/voices', { method: 'POST', body: fd })).json();
    msg.textContent = `已建立「${r.name}」`;
    await loadVoices(r.name);
  } catch (e) { msg.textContent = '錯誤：' + e.message; } finally { btn.disabled = false; }
});

// ------------------------------------------------------------ Clone：聲音庫
let selected = null;
try { selected = localStorage.getItem('t2v:voice'); } catch (_) { /* ignore */ }

function selectVoice(name) {
  selected = name;
  try { localStorage.setItem('t2v:voice', name || ''); } catch (_) { /* ignore */ }
  document.querySelectorAll('#c-voices li[data-name]').forEach((li) => li.classList.toggle('sel', li.dataset.name === name));
  $('#c-selected').textContent = name ? `使用聲音：${name}` : '尚未選擇聲音';
}

async function loadVoices(select) {
  const ul = $('#c-voices');
  let list = [];
  try { list = await (await api('/api/voices')).json(); } catch (e) { ul.innerHTML = ''; ul.textContent = '讀取失敗：' + e.message; return; }
  ul.innerHTML = '';
  if (!list.length) { ul.innerHTML = '<li class="muted">（還沒有）</li>'; selectVoice(null); return; }
  for (const v of list) {
    const li = document.createElement('li');
    li.dataset.name = v.name;
    li.innerHTML = '<span class="name"></span><span class="badge"></span><span class="muted secs"></span>' +
      '<button class="ghost small play" title="試聽參考音">▶</button><button class="ghost small del" title="刪除">🗑</button>';
    $('.name', li).textContent = v.name;
    $('.name', li).title = v.ref_text || '';
    $('.badge', li).textContent = v.icl ? '聲紋＋逐字稿' : '只用聲紋';
    $('.secs', li).textContent = v.seconds ? `${v.seconds.toFixed(1)}s` : '';
    li.addEventListener('click', () => selectVoice(v.name));
    $('.play', li).addEventListener('click', (e) => {
      e.stopPropagation();
      new Audio('/api/voice-audio?name=' + encodeURIComponent(v.name)).play().catch(() => {});
    });
    $('.del', li).addEventListener('click', async (e) => {
      e.stopPropagation();
      if (!confirm(`刪除聲音「${v.name}」？（voices 資料夾裡的檔案會一起刪除）`)) return;
      try { await api('/api/voices?name=' + encodeURIComponent(v.name), { method: 'DELETE' }); } catch (err) { alert(err.message); }
      loadVoices();
    });
    ul.appendChild(li);
  }
  const want = select || selected;
  selectVoice(list.some((v) => v.name === want) ? want : list[0].name);
}

const C = makeUi('c');
C.go.addEventListener('click', () => {
  if (!selected) { C.result.msg('請先在左邊選擇或建立一個聲音', true); return; }
  generate('clone', {
    voice: selected, text: $('#c-text').value, lang: $('#c-lang').value,
    stream: $('#c-stream').checked, ...C.params.get(),
  }, C);
});
$('#c-result .keep-seed').addEventListener('click', () => C.result.last && C.params.setSeed(C.result.last.seed));

// ------------------------------------------------------------ 啟動
async function init() {
  persist();
  const st = $('#status');
  try {
    const info = await (await api('/api/info')).json();
    const langs = ['auto', ...info.languages.filter((l) => l !== 'auto')];
    document.querySelectorAll('select.lang').forEach((sel) => {
      const keep = sel.value;
      sel.innerHTML = '';
      for (const l of langs) sel.add(new Option(LANG_ZH[l] || l, l));
      if (keep) sel.value = keep;
    });
    const parts = [`VoiceDesign ${info.design ? '✓' : '✗'}`, `Base ${info.base ? '✓' : '✗'}`];
    st.textContent = parts.join('　');
    st.title = 'qwentts.cpp ' + info.version;
    st.className = 'pill ' + (info.design && info.base ? 'ok' : 'bad');
    if (!info.design) D.result.msg('VoiceDesign 模型沒有載入，請執行 build.bat 下載模型。', true);
    if (!info.base) { C.result.msg('Base 模型沒有載入，無法使用固定聲音。', true); $('#c-create').disabled = true; }
  } catch (e) {
    st.textContent = '連不到伺服器'; st.className = 'pill bad';
  }
  loadVoices();
}
init();
