// The media engine proper.  Threads: demux (libavformat), video decode,
// audio decode, plus SDL's audio callback; the owner's thread calls the
// public API and poll()/frame().  Anything a thread produces carries a
// "serial": seek() bumps it, so stale packets/frames/samples that were still
// in flight are dropped by whoever finds them (the ffplay trick).

#include "player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace luxium::media {

namespace {
using Clock = std::chrono::steady_clock;
constexpr int    kRate         = 48000;          // audio output rate
constexpr double kAudioRingSec = 0.6;            // decoded audio kept ahead
constexpr size_t kFrameRing    = 5;              // decoded frames kept ahead
constexpr double kReadAheadSec = 12.0;           // demux stops this far ahead
constexpr size_t kMaxQueueBytes = 96u << 20;

double now_sec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

bool debug() {
    static const bool d = std::getenv("LUXIUM_MEDIA_DEBUG") != nullptr;
    return d;
}

bool getenv_off(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

// Packet queue between the demuxer and one decoder.
struct Item {
    AVPacket* pkt = nullptr;
    int       serial = 0;
    bool      flush = false;     // seek marker: flush the codec, drop until `target`
    bool      eof = false;       // end of stream: drain the codec
    double    target = 0;
    double    secs = 0;          // duration of the packet
};

struct PQ {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Item> q;
    size_t bytes = 0;
    double dur = 0;              // seconds of queued packets
    bool   abort = false;

    void clear() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& it : q) av_packet_free(&it.pkt);
        q.clear();
        bytes = 0;
        dur = 0;
    }
    void put(Item it) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (it.pkt) { bytes += it.pkt->size; dur += it.secs; }
            q.push_back(it);
        }
        cv.notify_one();
    }
    bool get(Item& out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return abort || !q.empty(); });
        if (abort) return false;
        out = q.front();
        q.pop_front();
        if (out.pkt) {
            bytes -= out.pkt->size;
            dur -= out.secs;
        }
        return true;
    }
};

struct AChunk {
    std::vector<float> s;        // interleaved stereo
    size_t off = 0;              // frames consumed
    double pts = 0;              // media time of the first frame
    double rate = 1;             // playback rate it was resampled for
};

AVPixelFormat hw_get_format(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    const AVPixelFormat want = *(const AVPixelFormat*)ctx->opaque;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want) return *p;
    return AV_PIX_FMT_NONE;
}

std::string av_err(int e) {
    char b[128];
    av_strerror(e, b, sizeof b);
    return b;
}

bool is_net_error(int e) {
    return e == AVERROR(ECONNREFUSED) || e == AVERROR(ETIMEDOUT) || e == AVERROR(EIO) ||
           e == AVERROR(ECONNRESET) || e == AVERROR(EHOSTUNREACH) || e == AVERROR(ENETUNREACH) ||
           e == AVERROR_HTTP_BAD_REQUEST || e == AVERROR_HTTP_UNAUTHORIZED ||
           e == AVERROR_HTTP_FORBIDDEN || e == AVERROR_HTTP_NOT_FOUND ||
           e == AVERROR_HTTP_OTHER_4XX || e == AVERROR_HTTP_SERVER_ERROR;
}
} // namespace

// ── the engine state ─────────────────────────────────────────────────────────
struct Player::Impl {
    Player& P;
    explicit Impl(Player& p) : P(p) {}

    // lifetime
    std::atomic<bool> abort{false};
    std::thread demux_t, vdec_t, adec_t;
    std::atomic<int> serial{1};          // bumped by seek()/open()
    int demux_serial = 1;                // demux thread only: serial its packets carry
    std::string url, headers;
    bool allow_file = false;

    // container
    AVFormatContext* fmt = nullptr;
    int vi = -1;
    std::atomic<int> ai{-1};
    AVCodecContext *vctx = nullptr, *actx = nullptr;
    AVBufferRef* hwdev = nullptr;
    AVRational vtb{1, 1000}, atb{1, 1000};   // stream time bases, kept so the decoders never touch `fmt`
    AVCodecParameters* vpar = nullptr;       // for the software fallback
    AVIOContext* mem_pb = nullptr;           // playlist served from memory after an HLS seek
    AVPixelFormat hw_pix = AV_PIX_FMT_NONE;
    std::atomic<bool> hw_on{false};
    double start_off = 0;                // container start_time, subtracted from every pts
    std::atomic<double> dur{std::numeric_limits<double>::quiet_NaN()};
    double v_frame_dur = 0.04;
    double sar = 1.0;
    std::string vcodec;
    PQ vpq, apq;

    // streams inside the file (tracks), text subtitle decoding
    struct SubDec {
        AVCodecContext* ctx = nullptr;
        AVRational tb{1, 1000};
        std::vector<SubCue> cues;            // append-only
        std::set<std::string> seen;          // "start|text": a seek re-reads packets
    };
    mutable std::mutex tm;                   // guards the track lists and SubDec::cues
    std::vector<TrackInfo> atracks, stracks;
    std::map<int, SubDec> subs;              // by stream index
    int pending_ai = -1;                     // select_audio(): applied by do_seek (guarded by dm)
    std::mutex swm;                          // an audio stream switch waiting for the audio thread
    AVCodecParameters* swap_par = nullptr;
    AVRational swap_tb{1, 1000};
    std::atomic<uint64_t> a_played{0};       // frames given to the device
    std::atomic<float> a_peak{0};
    std::string acodec, pixfmt;
    int a_channels = 0;
    std::atomic<bool> hw_used{false};        // a hardware frame was actually decoded
    std::atomic<bool> deint{false};

    // demux <-> owner
    std::mutex dm;
    std::condition_variable dcv;         // wakes the demux thread (seek, space freed, abort)
    std::atomic<bool> seek_req{false};
    std::atomic<double> seek_target{0};
    std::atomic<bool> eof{false};        // demuxer read everything
    std::atomic<double> v_end{0}, a_end{0};   // pts end of the last packet queued
    std::atomic<double> buf_start{0};
    std::atomic<bool> reading{false};
    bool logged_pkt = false;

    // decoded video
    std::mutex vm;
    std::condition_variable vcv;
    std::deque<std::shared_ptr<Frame>> vq;
    std::shared_ptr<Frame> cur;
    bool fresh = false;                  // a frame was presented since the last seek
    uint64_t frame_seq = 0;
    std::atomic<bool> v_eof{false}, a_eof{false};
    std::atomic<uint64_t> n_decoded{0}, n_presented{0}, n_dropped{0};
    SwsContext* sws = nullptr;

    // decoded audio
    std::mutex am;
    std::deque<AChunk> aq;
    size_t aq_frames = 0;
    std::condition_variable acv;
    SDL_AudioDeviceID dev = 0;
    int dev_samples = 1024;
    std::atomic<double> a_pts{0};        // media time of the next sample to play
    std::atomic<bool> audio_done{false}; // audio ran out: wall clock takes over
    std::atomic<bool> rate_changed{false};

    // wall clock (video-only media, or after the audio ended)
    std::mutex cm;
    double wall_media = 0;
    double wall_last = 0;

    // events
    std::mutex em;
    std::vector<Event> events;
    std::string err_msg;

    // poll() bookkeeping
    bool loaded_meta = false, loaded_data = false, waiting = false, play_pending = false;
    double last_timeupdate = 0, last_progress = 0, last_buf_end = -1, last_dur = -1;
    int last_vw = 0, last_vh = 0;
    bool seeked_pending = false;
    std::atomic<bool> data_ok{false};                // first frame/audio of this load or seek is in
    std::string err_sent;

