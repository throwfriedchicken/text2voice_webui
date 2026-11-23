// t2v-server.cpp: Qwen3-TTS 本機網頁應用（VoiceDesign + 固定聲音 / Voice Clone）
//
// 建立在 qwentts.cpp 的 C ABI (qwen.h) 上：一個行程同時載入
//   - VoiceDesign talker (用文字描述設計聲音)
//   - Base talker        (用參考音複製聲音)
// 並以 cpp-httplib 提供靜態網頁與 JSON / 音訊 API。完全不需要 Python。
//
// API
//   GET  /api/info                     模型狀態、可用語言
//   POST /api/design   JSON            VoiceDesign 生成 (stream=true 回傳 s16le PCM 串流)
//   POST /api/clone    JSON            用已存聲音生成   (同上)
//   POST /api/cancel                   取消進行中的生成
//   GET  /api/voices                   已存聲音清單
//   POST /api/voices   multipart       建立聲音 (name, ref_text, xvec, audio=WAV)
//   DELETE /api/voices?name=...        刪除聲音
//   GET  /api/voice-audio?name=...     取得聲音的參考音 WAV
//
// 聲音存成 voices/<name>.spk / .rvq / .txt / .wav，格式與 qwen-tts CLI 的
// --ref-spk / --ref-rvq / --ref-text 相同，可以互通。

#ifndef NOMINMAX
#    define NOMINMAX
#endif

// httplib 必須最先引入：它在 Windows 上要先於 windows.h 引入 winsock2.h。
#include "httplib.h"

#include "audio-io.h"
#include "qwen.h"
#include "rvq-file.h"
#include "version.h"
#include "yyjson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const int SR       = 24000;
static const int RVQ_BITS = 11;  // Qwen3-TTS 12 Hz codec: 2048 entries per codebook

// ------------------------------------------------------------------ 全域狀態
static qt_context *           g_design = nullptr;
static qt_context *           g_base   = nullptr;
static fs::path               g_voices_dir;
static fs::path               g_out_dir;
static std::atomic<uint64_t>  g_cancel_epoch{ 0 };

struct Voice {
    std::vector<float>   spk;    // speaker embedding
    std::vector<int32_t> codes;  // [K, T] row-major, ICL 模式才有
    int                  T = 0;
    std::string          ref_text;
};

static std::mutex                                          g_vmu;
static std::map<std::string, std::shared_ptr<const Voice>> g_voices;

// ------------------------------------------------------------------ 小工具
static fs::path u8p(const std::string & s) {
    return fs::u8path(s);
}

static bool read_file(const fs::path & p, std::string & out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool write_file(const fs::path & p, const void * data, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write((const char *) data, (std::streamsize) n);
    return (bool) f;
}

static std::string trim(const std::string & s) {
    const char * ws = " \t\r\n";
    size_t       a  = s.find_first_not_of(ws);
    if (a == std::string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// 檔名安全化：去掉 Windows 不允許的字元，最多 40 個字（UTF-8 字元）。
static std::string sanitize_name(const std::string & in) {
    std::string o;
    int         chars = 0;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char) in[i];
        if ((c & 0xC0) != 0x80) {  // 新字元的開頭
            if (++chars > 40) {
                break;
            }
        }
        if (c < 32 || std::strchr("\\/:*?\"<>|", (int) c)) {
            o += '_';
        } else {
            o += (char) c;
        }
    }
    o = trim(o);
    while (!o.empty() && o.back() == '.') {
        o.pop_back();
    }
    while (!o.empty() && o.front() == '.') {
        o.erase(o.begin());
    }
    return o;
}

static std::string pct_encode(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string         o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            o += (char) c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

static void s16_append(std::string & dst, const float * s, int n) {
    size_t off = dst.size();
    dst.resize(off + (size_t) n * 2);
    for (int i = 0; i < n; i++) {
        float x = s[i];
        if (!(x == x)) {
            x = 0.0f;
        }
        x         = std::max(-1.0f, std::min(1.0f, x));
        int16_t v = (int16_t) std::lrintf(x * 32767.0f);
        std::memcpy(&dst[off + (size_t) i * 2], &v, 2);
    }
}

// ------------------------------------------------------------------ JSON
static std::string jstr(yyjson_val * o, const char * k, const char * def = "") {
    yyjson_val * v = yyjson_obj_get(o, k);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(def);
}

static double jnum(yyjson_val * o, const char * k, double def) {
    yyjson_val * v = yyjson_obj_get(o, k);
    return yyjson_is_num(v) ? yyjson_get_num(v) : def;
}

static bool jbool(yyjson_val * o, const char * k, bool def) {
    yyjson_val * v = yyjson_obj_get(o, k);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}

static void send_json(httplib::Response & res, yyjson_mut_doc * doc) {
    size_t len = 0;
    char * s   = yyjson_mut_write(doc, 0, &len);
    res.set_content(std::string(s ? s : "{}", s ? len : 2), "application/json; charset=utf-8");
    free(s);
    yyjson_mut_doc_free(doc);
}

static void json_error(httplib::Response & res, int status, const std::string & msg) {
    yyjson_mut_doc * d = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val * r = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, r);
    yyjson_mut_obj_add_strcpy(d, r, "error", msg.c_str());
    res.status = status;
    send_json(res, d);
}

static void json_ok(httplib::Response & res) {
    yyjson_mut_doc * d = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val * r = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, r);
    yyjson_mut_obj_add_bool(d, r, "ok", true);
    send_json(res, d);
}