    void emit(const char* type, const std::string& detail = {}) {
        std::lock_guard<std::mutex> lk(em);
        events.push_back({type, detail});
    }
    void fail(int code, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(em);
            if (P.err_code_ != 0) return;
            err_msg = msg;
        }
        P.err_code_ = code;
        P.net_state_ = code == 4 ? 3 : 1;
        emit("error", msg);
    }

    double to_sec(int64_t ts, AVRational tb) const {
        return ts == AV_NOPTS_VALUE ? std::numeric_limits<double>::quiet_NaN()
                                    : ts * av_q2d(tb) - start_off;
    }

    // ── clock ────────────────────────────────────────────────────────────────
    bool audio_clock() const { return dev && !audio_done; }
    bool wall_running() const {
        return !P.paused_ && !P.ended_ && !waiting && !P.seeking_ && !P.err_code_;
    }
    void wall_sync() {                   // call with cm held
        const double t = now_sec();
        if (wall_running()) wall_media += (t - wall_last) * P.rate_;
        wall_last = t;
    }
    double clock() {
        if (audio_clock()) {
            const double lat = (double)dev_samples / kRate * P.rate_;
            return std::max(0.0, a_pts.load() - lat);
        }
        std::lock_guard<std::mutex> lk(cm);
        wall_sync();
        return wall_media;
    }
    void set_clock(double t) {
        a_pts = t;
        std::lock_guard<std::mutex> lk(cm);
        wall_media = t;
        wall_last = now_sec();
    }
    // The wall clock must integrate with the OLD running state before it flips.
    void clock_barrier() { std::lock_guard<std::mutex> lk(cm); wall_sync(); }

    // ── SDL audio callback ───────────────────────────────────────────────────
    static void audio_cb(void* ud, Uint8* stream, int len) {
        auto* d = (Impl*)ud;
        float* out = (float*)stream;
        const size_t want = len / (2 * sizeof(float));
        size_t done = 0;
        const float vol = d->P.muted_ ? 0.f : (float)d->P.volume_.load();
        // Hold the sound until the picture of this load/seek is ready, so A/V start together.
        const bool hold = d->P.seeking_ || (d->P.has_video_ && !d->data_ok);
        if (!hold) {
            std::lock_guard<std::mutex> lk(d->am);
            while (done < want && !d->aq.empty()) {
                AChunk& c = d->aq.front();
                if (c.off == 0) d->a_pts = c.pts;
                const size_t have = c.s.size() / 2 - c.off;
                const size_t n = std::min(have, want - done);
                const float* src = c.s.data() + c.off * 2;
                float pk = 0;
                for (size_t i = 0; i < n * 2; ++i) { out[done * 2 + i] = src[i] * vol; pk = std::max(pk, std::fabs(src[i])); }
                if (pk > d->a_peak) d->a_peak = pk;
                d->a_played += n;
                c.off += n;
                done += n;
                d->aq_frames -= n;
                d->a_pts = d->a_pts.load() + (double)n * c.rate / kRate;
                if (c.off * 2 >= c.s.size()) d->aq.pop_front();
            }
            if (done == 0 && d->a_eof && d->aq.empty()) {
                // Audio ended before the video: keep the clock going on the wall.
                std::lock_guard<std::mutex> ck(d->cm);
                d->wall_media = d->a_pts;
                d->wall_last = now_sec();
                d->audio_done = true;
            }
        }
        if (done < want) std::memset(out + done * 2, 0, (want - done) * 2 * sizeof(float));
        d->acv.notify_one();
    }

    // ── open / demux thread ──────────────────────────────────────────────────
    static int interrupt_cb(void* o) {
        auto* d = (Impl*)o;
        return d->abort || d->seek_req;
    }

    // VA-API only decodes 4:2:0 at 8 bit (10 bit for HEVC/VP9/AV1); Hi10P H.264, 4:2:2/4:4:4 and
    // 12 bit go straight to the (threaded) software decoder instead of failing 30 packets first.
    static bool hw_plausible(const AVCodecParameters* par) {
        // MPEG-2/MPEG-4 ASP/VC-1 advertise VA-API but drivers often lack them: ~30 failed packets, then software.
        // They are cheap in software anyway.
        if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC && par->codec_id != AV_CODEC_ID_VP9 &&
            par->codec_id != AV_CODEC_ID_AV1) return false;
        const AVPixFmtDescriptor* pd = par->format >= 0 ? av_pix_fmt_desc_get((AVPixelFormat)par->format) : nullptr;
        if (!pd) return true;
        if (pd->log2_chroma_w != 1 || pd->log2_chroma_h != 1) return false;
        const int depth = pd->comp[0].depth;
        if (depth > 10) return false;
        return depth <= 8 || par->codec_id == AV_CODEC_ID_HEVC || par->codec_id == AV_CODEC_ID_VP9 || par->codec_id == AV_CODEC_ID_AV1;
    }

    bool open_codec(AVCodecContext*& ctx, AVCodecParameters* par, AVRational tb, bool try_hw) {
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) return false;
        if (try_hw && !hw_plausible(par)) try_hw = false;
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, par);
        ctx->pkt_timebase = tb;
        ctx->thread_count = 0;           // let libavcodec pick
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (try_hw && codec->type == AVMEDIA_TYPE_VIDEO && !getenv_off("LUXIUM_MEDIA_HW")) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
                if (!cfg) break;
                if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI &&
                    (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    hw_pix = cfg->pix_fmt;
                    break;
                }
            }
            if (hw_pix != AV_PIX_FMT_NONE) {
                if (!hwdev && av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
                    hwdev = nullptr;
                if (hwdev) {
                    ctx->hw_device_ctx = av_buffer_ref(hwdev);
                    ctx->opaque = &hw_pix;
                    ctx->get_format = hw_get_format;
                    ctx->thread_count = 1;
                    hw_on = true;
                }
            }
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            if (hw_on) {                 // hardware refused this profile: software
                hw_on = false;
                return open_codec(ctx, par, tb, false);
            }
            return false;
        }
        return true;
    }

    AVDictionary* make_opts() {
        AVDictionary* o = nullptr;
        if (!headers.empty()) av_dict_set(&o, "headers", headers.c_str(), 0);
        av_dict_set(&o, "reconnect", "1", 0);
        av_dict_set(&o, "reconnect_streamed", "1", 0);
        av_dict_set(&o, "reconnect_delay_max", "4", 0);
        av_dict_set(&o, "multiple_requests", "1", 0);
        av_dict_set(&o, "allowed_extensions", "ALL", 0);
        av_dict_set(&o, "probesize", "4000000", 0);
        av_dict_set(&o, "analyzeduration", "4000000", 0);
        av_dict_set(&o, "protocol_whitelist",
                    allow_file ? "file,http,https,tcp,tls,crypto" : "http,https,tcp,tls,crypto", 0);
        return o;
    }

    // ── HLS seeking by reopening ─────────────────────────────────────────────
    // FFmpeg 6.1's hls demuxer cannot seek into fMP4 segments it has not read
    // yet ("partial file": the mov demuxer keeps fragment tables from before the
    // seek).  So a seek in a media playlist rewrites the playlist to start at the
    // segment holding the target and opens a fresh demuxer on it; the decoders
    // then drop frames up to the exact target like after any other seek.
    // The rewritten playlist comes from memory, so the segment requests of the
    // new demuxer carry no custom headers (Cookie/Referer): shortcut, see MEDIA.md.
    static int mem_read(void* o, uint8_t* buf, int n) {
        auto* s = (std::pair<std::string, size_t>*)o;
        const size_t left = s->first.size() - s->second;
        if (!left) return AVERROR_EOF;
        const size_t k = std::min<size_t>(left, n);
        std::memcpy(buf, s->first.data() + s->second, k);
        s->second += k;
        return (int)k;
    }
    std::pair<std::string, size_t> mem_data;
    double dur_base = 0;                     // the reopened playlist starts this far into the media
    void free_mem_pb() {
        if (mem_pb) { av_freep(&mem_pb->buffer); avio_context_free(&mem_pb); }
    }

    std::string fetch_playlist() {
        AVIOContext* io = nullptr;
        AVDictionary* o = make_opts();
        AVIOInterruptCB icb{&Impl::interrupt_cb, this};
        const int r = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, &icb, &o);
        av_dict_free(&o);
        if (r < 0) return {};
        std::string text;
        char buf[8192];
        for (int n; (n = avio_read(io, (unsigned char*)buf, sizeof buf)) > 0 && text.size() < (8u << 20);) text.append(buf, n);
        avio_closep(&io);
        return text;
    }

    bool reopen_hls_at(double t) {
        std::string text = fetch_playlist();
        if (text.rfind("#EXTM3U", 0) != 0 || text.find("#EXT-X-STREAM-INF") != std::string::npos) return false;
        std::vector<std::string> lines;
        for (size_t i = 0; i < text.size();) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string l = text.substr(i, j - i);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            i = j + 1;
        }
        // walk the segments: [index of its EXTINF line, start time]
        double at = 0, dur = 0;
        long seq0 = 0;
        int first_seg_line = -1, pick = -1, seg_no = 0;
        std::vector<std::pair<int, double>> segs;   // line of the URI, start time
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            if (l.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) seq0 = std::atol(l.c_str() + 22);
            else if (l.rfind("#EXTINF:", 0) == 0) dur = std::atof(l.c_str() + 8);
            else if (!l.empty() && l[0] != '#') {
                if (first_seg_line < 0) first_seg_line = (int)i;
                segs.push_back({(int)i, at});
                if (at <= t + 0.001) pick = seg_no;
                at += dur;
                ++seg_no;
            }
        }
        if (pick < 0 || segs.empty()) return false;
        // header tags (everything before the first segment except what we replace) + the tail from `pick`
        std::string out;
        for (int i = 0; i < first_seg_line; ++i) {
            const std::string& l = lines[i];
            if (l.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0 || l.rfind("#EXTINF:", 0) == 0 ||
                l.rfind("#EXT-X-DISCONTINUITY", 0) == 0 || l.rfind("#EXT-X-PROGRAM-DATE-TIME", 0) == 0) continue;
            out += l + "\n";
        }
        out += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(seq0 + pick) + "\n";
        // the EXTINF line of segment `pick` is the line before its URI (tags like BYTERANGE may sit between)
        int from = segs[pick].first;
        while (from > 0 && lines[from - 1].rfind("#EXTINF:", 0) != 0 && lines[from - 1][0] == '#') --from;
        if (from > 0 && lines[from - 1].rfind("#EXTINF:", 0) == 0) --from;
        for (size_t i = from; i < lines.size(); ++i) out += lines[i] + "\n";

        AVFormatContext* nf = avformat_alloc_context();
        nf->interrupt_callback = {&Impl::interrupt_cb, this};
        AVIOContext* old_pb = mem_pb;
        mem_data = {out, 0};
        auto* buf = (unsigned char*)av_malloc(32768);
        mem_pb = avio_alloc_context(buf, 32768, 0, &mem_data, &Impl::mem_read, nullptr, nullptr);
        nf->pb = mem_pb;
        nf->flags |= AVFMT_FLAG_CUSTOM_IO;
        AVDictionary* o = make_opts();
        int r = avformat_open_input(&nf, url.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (r >= 0) r = avformat_find_stream_info(nf, nullptr);
        if (r < 0 || (int)nf->nb_streams <= std::max(vi, ai.load())) {
            if (nf) avformat_close_input(&nf);
            free_mem_pb();
            mem_pb = old_pb;
            return false;
        }
        for (unsigned i = 0; i < nf->nb_streams; ++i) nf->streams[i]->discard = AVDISCARD_ALL;
        if (vi >= 0) nf->streams[vi]->discard = AVDISCARD_DEFAULT;
        if (ai >= 0) nf->streams[ai.load()]->discard = AVDISCARD_DEFAULT;
        dur_base = segs[pick].second;
        AVFormatContext* prev = fmt;
        fmt = nf;
        avformat_close_input(&prev);
        if (old_pb) { av_freep(&old_pb->buffer); avio_context_free(&old_pb); }
        return true;
    }

    // ── tracks ───────────────────────────────────────────────────────────────
    void list_tracks() {
        std::lock_guard<std::mutex> lk(tm);
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            const AVCodecParameters* cp = st->codecpar;
            if (cp->codec_type != AVMEDIA_TYPE_AUDIO && cp->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
            TrackInfo t;
            t.index = (int)i;
            t.codec = avcodec_get_name(cp->codec_id);
            if (AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) t.lang = e->value;
            if (AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0)) t.title = e->value;
            t.is_default = st->disposition & AV_DISPOSITION_DEFAULT;
            if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
                t.channels = cp->ch_layout.nb_channels;
                t.usable = avcodec_find_decoder(cp->codec_id) != nullptr;
                atracks.push_back(t);
            } else {
                const AVCodecDescriptor* cd = avcodec_descriptor_get(cp->codec_id);
                // HLS subtitle renditions are separate playlists: opening them would cost requests for nothing
                t.usable = cd && (cd->props & AV_CODEC_PROP_TEXT_SUB) && avcodec_find_decoder(cp->codec_id) &&
                           !std::strstr(fmt->iformat->name, "hls");
                stracks.push_back(t);
            }
        }
    }

    void open_sub_decoders() {
        for (auto& t : stracks) {
            if (!t.usable) continue;
            AVStream* st = fmt->streams[t.index];
            SubDec sd;
            sd.tb = st->time_base;
            if (!open_codec(sd.ctx, st->codecpar, sd.tb, false)) { t.usable = false; continue; }
            st->discard = AVDISCARD_DEFAULT;
            subs.emplace(t.index, std::move(sd));
        }
    }

    // ASS "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (what every FFmpeg
    // text-subtitle decoder emits) -> WebVTT cue markup: line breaks and i/b/u kept, the rest dropped.
    static bool ass_to_cue(const char* ass, std::string& out) {
        std::string l = ass;
        size_t p = 0;
        for (int i = 0; i < 8; ++i) { p = l.find(',', p); if (p == std::string::npos) { p = 0; break; } ++p; }
        l = l.substr(p);
        std::string o;
        bool tag[3] = {false, false, false};
        static const char kTag[] = {'i', 'b', 'u'};
        for (size_t i = 0; i < l.size(); ++i) {
            const char c = l[i];
            if (c == '{') {
                const size_t e = l.find('}', i);
                if (e == std::string::npos) break;
                const std::string ov = l.substr(i + 1, e - i - 1);
                if (ov.find("\\p1") != std::string::npos) return false;   // vector drawing, no text
                for (int k = 0; k < 3; ++k) {
                    const std::string on = std::string("\\") + kTag[k] + "1", off = std::string("\\") + kTag[k] + "0";
                    if (ov.find(on) != std::string::npos && !tag[k]) { o += std::string("<") + kTag[k] + ">"; tag[k] = true; }
                    if (ov.find(off) != std::string::npos && tag[k]) { o += std::string("</") + kTag[k] + ">"; tag[k] = false; }
                }
                i = e;
            } else if (c == '\\' && i + 1 < l.size() && (l[i + 1] == 'N' || l[i + 1] == 'n')) { o += '\n'; ++i; }
            else if (c == '\\' && i + 1 < l.size() && l[i + 1] == 'h') { o += ' '; ++i; }
            else if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else o += c;
        }
        for (int k = 2; k >= 0; --k) if (tag[k]) o += std::string("</") + kTag[k] + ">";
        while (!o.empty() && (o.back() == '\n' || o.back() == ' ')) o.pop_back();
        out = o;
        return !out.empty();
    }

    void decode_sub(SubDec& sd, const AVPacket* pkt) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(sd.ctx, &sub, &got, const_cast<AVPacket*>(pkt)) < 0 || !got) return;
        const double t0 = to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, sd.tb);
        double len = pkt->duration > 0 ? pkt->duration * av_q2d(sd.tb) : (sub.end_display_time - sub.start_display_time) / 1000.0;
        if (!(len > 0) || len > 600) len = 3;
        if (!std::isnan(t0)) {
            const double start = t0 + sub.start_display_time / 1000.0;
            for (unsigned i = 0; i < sub.num_rects; ++i) {
                std::string text;
                if (sub.rects[i]->type != SUBTITLE_ASS || !sub.rects[i]->ass || !ass_to_cue(sub.rects[i]->ass, text)) continue;
                char key[48];
                std::snprintf(key, sizeof key, "%.0f|", start * 1000);
                std::lock_guard<std::mutex> lk(tm);
                if (sd.seen.insert(std::string(key) + text).second) sd.cues.push_back({start, start + len, text});
            }
        }
        avsubtitle_free(&sub);
    }

    void demux_main() {
        fmt = avformat_alloc_context();
        fmt->interrupt_callback = {&Impl::interrupt_cb, this};
        AVDictionary* o = make_opts();
        int r = avformat_open_input(&fmt, url.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (r < 0) {
            if (!abort) fail(is_net_error(r) ? 2 : 4, "open: " + av_err(r));
            return;
        }
        if ((r = avformat_find_stream_info(fmt, nullptr)) < 0) {
            if (!abort) fail(4, "probe: " + av_err(r));
            return;
        }
        start_off = fmt->start_time == AV_NOPTS_VALUE ? 0 : fmt->start_time / (double)AV_TIME_BASE;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) fmt->streams[i]->discard = AVDISCARD_ALL;
        vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi >= 0 && (fmt->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC)) vi = -1;
        list_tracks();
        if (vi < 0 && atracks.empty()) { fail(4, "no playable streams"); return; }

        if (vi >= 0) {
            AVStream* st = fmt->streams[vi];
            vtb = st->time_base;
            vpar = avcodec_parameters_alloc();
            avcodec_parameters_copy(vpar, st->codecpar);
            if (!open_codec(vctx, st->codecpar, vtb, true)) { fail(4, "no decoder for the video stream"); return; }
            st->discard = AVDISCARD_DEFAULT;
            const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            if (fr.num && fr.den) v_frame_dur = 1.0 / av_q2d(fr);
            AVRational s = av_guess_sample_aspect_ratio(fmt, st, nullptr);
            sar = (s.num > 0 && s.den > 0) ? av_q2d(s) : 1.0;
            vcodec = avcodec_get_name(st->codecpar->codec_id);
            pixfmt = av_get_pix_fmt_name((AVPixelFormat)st->codecpar->format) ? av_get_pix_fmt_name((AVPixelFormat)st->codecpar->format) : "";
            P.vw_ = (int)std::lround(st->codecpar->width * sar);
            P.vh_ = st->codecpar->height;
            P.has_video_ = true;
        }
        // The default-flagged audio stream first, then file order; the first one that opens plays.
        std::vector<int> order;
        for (auto& t : atracks) if (t.usable && t.is_default) order.push_back(t.index);
        for (auto& t : atracks) if (t.usable && !t.is_default) order.push_back(t.index);
        for (int idx : order) {
            AVStream* st = fmt->streams[idx];
            atb = st->time_base;
            if (!open_codec(actx, st->codecpar, atb, false)) continue;
            ai = idx;
            st->discard = AVDISCARD_DEFAULT;
            acodec = avcodec_get_name(st->codecpar->codec_id);
            a_channels = st->codecpar->ch_layout.nb_channels;
            P.has_audio_ = true;
            open_audio_device();
            break;
        }
        if (ai < 0 && vi < 0) { fail(4, "no decoder for the audio stream"); return; }
        {
            std::lock_guard<std::mutex> lk(tm);
            for (auto& t : atracks) t.selected = t.index == ai;
        }
        open_sub_decoders();
        if (fmt->duration != AV_NOPTS_VALUE) dur = fmt->duration / (double)AV_TIME_BASE;
        else dur = std::numeric_limits<double>::infinity();
        P.ready_ = 1;
        P.net_state_ = 2;
        emit("durationchange");
        emit("loadedmetadata");
        double init = 0;
        {
            std::lock_guard<std::mutex> lk(dm);
            if (seek_req) init = seek_target;   // seek() before metadata
        }
        if (init <= 0) set_clock(0);
        demux_serial = serial;
        if (vi >= 0) vdec_t = std::thread([this] { video_main(); });
        if (ai >= 0) adec_t = std::thread([this] { audio_main(); });
        if (dev && !P.paused_) SDL_PauseAudioDevice(dev, 0);
        read_loop();
    }

    void open_audio_device() {
        static std::mutex m;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        }
        SDL_AudioSpec want{}, have{};
        want.freq = kRate;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = (Uint16)dev_samples;
        want.callback = &Impl::audio_cb;
        want.userdata = this;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dev) dev_samples = have.samples;   // else: silent playback on the wall clock
    }

    bool queue_full() {
        const size_t b = vpq.bytes + apq.bytes;
        if (b > kMaxQueueBytes) return true;
        const bool v_ok = vi < 0 || vpq.dur >= kReadAheadSec;
        const bool a_ok = ai < 0 || apq.dur >= kReadAheadSec;
        return v_ok && a_ok;
    }

    void read_loop() {
        int errors = 0;
        while (!abort) {
            if (seek_req) do_seek();
            if (eof || queue_full()) {
                reading = false;
                if (P.net_state_ == 2) P.net_state_ = 1;
                std::unique_lock<std::mutex> lk(dm);
                dcv.wait_for(lk, std::chrono::milliseconds(eof ? 100 : 20),
                             [&] { return abort || seek_req; });
                refresh_duration();
                continue;
            }
            reading = true;
            if (P.net_state_ == 1) P.net_state_ = 2;
            AVPacket* pkt = av_packet_alloc();
            const int r = av_read_frame(fmt, pkt);
            if (r < 0) {
                av_packet_free(&pkt);
                if (abort) break;
                if (debug()) std::fprintf(stderr, "[media] read: %s\n", av_err(r).c_str());
                if (seek_req) continue;
                if (r == AVERROR_EOF || (fmt->pb && avio_feof(fmt->pb))) {
                    Item e;
                    e.eof = true;
                    e.serial = demux_serial;
                    if (vi >= 0) vpq.put(e);
                    if (ai >= 0) apq.put(e);
                    eof = true;
                    refresh_duration();
                    continue;
                }
                if (++errors > 3) {
                    fail(is_net_error(r) ? 2 : 3, "read: " + av_err(r));
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            errors = 0;
            const int si = pkt->stream_index;
            if (si != vi && si != ai) {
                auto sd = subs.find(si);
                if (sd != subs.end()) decode_sub(sd->second, pkt);
                av_packet_free(&pkt);
                continue;
            }
            AVStream* st = fmt->streams[si];
            const double secs = pkt->duration > 0 ? pkt->duration * av_q2d(st->time_base) : 0;
            double pts = to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, st->time_base);
            if (debug() && !logged_pkt) { logged_pkt = true; std::fprintf(stderr, "[media] first packet after open/seek: stream %d pts=%.3f\n", si, pts); }
            if (!std::isnan(pts)) (si == vi ? v_end : a_end) = pts + secs;
            Item it;
            it.pkt = pkt;
            it.serial = demux_serial;
            it.secs = secs;
            (si == vi ? vpq : apq).put(it);
        }
        vpq.abort = apq.abort = true;
        vpq.cv.notify_all();
        apq.cv.notify_all();
    }

    void refresh_duration() {
        if (!fmt || fmt->duration == AV_NOPTS_VALUE) return;
        if (fmt->duration != AV_NOPTS_VALUE) {
            const double d = fmt->duration / (double)AV_TIME_BASE + dur_base;
            if (std::fabs(d - dur.load()) > 0.01 || std::isinf(dur.load())) dur = d;
        }
    }

    void do_seek() {
        double t;
        int new_ai = -1;
        {
            std::lock_guard<std::mutex> lk(dm);
            t = seek_target;
            seek_req = false;
            eof = false;                 // poll() trusts eof only while no seek is pending
            new_ai = pending_ai;
            pending_ai = -1;
        }
        if (new_ai >= 0 && new_ai != ai && new_ai < (int)fmt->nb_streams) {
            // Audio stream switch: the audio thread reopens its decoder when it meets the flush below.
            fmt->streams[ai]->discard = AVDISCARD_ALL;
            fmt->streams[new_ai]->discard = AVDISCARD_DEFAULT;
            {
                std::lock_guard<std::mutex> lk(swm);
                if (swap_par) avcodec_parameters_free(&swap_par);
                swap_par = avcodec_parameters_alloc();
                avcodec_parameters_copy(swap_par, fmt->streams[new_ai]->codecpar);
                swap_tb = fmt->streams[new_ai]->time_base;
            }
            ai = new_ai;
            std::lock_guard<std::mutex> lk(tm);
            for (auto& tr : atracks) tr.selected = tr.index == new_ai;
        }
        const double d = dur;
        if (std::isfinite(d)) t = std::min(t, d);
        t = std::max(0.0, t);
        int r;
        if (std::strstr(fmt->iformat->name, "hls") && t > 0.5 && reopen_hls_at(t)) {
            r = 0;
        } else {
            const int64_t ts = (int64_t)((t + start_off) * AV_TIME_BASE);
            dur_base = 0;
            r = avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0);
            if (r < 0) r = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        }
        if (debug()) std::fprintf(stderr, "[media] seek to %.3f -> %d (%s)\n", t, r, r < 0 ? av_err(r).c_str() : "ok");
        vpq.clear();
        apq.clear();
        demux_serial = serial;
        Item f;
        f.flush = true;
        f.serial = demux_serial;
        f.target = t;
        if (vi >= 0) vpq.put(f);
        if (ai >= 0) apq.put(f);
        v_end = a_end = t;
        buf_start = t;
        logged_pkt = false;
    }

    // ── video decode ─────────────────────────────────────────────────────────
    void video_main() {
        double discard_until = 0;
        int cur_serial = serial.load();
        int bad = 0;
        bool got_any = false;
        AVFrame* fr = av_frame_alloc();
        AVFrame* sw = av_frame_alloc();
        auto drain = [&]() {
            for (;;) {
                const int r = avcodec_receive_frame(vctx, fr);
                if (r < 0) {
                    if (r != AVERROR(EAGAIN) && r != AVERROR_EOF && ++bad > 30 && !got_any) {
                        if (hw_on) switch_to_software(); else fail(3, "video decode: " + av_err(r));
                    }
                    return;
                }
                bad = 0;
                got_any = true;
                AVFrame* src = fr;
                if (fr->format == hw_pix && hw_on) {
                    hw_used = true;
                    av_frame_unref(sw);
                    if (av_hwframe_transfer_data(sw, fr, 0) < 0) { av_frame_unref(fr); continue; }
                    sw->pts = fr->pts;
                    sw->best_effort_timestamp = fr->best_effort_timestamp;
                    sw->color_range = fr->color_range;
                    sw->colorspace = fr->colorspace;
                    src = sw;
                }
                const double pts = to_sec(src->best_effort_timestamp, vtb);
                const bool skip = cur_serial != serial.load() ||
                                  (!std::isnan(pts) && pts + v_frame_dur <= discard_until + 1e-6);
                if (!skip) {
                    auto f = convert(src, std::isnan(pts) ? discard_until : pts);
                    n_decoded++;
                    std::unique_lock<std::mutex> lk(vm);
                    vcv.wait(lk, [&] { return abort || vq.size() < kFrameRing || cur_serial != serial.load(); });
                    if (!abort && cur_serial == serial.load()) vq.push_back(std::move(f));
                }
                av_frame_unref(fr);
                if (src == sw) av_frame_unref(sw);
            }
        };
        Item it;
        while (vpq.get(it)) {
            if (it.flush) {
                avcodec_flush_buffers(vctx);
                discard_until = it.target;
                cur_serial = it.serial;
                v_eof = false;
                continue;
            }
            if (it.eof) {
                if (it.serial == serial.load()) {
                    avcodec_send_packet(vctx, nullptr);
                    drain();
                    avcodec_flush_buffers(vctx);
                    v_eof = true;
                }
                continue;
            }
            if (it.serial != serial.load()) { av_packet_free(&it.pkt); continue; }
            const int r = avcodec_send_packet(vctx, it.pkt);
            av_packet_free(&it.pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) {
                if (++bad > 30 && !got_any && hw_on) switch_to_software();
                else if (bad > 200) fail(3, "video decode: " + av_err(r));
            }
            drain();
        }
        av_frame_free(&fr);
        av_frame_free(&sw);
    }

    // The VA-API decoder choked: reopen in software and replay from the clock.
    void switch_to_software() {
        hw_on = false;
        AVCodecContext* n = nullptr;
        if (open_codec(n, vpar, vtb, false)) {
            AVCodecContext* old = vctx;
            vctx = n;
            avcodec_free_context(&old);
            std::lock_guard<std::mutex> lk(dm);
            seek_target = clock();
            seek_req = true;
            serial++;
            dcv.notify_all();
        } else fail(3, "video decode: hardware decoder failed");
    }

    std::shared_ptr<Frame> convert(AVFrame* s, double pts) {
        auto f = std::make_shared<Frame>();
        f->w = s->width;
        f->h = s->height;
        f->display_w = (int)std::lround(s->width * sar);
        f->pts = pts;
        const int cw = (f->w + 1) / 2 * 2, ch = (f->h + 1) / 2;
        f->data.resize((size_t)f->w * f->h + (size_t)cw * ch);
        AVPixelFormat pf = (AVPixelFormat)s->format;
        bool full = s->color_range == AVCOL_RANGE_JPEG;
        if (pf == AV_PIX_FMT_YUVJ420P) { pf = AV_PIX_FMT_YUV420P; full = true; }
        else if (pf == AV_PIX_FMT_YUVJ422P) { pf = AV_PIX_FMT_YUV422P; full = true; }
        else if (pf == AV_PIX_FMT_YUVJ444P) { pf = AV_PIX_FMT_YUV444P; full = true; }
        f->full_range = full;
        switch (s->colorspace) {
        case AVCOL_SPC_BT709: f->matrix = 1; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_SMPTE240M: f->matrix = 0; break;
        case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: f->matrix = 2; break;
        default: f->matrix = s->height >= 720 ? 1 : 0;
        }
        const bool interlaced = s->interlaced_frame != 0;
        uint8_t* dst[2] = {f->data.data(), f->data.data() + (size_t)f->w * f->h};
        if (pf == AV_PIX_FMT_NV12) {
            for (int y = 0; y < f->h; ++y) std::memcpy(dst[0] + (size_t)y * f->w, s->data[0] + (size_t)y * s->linesize[0], f->w);
            for (int y = 0; y < ch; ++y) std::memcpy(dst[1] + (size_t)y * cw, s->data[1] + (size_t)y * s->linesize[1], cw);
        } else {
            sws = sws_getCachedContext(sws, s->width, s->height, pf, s->width, s->height,
                                       AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (sws) {
                av_opt_set_int(sws, "threads", 4, 0);
                int dstride[2] = {f->w, cw};
                sws_scale(sws, s->data, s->linesize, 0, s->height, dst, dstride);
            }
        }
        if (interlaced) {                // shortcut: line-doubling of one field, no motion adaptation
            deint = true;
            auto bob = [](uint8_t* p, int rows, int stride) {
                for (int y = 1; y < rows; y += 2) {
                    const uint8_t* a = p + (size_t)(y - 1) * stride;
                    const uint8_t* b = y + 1 < rows ? p + (size_t)(y + 1) * stride : a;
                    uint8_t* o = p + (size_t)y * stride;
                    for (int x = 0; x < stride; ++x) o[x] = (uint8_t)((a[x] + b[x] + 1) >> 1);
                }
            };
            bob(dst[0], f->h, f->w);
            bob(dst[1], ch, cw);
        }
        return f;
    }

    // ── audio decode ─────────────────────────────────────────────────────────
    void audio_main() {
        SwrContext* swr = nullptr;
        double swr_rate = 0;
        double discard_until = 0;
        int cur_serial = serial.load();
        double next_pts = 0;
        AVFrame* fr = av_frame_alloc();
        auto make_swr = [&]() {
            swr_free(&swr);
            swr_rate = P.rate_;
            AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
            AVChannelLayout in = actx->ch_layout;
            if (in.nb_channels == 0) av_channel_layout_default(&in, 2);
            swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, kRate, &in, actx->sample_fmt,
                                (int)std::lround(actx->sample_rate * swr_rate), 0, nullptr);
            av_opt_set_double(swr, "rematrix_maxval", 1.0, 0);   // downmix without clipping
            if (swr_init(swr) < 0) swr_free(&swr);
        };
        auto drain = [&]() {
            while (avcodec_receive_frame(actx, fr) >= 0) {
                if (cur_serial != serial.load()) { av_frame_unref(fr); continue; }
                if (!swr || swr_rate != P.rate_) make_swr();
                if (!swr) { av_frame_unref(fr); continue; }
                double pts = to_sec(fr->best_effort_timestamp, atb);
                if (std::isnan(pts)) pts = next_pts;
                const double media_len = (double)fr->nb_samples / fr->sample_rate;
                next_pts = pts + media_len;
                const int max_out = (int)(swr_get_out_samples(swr, fr->nb_samples)) + 32;
                AChunk c;
                c.s.resize((size_t)max_out * 2);
                uint8_t* o[1] = {(uint8_t*)c.s.data()};
                const int n = swr_convert(swr, o, max_out, (const uint8_t**)fr->extended_data, fr->nb_samples);
                av_frame_unref(fr);
                if (n <= 0) continue;
                c.s.resize((size_t)n * 2);
                c.pts = pts;
                c.rate = swr_rate;
                if (pts < discard_until) {   // trim what lies before the seek target
                    const size_t skip = (size_t)std::max(0.0, (discard_until - pts) * kRate / swr_rate);
                    if (skip >= (size_t)n) continue;
                    c.s.erase(c.s.begin(), c.s.begin() + skip * 2);
                    c.pts = discard_until;
                }
                std::unique_lock<std::mutex> lk(am);
                acv.wait(lk, [&] { return abort || cur_serial != serial.load() ||
                                          (double)aq_frames / kRate < kAudioRingSec; });
                if (abort || cur_serial != serial.load()) continue;
                aq_frames += c.s.size() / 2;
                aq.push_back(std::move(c));
            }
        };
        Item it;
        while (apq.get(it)) {
            if (it.flush) {
                AVCodecParameters* np = nullptr;
                AVRational ntb{1, 1000};
                {
                    std::lock_guard<std::mutex> lk(swm);
                    np = swap_par;
                    swap_par = nullptr;
                    ntb = swap_tb;
                }
                if (np) {                // audio stream switch
                    if (actx) avcodec_free_context(&actx);
                    atb = ntb;
                    open_codec(actx, np, ntb, false);
                    avcodec_parameters_free(&np);
                    if (actx) { acodec = avcodec_get_name(actx->codec_id); a_channels = actx->ch_layout.nb_channels; }
                    swr_free(&swr);
                } else if (actx) avcodec_flush_buffers(actx);
                swr_free(&swr);
                discard_until = it.target;
                next_pts = it.target;
                cur_serial = it.serial;
                a_eof = false;
                continue;
            }
            if (it.eof) {
                if (it.serial == serial.load()) {
                    if (actx) {
                        avcodec_send_packet(actx, nullptr);
                        drain();
                        avcodec_flush_buffers(actx);
                    }
                    a_eof = true;
                }
                continue;
            }
            if (it.serial != serial.load() || !actx) { av_packet_free(&it.pkt); continue; }
            avcodec_send_packet(actx, it.pkt);
            av_packet_free(&it.pkt);
            drain();
        }
        av_frame_free(&fr);
        swr_free(&swr);
    }

    // ── presenting ───────────────────────────────────────────────────────────
    void present() {
        const double c = clock();
        std::lock_guard<std::mutex> lk(vm);
        bool changed = false;
        while (!vq.empty()) {
            auto& f = vq.front();
            if (!fresh) {                // first frame after open/seek: show it right away
                fresh = true;
                cur = f;
                vq.pop_front();
                changed = true;
                continue;
            }
            if (f->pts > c + 0.002) break;
            if (vq.size() >= 2 && vq[1]->pts <= c) { vq.pop_front(); n_dropped++; continue; }
            cur = f;
            vq.pop_front();
            changed = true;
        }
        if (changed && cur) { cur->seq = ++frame_seq; n_presented++; }
        vcv.notify_all();
    }

    void shutdown() {
        abort = true;
        vpq.abort = apq.abort = true;
        vpq.cv.notify_all();
        apq.cv.notify_all();
        dcv.notify_all();
        vcv.notify_all();
        acv.notify_all();
        if (dev) SDL_PauseAudioDevice(dev, 1);
        if (demux_t.joinable()) demux_t.join();
        if (vdec_t.joinable()) vdec_t.join();
        if (adec_t.joinable()) adec_t.join();
        if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
        vpq.clear();
        apq.clear();
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (vctx) avcodec_free_context(&vctx);
        if (actx) avcodec_free_context(&actx);
        if (hwdev) av_buffer_unref(&hwdev);
        for (auto& [i, sd] : subs) avcodec_free_context(&sd.ctx);
        subs.clear();
        if (swap_par) avcodec_parameters_free(&swap_par);
        if (fmt) avformat_close_input(&fmt);
        free_mem_pb();
        if (vpar) avcodec_parameters_free(&vpar);
    }
};

// ── Player: the public face ──────────────────────────────────────────────────
Player::Player() : d(std::make_unique<Impl>(*this)) {}
Player::~Player() { close(); }

void Player::open(const std::string& url, const std::string& headers, bool allow_file) {
    close();
    d = std::make_unique<Impl>(*this);
    d->url = url;
    d->headers = headers;
    d->allow_file = allow_file;
    err_code_ = 0;
    ready_ = 0;
    net_state_ = 2;
    ended_ = false;
    seeking_ = false;
    has_video_ = has_audio_ = false;
    vw_ = vh_ = 0;
    paused_ = true;
    d->emit("loadstart");
    d->demux_t = std::thread([this, impl = d.get()] {
        impl->demux_main();
        if (impl->abort) return;
        // demux_main returned either on failure or after read_loop() finished
    });
}

void Player::close() {
    if (d) d->shutdown();
    d = std::make_unique<Impl>(*this);
    ready_ = net_state_ = 0;
    has_video_ = has_audio_ = false;
}

void Player::play() {
    if (!d->demux_t.joinable() || err_code_) return;
    if (ended_) {                        // play() after the end restarts
        ended_ = false;
        seek(0);
    }
    if (!paused_) return;
    d->clock_barrier();
    paused_ = false;
    d->play_pending = true;
    d->emit("play");
    if (d->dev) SDL_PauseAudioDevice(d->dev, 0);
}