// ------------------------------------------------------------------ 聲音庫
static void load_voices() {
    if (!g_base) {
        return;
    }
    const int       K = qt_num_codebooks(g_base);
    std::error_code ec;
    for (auto it = fs::directory_iterator(g_voices_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path & p = it->path();
        if (p.extension() != ".spk") {
            continue;
        }
        std::string name = p.stem().u8string();
        std::string buf;
        if (!read_file(p, buf) || buf.empty() || buf.size() % 4) {
            continue;
        }
        auto v = std::make_shared<Voice>();
        v->spk.resize(buf.size() / 4);
        std::memcpy(v->spk.data(), buf.data(), buf.size());

        std::string txt, rvq;
        if (read_file(g_voices_dir / u8p(name + ".txt"), txt)) {
            v->ref_text = trim(txt);
        }
        if (!v->ref_text.empty() && read_file(g_voices_dir / u8p(name + ".rvq"), rvq)) {
            if (!rvq_read_buf((const uint8_t *) rvq.data(), rvq.size(), K, RVQ_BITS, v->codes, &v->T)) {
                v->codes.clear();
                v->T = 0;
            }
        }
        if (v->codes.empty()) {
            v->ref_text.clear();  // 沒有 RVQ 就只能用聲紋模式
        }
        g_voices[name] = v;
    }
    fprintf(stderr, "[t2v] 載入 %zu 個已存聲音\n", g_voices.size());
}

// ------------------------------------------------------------------ 生成
struct GenReq {
    std::string text, lang = "auto", instruct, voice;
    int64_t     seed      = -1;
    int         max_new   = -1;
    int         top_k     = -1;
    float       temp      = NAN;
    float       top_p     = NAN;
    float       rep       = NAN;
    bool        stream    = false;
};

struct SynthCtl {
    uint64_t                                epoch = 0;
    std::atomic<bool> *                     gone  = nullptr;
    std::function<bool(const float *, int)> chunk;
};

static bool cancel_cb(void * u) {
    auto * c = (SynthCtl *) u;
    return g_cancel_epoch.load() != c->epoch || (c->gone && c->gone->load());
}

static bool chunk_cb(const float * s, int n, void * u) {
    auto * c = (SynthCtl *) u;
    return c->chunk(s, n);
}

static int synth(qt_context * q, const GenReq & r, const Voice * v, SynthCtl & ctl, std::vector<float> & out,
                 std::string & err) {
    qt_tts_params p;
    qt_tts_default_params(&p);
    p.text = r.text.c_str();
    p.lang = r.lang.c_str();
    if (!r.instruct.empty()) {
        p.instruct = r.instruct.c_str();
    }
    if (v) {
        p.ref_spk_emb = v->spk.data();
        p.ref_spk_dim = (int) v->spk.size();
        if (!v->codes.empty() && !v->ref_text.empty()) {
            p.ref_codes = v->codes.data();
            p.ref_T     = v->T;
            p.ref_text  = v->ref_text.c_str();
        }
    }
    p.seed = r.seed;
    if (r.max_new > 0) {
        p.max_new_tokens = r.max_new;
    }
    if (r.top_k >= 0) {
        p.top_k = r.top_k;
    }
    if (!std::isnan(r.temp)) {
        p.temperature = r.temp;
    }
    if (!std::isnan(r.top_p)) {
        p.top_p = r.top_p;
    }
    if (!std::isnan(r.rep)) {
        p.repetition_penalty = r.rep;
    }
    p.cancel           = cancel_cb;
    p.cancel_user_data = &ctl;
    if (ctl.chunk) {
        p.on_chunk           = chunk_cb;
        p.on_chunk_user_data = &ctl;
    }
    qt_audio a  = {};
    auto     rc = qt_synthesize(q, &p, &a);
    if (rc == QT_STATUS_OK && a.n_samples > 0) {
        out.insert(out.end(), a.samples, a.samples + a.n_samples);
    }
    qt_audio_free(&a);
    if (rc != QT_STATUS_OK) {
        err = rc == QT_STATUS_CANCELLED ? "已取消" : qt_last_error();
    }
    return (int) rc;
}

static void save_output(const std::string & fname, const std::vector<float> & pcm) {
    if (pcm.empty()) {
        return;
    }
    std::string wav = audio_encode_wav(pcm.data(), (int) pcm.size(), SR, WAV_S16);
    if (!write_file(g_out_dir / u8p(fname), wav.data(), wav.size())) {
        fprintf(stderr, "[t2v] 無法寫入 outputs/%s\n", fname.c_str());
    }
}

static void handle_generate(const httplib::Request & req, httplib::Response & res, bool clone) {
    qt_context * q = clone ? g_base : g_design;
    if (!q) {
        json_error(res, 503, clone ? "Base 模型沒有載入（找不到 GGUF 檔）" : "VoiceDesign 模型沒有載入（找不到 GGUF 檔）");
        return;
    }
    yyjson_doc * doc = yyjson_read(req.body.data(), req.body.size(), 0);
    if (!doc) {
        json_error(res, 400, "JSON 格式錯誤");
        return;
    }
    yyjson_val * o = yyjson_doc_get_root(doc);
    GenReq       r;
    r.text     = trim(jstr(o, "text"));
    r.lang     = jstr(o, "lang", "auto");
    r.instruct = trim(jstr(o, "instruct"));
    r.voice    = jstr(o, "voice");
    r.seed     = (int64_t) jnum(o, "seed", -1);
    r.max_new  = (int) jnum(o, "max_new_tokens", -1);
    r.top_k    = (int) jnum(o, "top_k", -1);
    r.temp     = (float) jnum(o, "temperature", NAN);
    r.top_p    = (float) jnum(o, "top_p", NAN);
    r.rep      = (float) jnum(o, "repetition_penalty", NAN);
    r.stream   = jbool(o, "stream", false);
    yyjson_doc_free(doc);
    if (r.lang.empty()) {
        r.lang = "auto";
    }

    if (r.text.empty()) {
        json_error(res, 400, "請輸入要念的文字");
        return;
    }
    if (!clone && r.instruct.empty()) {
        json_error(res, 400, "請輸入聲音描述");
        return;
    }
    std::shared_ptr<const Voice> voice;
    if (clone) {
        std::lock_guard<std::mutex> lk(g_vmu);
        auto                        it = g_voices.find(r.voice);
        if (it == g_voices.end()) {
            json_error(res, 404, "找不到聲音「" + r.voice + "」");
            return;
        }
        voice = it->second;
    }
    // 伺服器端決定 seed，回傳給前端，方便重現同一個結果
    if (r.seed < 0) {
        std::random_device rd;
        r.seed = (int64_t) (rd() & 0x7fffffff);
    }
    const std::string fname = (clone ? "clone_" + sanitize_name(r.voice) : std::string("design")) + "_" + stamp() + "_" +
                              std::to_string(r.seed) + ".wav";
    res.set_header("X-Seed", std::to_string(r.seed));
    res.set_header("X-File", pct_encode(fname));
    res.set_header("Cache-Control", "no-cache");

    const uint64_t epoch = g_cancel_epoch.load();

    if (!r.stream) {
        SynthCtl           ctl;
        ctl.epoch = epoch;
        std::vector<float> pcm;
        std::string        err;
        auto               t0 = std::chrono::steady_clock::now();
        int                rc = synth(q, r, voice.get(), ctl, pcm, err);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (rc != 0) {
            json_error(res, rc == QT_STATUS_CANCELLED ? 409 : 500, err);
            return;
        }
        save_output(fname, pcm);
        fprintf(stderr, "[t2v] %s：%.1f 秒音訊，耗時 %.1f 秒\n", fname.c_str(), (double) pcm.size() / SR, ms / 1000);
        res.set_header("X-Gen-Ms", std::to_string((long long) ms));
        res.set_content(audio_encode_wav(pcm.data(), (int) pcm.size(), SR, WAV_S16), "audio/wav");
        return;
    }

    // 串流：合成在自己的執行緒，把 s16le 推進佇列；連線執行緒負責送出。
    struct StreamState {
        std::mutex              mu;
        std::condition_variable cv;
        std::string             pending;
        bool                    done = false;
        int                     rc   = 0;
        std::string             err;
        std::atomic<bool>       gone{ false };
        std::thread             th;
    };

    auto st = std::make_shared<StreamState>();
    st->th  = std::thread([st, q, r, voice, epoch, fname]() {
        std::vector<float> all;
        SynthCtl           ctl;
        ctl.epoch = epoch;
        ctl.gone  = &st->gone;
        ctl.chunk = [st, &all](const float * s, int n) -> bool {
            if (st->gone.load()) {
                return false;
            }
            all.insert(all.end(), s, s + n);
            std::string bytes;
            s16_append(bytes, s, n);
            std::lock_guard<std::mutex> lk(st->mu);
            st->pending += bytes;
            st->cv.notify_all();
            return true;
        };
        std::vector<float> unused;
        std::string        err;
        auto               t0 = std::chrono::steady_clock::now();
        int                rc = synth(q, r, voice.get(), ctl, unused, err);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (rc == 0) {
            save_output(fname, all);
            fprintf(stderr, "[t2v] %s：%.1f 秒音訊，耗時 %.1f 秒（串流）\n", fname.c_str(), (double) all.size() / SR, s);
        }
        std::lock_guard<std::mutex> lk(st->mu);
        st->rc   = rc;
        st->err  = err;
        st->done = true;
        st->cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lk(st->mu);
        st->cv.wait(lk, [&] { return st->done || !st->pending.empty(); });
        if (st->pending.empty() && st->rc != 0) {
            int         rc  = st->rc;
            std::string err = st->err;
            lk.unlock();
            st->th.join();
            json_error(res, rc == QT_STATUS_CANCELLED ? 409 : 500, err.empty() ? "生成失敗" : err);
            return;
        }
    }

    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "audio/pcm",
        [st](size_t, httplib::DataSink & sink) -> bool {
            std::string chunk;
            int         rc = 0;
            {
                std::unique_lock<std::mutex> lk(st->mu);
                st->cv.wait(lk, [&] { return st->done || !st->pending.empty(); });
                chunk.swap(st->pending);
                rc = st->rc;
            }
            if (!chunk.empty()) {
                if (!sink.write(chunk.data(), chunk.size())) {
                    st->gone.store(true);
                    return false;
                }
                return true;
            }
            if (rc != 0) {
                return false;
            }
            sink.done();
            return true;
        },
        [st](bool) {
            st->gone.store(true);
            if (st->th.joinable()) {
                st->th.join();
            }
        });
}

// ------------------------------------------------------------------ 聲音 API
static void handle_voice_create(const httplib::Request & req, httplib::Response & res) {
    if (!g_base) {
        json_error(res, 503, "Base 模型沒有載入，無法建立聲音");
        return;
    }
    if (!req.form.has_file("audio")) {
        json_error(res, 400, "缺少參考音檔");
        return;
    }
    const std::string name     = sanitize_name(req.form.get_field("name"));
    const std::string ref_text = trim(req.form.get_field("ref_text"));
    const bool        xvec     = req.form.get_field("xvec") == "1";
    if (name.empty()) {
        json_error(res, 400, "請替這個聲音取名字");
        return;
    }
    if (!xvec && ref_text.empty()) {
        json_error(res, 400, "一般模式需要參考音的逐字稿；沒有逐字稿請勾選「只用聲紋」");
        return;
    }
    const httplib::FormData f   = req.form.get_file("audio");
    int                     T   = 0;
    float *                 pcm = audio_read_mono_buf((const uint8_t *) f.content.data(), f.content.size(), SR, &T);
    if (!pcm) {
        json_error(res, 400, "無法讀取參考音（需要 WAV）");
        return;
    }
    if (T < SR) {
        free(pcm);
        json_error(res, 400, "參考音太短，至少要 1 秒");
        return;
    }
    if (T > SR * 30) {
        free(pcm);
        json_error(res, 400, "參考音太長，請控制在 30 秒內（建議 5–15 秒）");
        return;
    }
    qt_voice_ref ref = {};
    if (qt_extract_voice_ref(g_base, pcm, T, &ref) != QT_STATUS_OK) {
        std::string e = qt_last_error();
        free(pcm);
        json_error(res, 500, "擷取聲音特徵失敗：" + e);
        return;
    }
    auto v = std::make_shared<Voice>();
    v->spk.assign(ref.ref_spk_emb, ref.ref_spk_emb + ref.ref_spk_dim);
    if (!xvec) {
        v->codes.assign(ref.ref_codes, ref.ref_codes + (size_t) ref.num_codebooks * (size_t) ref.ref_T);
        v->T        = ref.ref_T;
        v->ref_text = ref_text;
    }
    qt_voice_ref_free(&ref);

    std::error_code ec;
    const fs::path  d = g_voices_dir;
    bool            ok = write_file(d / u8p(name + ".spk"), v->spk.data(), v->spk.size() * sizeof(float));
    if (!xvec) {
        std::vector<uint8_t> packed = rvq_pack_codes(v->codes, RVQ_BITS);
        ok = ok && write_file(d / u8p(name + ".rvq"), packed.data(), packed.size());
        ok = ok && write_file(d / u8p(name + ".txt"), ref_text.data(), ref_text.size());
    } else {
        fs::remove(d / u8p(name + ".rvq"), ec);
        fs::remove(d / u8p(name + ".txt"), ec);
    }
    std::string wav = audio_encode_wav(pcm, T, SR, WAV_S16);
    free(pcm);
    ok = ok && write_file(d / u8p(name + ".wav"), wav.data(), wav.size());
    if (!ok) {
        json_error(res, 500, "寫入 voices 資料夾失敗");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_vmu);
        g_voices[name] = v;
    }
    fprintf(stderr, "[t2v] 建立聲音「%s」（%s，參考音 %.1f 秒）\n", name.c_str(), xvec ? "只用聲紋" : "ICL",
            T / (double) SR);
    yyjson_mut_doc * doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val * r   = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, r);
    yyjson_mut_obj_add_strcpy(doc, r, "name", name.c_str());
    send_json(res, doc);
}