void Player::pause() {
    if (paused_) return;
    d->clock_barrier();
    paused_ = true;
    d->play_pending = false;
    d->waiting = false;
    if (d->dev) SDL_PauseAudioDevice(d->dev, 1);
    d->emit("timeupdate");
    d->emit("pause");
}

void Player::seek(double t) {
    if (!d->demux_t.joinable() || std::isnan(t)) return;
    const double dd = d->dur;
    if (std::isfinite(dd)) t = std::min(t, dd);
    t = std::max(0.0, t);
    d->clock_barrier();
    ended_ = false;
    seeking_ = true;
    d->seeked_pending = true;
    d->emit("seeking");
    ++d->serial;
    {
        std::lock_guard<std::mutex> lk(d->am);
        d->aq.clear();
        d->aq_frames = 0;
        d->audio_done = false;
        d->a_eof = false;
    }
    {
        std::lock_guard<std::mutex> lk(d->vm);
        d->vq.clear();
        d->fresh = false;
        d->v_eof = false;
    }
    d->set_clock(t);
    d->data_ok = false;
    {
        std::lock_guard<std::mutex> lk(d->dm);
        d->seek_target = t;
        d->seek_req = true;
    }
    d->dcv.notify_all();
    d->vcv.notify_all();
    d->acv.notify_all();
}

void Player::set_volume(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (v == volume_) return;
    volume_ = v;
    d->emit("volumechange");
}
void Player::set_muted(bool m) {
    if (m == muted_) return;
    muted_ = m;
    d->emit("volumechange");
}
void Player::set_rate(double r) {
    r = std::clamp(r, 0.25, 4.0);
    if (r == rate_) return;
    d->clock_barrier();
    rate_ = r;
    d->emit("ratechange");
}

double Player::duration() const { return d->dur; }
double Player::current_time() const {
    double t = seeking_ ? d->seek_target.load() : d->clock();
    const double dd = d->dur;
    if (std::isfinite(dd)) t = std::min(t, dd);
    return t;
}
std::string Player::error_message() const {
    std::lock_guard<std::mutex> lk(d->em);
    return d->err_msg;
}
std::vector<Range> Player::buffered() const {
    if (ready_ < 1) return {};
    const Impl& s = *d;
    double end = std::numeric_limits<double>::infinity();
    if (s.vi >= 0) end = std::min(end, s.v_end.load());
    if (s.ai >= 0) end = std::min(end, s.a_end.load());
    if (s.eof && std::isfinite(s.dur.load())) end = s.dur.load();
    const double start = std::min(s.buf_start.load(), d->clock());
    if (!std::isfinite(end) || end <= start) return {};
    return {{start, end}};
}