static void handle_voice_list(const httplib::Request &, httplib::Response & res) {
    yyjson_mut_doc * doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val * arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    std::lock_guard<std::mutex> lk(g_vmu);
    for (const auto & kv : g_voices) {
        yyjson_mut_val * o = yyjson_mut_arr_add_obj(doc, arr);
        yyjson_mut_obj_add_strcpy(doc, o, "name", kv.first.c_str());
        yyjson_mut_obj_add_bool(doc, o, "icl", !kv.second->codes.empty());
        yyjson_mut_obj_add_strcpy(doc, o, "ref_text", kv.second->ref_text.c_str());
        std::error_code ec;
        auto            sz = fs::file_size(g_voices_dir / u8p(kv.first + ".wav"), ec);
        yyjson_mut_obj_add_real(doc, o, "seconds", ec ? 0.0 : (double) (sz > 44 ? sz - 44 : 0) / (2.0 * SR));
    }
    send_json(res, doc);
}

static void handle_voice_delete(const httplib::Request & req, httplib::Response & res) {
    const std::string name = req.get_param_value("name");
    {
        std::lock_guard<std::mutex> lk(g_vmu);
        if (!g_voices.erase(name)) {
            json_error(res, 404, "找不到聲音「" + name + "」");
            return;
        }
    }
    std::error_code ec;
    for (const char * ext : { ".spk", ".rvq", ".txt", ".wav" }) {
        fs::remove(g_voices_dir / u8p(name + ext), ec);
    }
    json_ok(res);
}