std::vector<Range> Player::seekable() const {
    const double dd = d->dur;
    if (ready_ < 1) return {};
    if (std::isfinite(dd)) return {{0, dd}};
    return buffered();
}

std::vector<Event> Player::poll() {
    Impl& s = *d;
    std::vector<Event> out;
    {
        std::lock_guard<std::mutex> lk(s.em);
        out.swap(s.events);
    }
    auto emit = [&](const char* t) { out.push_back({t, {}}); };
    if (ready_ < 1) return out;
    s.present();
    const double clk = s.clock();
    const double t = now_sec();

    if (vw_ != s.last_vw || vh_ != s.last_vh) {
        if (vw_) emit("resize");
        s.last_vw = vw_;
        s.last_vh = vh_;
    }
    const double dnow = s.dur;
    if (s.last_dur == -1) s.last_dur = dnow;
    else if (dnow != s.last_dur && !(std::isnan(dnow) && std::isnan(s.last_dur))) {
        s.last_dur = dnow;
        emit("durationchange");
    }

    // Has the first picture/audio of this load or seek arrived?
    if (!s.data_ok) {
        if (has_video_) { std::lock_guard<std::mutex> lk(s.vm); s.data_ok = s.fresh; }
        else if (has_audio_) { std::lock_guard<std::mutex> lk(s.am); s.data_ok = s.aq_frames > 0 || s.a_eof; }
        else s.data_ok = true;
    }
    if (!s.data_ok && seeking_ && s.eof && !s.seek_req) {   // seeked to the very end: nothing left to decode
        bool v_done = s.vi < 0, a_done = s.ai < 0;
        if (!v_done) { std::lock_guard<std::mutex> lk(s.vm); v_done = s.v_eof && s.vq.empty(); }
        if (!a_done) a_done = s.a_eof;
        if (v_done && a_done) s.data_ok = true;
    }
    if (seeking_ && s.data_ok && !s.seek_req) {
        seeking_ = false;
        emit("timeupdate");
        emit("seeked");
    }

    // readyState from how far ahead of the clock the demuxer has read.
    double end = std::numeric_limits<double>::infinity();
    if (!s.eof) {
        if (s.vi >= 0) end = std::min(end, s.v_end.load());
        if (s.ai >= 0) end = std::min(end, s.a_end.load());
    }
    const double ahead = end - clk;
    const int prev = ready_;
    int r = 1;
    if (s.data_ok) {
        if (ahead >= (prev >= 4 ? 1.0 : 4.0)) r = 4;
        else if (ahead >= (prev >= 3 ? 0.05 : 0.4)) r = 3;
        else r = 2;
    }
    ready_ = r;
    if (r >= 2 && !s.loaded_data) { s.loaded_data = true; emit("loadeddata"); }
    if (prev < 3 && r >= 3) emit("canplay");
    if (prev < 4 && r == 4) emit("canplaythrough");

    if (!paused_ && !ended_ && !seeking_) {
        if (s.play_pending) {
            s.play_pending = false;
            if (r >= 3) emit("playing");
            else { s.clock_barrier(); s.waiting = true; emit("waiting"); }
        } else if (!s.waiting && r < 3) {
            s.clock_barrier();
            s.waiting = true;
            emit("waiting");
        } else if (s.waiting && r >= 3) {
            s.clock_barrier();
            s.waiting = false;
            emit("playing");
        }
    }

    // Natural end of the media.
    if (s.eof && s.data_ok && !seeking_ && !ended_ && !paused_) {
        bool v_done = s.vi < 0, a_done = s.ai < 0;
        if (!v_done) { std::lock_guard<std::mutex> lk(s.vm); v_done = s.v_eof && s.vq.empty(); }
        if (!a_done) { std::lock_guard<std::mutex> lk(s.am); a_done = s.a_eof && s.aq_frames == 0; }
        if (v_done && a_done) {
            if (loop_) {
                seek(0);
            } else {
                s.clock_barrier();
                if (std::isfinite(dnow)) s.set_clock(dnow);
                ended_ = true;
                paused_ = true;
                if (s.dev) SDL_PauseAudioDevice(s.dev, 1);
                emit("timeupdate");
                emit("pause");
                emit("ended");
            }
        }
    }

    if (!paused_ && !seeking_ && t - s.last_timeupdate >= 0.25) {
        s.last_timeupdate = t;
        emit("timeupdate");
    }
    const double be = std::isfinite(end) ? end : (std::isfinite(dnow) ? dnow : 0);
    if (be != s.last_buf_end && t - s.last_progress >= 0.5) {
        s.last_buf_end = be;
        s.last_progress = t;
        emit("progress");
    }
    return out;
}