static void handle_voice_audio(const httplib::Request & req, httplib::Response & res) {
    const std::string name = req.get_param_value("name");
    {
        std::lock_guard<std::mutex> lk(g_vmu);
        if (!g_voices.count(name)) {
            json_error(res, 404, "找不到聲音");
            return;
        }
    }
    std::string wav;
    if (!read_file(g_voices_dir / u8p(name + ".wav"), wav)) {
        json_error(res, 404, "找不到參考音檔");
        return;
    }
    res.set_content(std::move(wav), "audio/wav");
}

static void handle_info(const httplib::Request &, httplib::Response & res) {
    yyjson_mut_doc * doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val * r   = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, r);
    yyjson_mut_obj_add_strcpy(doc, r, "version", qt_version());
    yyjson_mut_obj_add_bool(doc, r, "design", g_design != nullptr);
    yyjson_mut_obj_add_bool(doc, r, "base", g_base != nullptr);
    yyjson_mut_val * langs = yyjson_mut_arr(doc);
    qt_context *     q     = g_design ? g_design : g_base;
    if (q) {
        for (int i = 0; i < qt_n_languages(q); i++) {
            yyjson_mut_arr_add_strcpy(doc, langs, qt_language_name(q, i));
        }
    }
    yyjson_mut_obj_add_val(doc, r, "languages", langs);
    send_json(res, doc);
}

// ------------------------------------------------------------------ main
static qt_context * load_model(const std::string & talker, const std::string & codec, const char * device, bool fa,
                               const char * label) {
    std::error_code ec;
    if (talker.empty() || !fs::exists(u8p(talker), ec)) {
        fprintf(stderr, "[t2v] 略過 %s：找不到 %s\n", label, talker.c_str());
        return nullptr;
    }
    qt_init_params ip;
    qt_init_default_params(&ip);
    ip.talker_path = talker.c_str();
    ip.codec_path  = codec.c_str();
    ip.use_fa      = fa;
    ip.device      = device;
    fprintf(stderr, "[t2v] 載入 %s：%s\n", label, talker.c_str());
    qt_context * q = qt_init(&ip);
    if (!q) {
        fprintf(stderr, "[t2v] 載入 %s 失敗：%s\n", label, qt_last_error());
    }
    return q;
}

static void print_usage() {
    fprintf(stderr,
            "t2v-server (qwentts.cpp %s)\n"
            "  --design <gguf>   VoiceDesign talker\n"
            "  --base <gguf>     Base talker（複製聲音用）\n"
            "  --codec <gguf>    12Hz tokenizer\n"
            "  --web <dir>       網頁資料夾（預設 web）\n"
            "  --data <dir>      存放 outputs / voices 的資料夾（預設 .）\n"
            "  --host <ip>       預設 127.0.0.1\n"
            "  --port <n>        預設 7860\n"
            "  --device <name>   CUDA0 / Vulkan0 / CPU（預設自動）\n"
            "  --no-fa           關閉 flash attention\n"
            "  --no-browser      不自動開瀏覽器\n",
            QWEN_VERSION);
}