std::shared_ptr<const Frame> Player::frame(uint64_t after_seq) {
    d->present();
    std::lock_guard<std::mutex> lk(d->vm);
    if (d->cur && d->cur->seq > after_seq) return d->cur;
    return nullptr;
}

Player::Stats Player::stats() const {
    Stats s;
    s.decoded = d->n_decoded;
    s.presented = d->n_presented;
    s.dropped = d->n_dropped;
    s.hw = d->hw_used;
    s.codec = d->vcodec;
    s.audio_frames = d->a_played;
    s.audio_peak = d->a_peak;
    s.audio_codec = d->acodec;
    s.audio_channels = d->a_channels;
    s.deinterlace = d->deint;
    s.pix_fmt = d->pixfmt;
    return s;
}

std::vector<TrackInfo> Player::audio_tracks() const {
    std::lock_guard<std::mutex> lk(d->tm);
    return d->atracks;
}
std::vector<TrackInfo> Player::subtitle_tracks() const {
    std::lock_guard<std::mutex> lk(d->tm);
    return d->stracks;
}

bool Player::select_audio(int idx) {
    Impl& s = *d;
    if (!s.adec_t.joinable() || ready_ < 1) return false;
    {
        std::lock_guard<std::mutex> lk(s.tm);
        bool ok = false;
        for (auto& t : s.atracks) ok |= t.index == idx && t.usable;
        if (!ok) return false;
        if (idx == s.ai) return true;
    }
    {
        std::lock_guard<std::mutex> lk(s.dm);
        s.pending_ai = idx;
    }
    seek(current_time());                // same path as any seek: the demuxer applies the switch first
    return true;
}

std::vector<SubCue> Player::sub_cues(int idx, size_t from, size_t& total) const {
    std::lock_guard<std::mutex> lk(d->tm);
    auto it = d->subs.find(idx);
    total = 0;
    if (it == d->subs.end()) return {};
    total = it->second.cues.size();
    if (from >= total) return {};
    return std::vector<SubCue>(it->second.cues.begin() + from, it->second.cues.end());
}

// ── what we can play ─────────────────────────────────────────────────────────
bool built() { return true; }