int main(int argc, char ** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    utf8_init(&argc, &argv);
#endif
    std::string  design, base, codec, web = "web", data = ".", host = "127.0.0.1";
    int          port    = 7860;
    const char * device  = nullptr;
    bool         fa      = true;
    bool         browser = true;
    for (int i = 1; i < argc; i++) {
        std::string a    = argv[i];
        auto        next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--design") {
            design = next();
        } else if (a == "--base") {
            base = next();
        } else if (a == "--codec") {
            codec = next();
        } else if (a == "--web") {
            web = next();
        } else if (a == "--data") {
            data = next();
        } else if (a == "--host") {
            host = next();
        } else if (a == "--port") {
            port = std::atoi(next().c_str());
        } else if (a == "--device") {
            device = i + 1 < argc ? argv[++i] : nullptr;
        } else if (a == "--no-fa") {
            fa = false;
        } else if (a == "--no-browser") {
            browser = false;
        } else {
            print_usage();
            return 1;
        }
    }
    if (codec.empty()) {
        print_usage();
        return 1;
    }

    std::error_code ec;
    g_voices_dir = u8p(data) / "voices";
    g_out_dir    = u8p(data) / "outputs";
    fs::create_directories(g_voices_dir, ec);
    fs::create_directories(g_out_dir, ec);

    fprintf(stderr, "[t2v] qwentts.cpp %s\n", qt_version());
    g_design = load_model(design, codec, device, fa, "VoiceDesign");
    g_base   = load_model(base, codec, device, fa, "Base");
    if (!g_design && !g_base) {
        fprintf(stderr, "[t2v] 警告：兩個模型都沒有載入，只能開啟介面。請先執行 build.bat 下載模型。\n");
    }
    load_voices();

    httplib::Server svr;
    svr.set_payload_max_length(64 * 1024 * 1024);
    svr.set_exception_handler([](const httplib::Request &, httplib::Response & res, std::exception_ptr ep) {
        std::string msg = "伺服器內部錯誤";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            msg += std::string("：") + e.what();
        } catch (...) {
        }
        json_error(res, 500, msg);
    });

    svr.Get("/api/info", handle_info);
    svr.Post("/api/design", [](const httplib::Request & q, httplib::Response & s) { handle_generate(q, s, false); });
    svr.Post("/api/clone", [](const httplib::Request & q, httplib::Response & s) { handle_generate(q, s, true); });
    svr.Post("/api/cancel", [](const httplib::Request &, httplib::Response & s) {
        g_cancel_epoch.fetch_add(1);
        json_ok(s);
    });
    svr.Get("/api/voices", handle_voice_list);
    svr.Post("/api/voices", handle_voice_create);
    svr.Delete("/api/voices", handle_voice_delete);
    svr.Get("/api/voice-audio", handle_voice_audio);

    if (!svr.set_mount_point("/", web)) {
        fprintf(stderr, "[t2v] 找不到網頁資料夾：%s\n", web.c_str());
        return 1;
    }

    if (!svr.bind_to_port(host, port)) {
        fprintf(stderr, "[t2v] 無法使用 %s:%d（可能已經有一個 t2v-server 在跑，或連接埠被佔用）\n", host.c_str(), port);
        return 1;
    }
    std::string url = "http://" + (host == "0.0.0.0" ? std::string("127.0.0.1") : host) + ":" + std::to_string(port) + "/";
    fprintf(stderr, "\n[t2v] 準備完成：%s  （關閉這個視窗即可結束）\n\n", url.c_str());
#ifdef _WIN32
    if (browser) {
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
#else
    (void) browser;
#endif
    svr.listen_after_bind();

    if (g_design) {
        qt_free(g_design);
    }
    if (g_base) {
        qt_free(g_base);
    }
    return 0;
}