// RFC 6381 / ISO sample-entry codec strings -> FFmpeg decoder.  Anything else is tried as an
// FFmpeg codec or decoder name ("mpeg2video", "vc1", "dca", "truehd", "pcm_s16le"...).
static bool codec_known(const std::string& c) {
    auto starts = [&](const char* p) { return c.rfind(p, 0) == 0; };
    AVCodecID id = AV_CODEC_ID_NONE;
    if (starts("avc1") || starts("avc3") || c == "h264") id = AV_CODEC_ID_H264;
    else if (starts("hvc1") || starts("hev1") || starts("dvh1") || starts("dvhe") || c == "hevc" || c == "h265") id = AV_CODEC_ID_HEVC;
    else if (starts("av01") || c == "av1") id = AV_CODEC_ID_AV1;
    else if (c == "vp8" || c == "vp08") id = AV_CODEC_ID_VP8;
    else if (starts("vp09") || c == "vp9") id = AV_CODEC_ID_VP9;
    else if (starts("mp4v.20") || c == "mpeg4") id = AV_CODEC_ID_MPEG4;
    else if (starts("mp4v.6") || starts("mp4v.105") || c == "mp2v" || c == "mpeg2video" || c == "mpeg1video") id = AV_CODEC_ID_MPEG2VIDEO;
    else if (c == "vc-1" || c == "wvc1" || c == "vc1") id = AV_CODEC_ID_VC1;
    else if (c == "wmv3") id = AV_CODEC_ID_WMV3;
    else if (c == "wmv1" || c == "wmv2") id = c == "wmv1" ? AV_CODEC_ID_WMV1 : AV_CODEC_ID_WMV2;
    else if (starts("ap4") || c == "apch" || c == "apcn" || c == "apcs" || c == "apco" || c == "prores") id = AV_CODEC_ID_PRORES;
    else if (c == "mjpg" || c == "mjpeg" || c == "jpeg") id = AV_CODEC_ID_MJPEG;
    else if (c == "theora") id = AV_CODEC_ID_THEORA;
    else if (c == "h263" || c == "s263") id = AV_CODEC_ID_H263;
    else if (starts("mp4a.40") || starts("mp4a.66") || starts("mp4a.67") || starts("mp4a.68")) id = AV_CODEC_ID_AAC;
    else if (starts("mp4a.69") || starts("mp4a.6b") || c == "mp3" || c == ".mp3") id = AV_CODEC_ID_MP3;
    else if (c == "mp2" || c == "mp4a.6a") id = AV_CODEC_ID_MP2;
    else if (c == "ac-3" || c == "ac3" || starts("mp4a.a5")) id = AV_CODEC_ID_AC3;
    else if (c == "ec-3" || c == "eac3" || starts("mp4a.a6")) id = AV_CODEC_ID_EAC3;
    else if (starts("dts") || starts("mp4a.a9") || starts("mp4a.aa") || starts("mp4a.ab") || starts("mp4a.ac")) id = AV_CODEC_ID_DTS;
    else if (c == "mlpa" || c == "truehd") id = AV_CODEC_ID_TRUEHD;
    else if (c == "opus") id = AV_CODEC_ID_OPUS;
    else if (c == "vorbis") id = AV_CODEC_ID_VORBIS;
    else if (c == "flac") id = AV_CODEC_ID_FLAC;
    else if (c == "alac") id = AV_CODEC_ID_ALAC;
    else if (c == "wma" || c == "wmav2") id = AV_CODEC_ID_WMAV2;
    else if (c == "wmav1") id = AV_CODEC_ID_WMAV1;
    else if (c == "wmapro") id = AV_CODEC_ID_WMAPRO;
    else if (c == "wav" || c == "1" || c == "pcm" || c == "ipcm" || c == "twos" || c == "sowt" || c == "lpcm") id = AV_CODEC_ID_PCM_S16LE;
    else if (c == "fpcm" || c == "pcm_f32le") id = AV_CODEC_ID_PCM_F32LE;
    if (id != AV_CODEC_ID_NONE) return avcodec_find_decoder(id) != nullptr;
    if (avcodec_find_decoder_by_name(c.c_str())) return true;
    const AVCodecDescriptor* cd = avcodec_descriptor_get_by_name(c.c_str());
    return cd && avcodec_find_decoder(cd->id);
}

std::string can_play(const std::string& spec) {
    std::string mime = spec, params;
    const size_t semi = spec.find(';');
    if (semi != std::string::npos) { mime = spec.substr(0, semi); params = spec.substr(semi + 1); }
    auto trim = [](std::string s) {
        auto junk = [](char c) { return c == ' ' || c == '"' || c == '\''; };
        while (!s.empty() && junk(s.front())) s.erase(s.begin());
        while (!s.empty() && junk(s.back())) s.pop_back();
        return s;
    };
    mime = trim(mime);
    for (auto& ch : mime) ch = (char)std::tolower((unsigned char)ch);
    // Every container FFmpeg demuxes: the answer is "maybe" without codecs (the file may hold anything).
    static const char* ok[] = {
        "video/mp4", "audio/mp4", "video/x-m4v", "video/m4v", "video/quicktime", "video/3gpp", "video/3gpp2", "audio/3gpp",
        "audio/3gpp2", "video/webm", "audio/webm", "video/x-matroska", "audio/x-matroska", "application/x-matroska",
        "video/mp2t", "video/mp2ts", "video/vnd.dlna.mpeg-tts", "video/mpeg", "video/x-mpeg", "video/mpg", "video/x-ms-wmv",
        "video/x-ms-asf", "video/x-ms-wm", "video/x-msvideo", "video/avi", "video/msvideo", "video/x-flv", "video/flv",
        "video/ogg", "application/ogg", "video/x-ogm", "video/divx", "video/x-divx", "video/dv", "video/mxf", "application/mxf",
        "audio/mpeg", "audio/mp3", "audio/mpg", "audio/x-mpeg", "audio/aac", "audio/aacp", "audio/x-m4a", "audio/m4a", "audio/x-aac",
        "audio/ogg", "audio/opus", "audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave", "audio/flac", "audio/x-flac",
        "audio/ac3", "audio/eac3", "audio/vnd.dolby.dd-raw", "audio/vnd.dts", "audio/vnd.dts.hd", "audio/x-dts", "audio/x-ms-wma",
        "audio/x-ms-asf", "audio/aiff", "audio/x-aiff", "audio/x-ape", "audio/x-wavpack", "audio/x-matroska", "audio/x-mka",
        "application/vnd.apple.mpegurl", "application/x-mpegurl", "audio/mpegurl", "audio/x-mpegurl"};
    bool known = false;
    for (const char* m : ok) if (mime == m) known = true;
    if (!known) return "";
    const size_t cp = params.find("codecs");
    if (cp == std::string::npos) return "maybe";
    std::string list = trim(params.substr(params.find('=', cp) + 1));
    bool all = true;
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        std::string c = trim(list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        for (auto& ch : c) ch = (char)std::tolower((unsigned char)ch);
        if (!c.empty() && !codec_known(c)) all = false;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return all ? "probably" : "";
}

// Decoder/demuxer inventory for pages.  Names are FFmpeg's, i.e. what ffprobe reports as codec_name /
// format_name, so a server that probes with ffprobe can compare directly.
std::string capabilities_json() {
    std::set<std::string> v, a, sub, cont;
    void* it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it)) {
        if (!av_codec_is_decoder(c) || (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL)) continue;
        const char* n = avcodec_get_name(c->id);
        if (c->type == AVMEDIA_TYPE_VIDEO) v.insert(n);
        else if (c->type == AVMEDIA_TYPE_AUDIO) a.insert(n);
        else if (c->type == AVMEDIA_TYPE_SUBTITLE) sub.insert(n);
    }
    it = nullptr;
    while (const AVInputFormat* f = av_demuxer_iterate(&it)) {
        std::string names = f->name;
        for (size_t p = 0; p <= names.size();) {
            const size_t q = names.find(',', p);
            cont.insert(names.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    auto arr = [](const std::set<std::string>& x) {
        std::string o = "[";
        for (auto& n : x) o += (o.size() > 1 ? ",\"" : "\"") + n + "\"";
        return o + "]";
    };
    return "{\"video\":" + arr(v) + ",\"audio\":" + arr(a) + ",\"subtitle\":" + arr(sub) + ",\"containers\":" + arr(cont) + "}";
}

} // namespace luxium::media
