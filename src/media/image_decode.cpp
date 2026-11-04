#include "image_decode.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace luxium::media {

namespace {

struct Mem { const uint8_t* p; size_t n, at; };

int read_mem(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<Mem*>(opaque);
    if (m->at >= m->n) return AVERROR_EOF;
    const size_t k = std::min<size_t>(size, m->n - m->at);
    std::memcpy(buf, m->p + m->at, k);
    m->at += k;
    return int(k);
}

int64_t seek_mem(void* opaque, int64_t off, int whence) {
    auto* m = static_cast<Mem*>(opaque);
    if (whence == AVSEEK_SIZE) return int64_t(m->n);
    int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? int64_t(m->at) + off : int64_t(m->n) + off;
    if (to < 0 || to > int64_t(m->n)) return -1;
    m->at = size_t(to);
    return to;
}

} // namespace

bool decode_still_image(const uint8_t* data, size_t size, int& w, int& h,
                        std::vector<uint8_t>& rgb, std::vector<uint8_t>& rgba) {
    if (size < 4 || size > (64u << 20)) return false;
    Mem mem{data, size, 0};
    constexpr int kIo = 1 << 15;
    auto* iobuf = static_cast<uint8_t*>(av_malloc(kIo));
    AVIOContext* io = avio_alloc_context(iobuf, kIo, 0, &mem, read_mem, nullptr, seek_mem);
    AVFormatContext* fmt = avformat_alloc_context();
    if (!iobuf || !io || !fmt) { if (io) { av_freep(&io->buffer); avio_context_free(&io); } else av_free(iobuf); avformat_free_context(fmt); return false; }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    bool ok = false;
    AVCodecContext* cc = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    SwsContext* sws = nullptr;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) >= 0 && avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (si >= 0) {
            AVStream* st = fmt->streams[si];
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            cc = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (cc && avcodec_parameters_to_context(cc, st->codecpar) >= 0 && avcodec_open2(cc, codec, nullptr) >= 0) {
                bool got = false;
                while (!got && av_read_frame(fmt, pkt) >= 0) {
                    if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, fr) >= 0) got = true;
                    av_packet_unref(pkt);
                }
                if (!got) { avcodec_send_packet(cc, nullptr); got = avcodec_receive_frame(cc, fr) >= 0; }
                if (got && fr->width > 0 && fr->height > 0 && fr->width <= 16384 && fr->height <= 16384) {
                    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(fr->format));
                    const bool alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
                    w = fr->width; h = fr->height;
                    sws = sws_getContext(w, h, AVPixelFormat(fr->format), w, h, alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (sws) {
                        std::vector<uint8_t>& out = alpha ? rgba : rgb;
                        const int bpp = alpha ? 4 : 3;
                        out.assign(size_t(w) * h * bpp, 0);
                        uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
                        int dstride[4] = {w * bpp, 0, 0, 0};
                        sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dstride);
                        if (alpha) {   // swscale hands RGBA out as the decoder had it: straight alpha for GIF/ICO/AVIF
                            rgb.clear();
                        }
                        ok = true;
                    }
                }
            }
        }
    }
    if (sws) sws_freeContext(sws);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    if (cc) avcodec_free_context(&cc);
    if (fmt) avformat_close_input(&fmt);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    return ok;
}

bool decode_animation(const uint8_t* data, size_t size, int& w, int& h,
                      std::vector<std::vector<uint8_t>>& frames, std::vector<int>& delays_ms) {
    if (size < 6 || size > (32u << 20)) return false;
    Mem mem{data, size, 0};
    constexpr int kIo = 1 << 15;
    auto* iobuf = static_cast<uint8_t*>(av_malloc(kIo));
    AVIOContext* io = avio_alloc_context(iobuf, kIo, 0, &mem, read_mem, nullptr, seek_mem);
    AVFormatContext* fmt = avformat_alloc_context();
    if (!iobuf || !io || !fmt) { if (io) { av_freep(&io->buffer); avio_context_free(&io); } else av_free(iobuf); avformat_free_context(fmt); return false; }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVCodecContext* cc = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    SwsContext* sws = nullptr;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) >= 0 && avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (si >= 0) {
            AVStream* st = fmt->streams[si];
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            cc = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (cc && avcodec_parameters_to_context(cc, st->codecpar) >= 0 && avcodec_open2(cc, codec, nullptr) >= 0) {
                size_t total = 0;
                auto take = [&]() {
                    if (fr->width <= 0 || fr->height <= 0 || fr->width > 2048 || fr->height > 2048) return false;
                    if (frames.empty()) { w = fr->width; h = fr->height; }
                    if (fr->width != w || fr->height != h) return false;
                    if (!sws) sws = sws_getContext(w, h, AVPixelFormat(fr->format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws) return false;
                    std::vector<uint8_t> px(size_t(w) * h * 4);
                    uint8_t* dst[4] = {px.data(), nullptr, nullptr, nullptr};
                    int dstride[4] = {w * 4, 0, 0, 0};
                    sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dstride);
                    total += px.size();
                    if (total > (96u << 20) || frames.size() >= 300) return false;
                    // how long it shows: its own duration, else the gap to the next frame's time (GIF delay, in 1/100 s)
                    int64_t dur = fr->duration;
                    int ms = dur > 0 ? int(dur * 1000 * st->time_base.num / std::max(1, st->time_base.den)) : 100;
                    frames.push_back(std::move(px));
                    delays_ms.push_back(std::clamp(ms, 20, 10000));   // browsers clamp tiny delays up
                    return true;
                };
                bool stop = false;
                while (!stop && av_read_frame(fmt, pkt) >= 0) {
                    if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0)
                        while (!stop && avcodec_receive_frame(cc, fr) >= 0) { if (!take()) stop = true; }
                    av_packet_unref(pkt);
                }
            }
        }
    }
    if (sws) sws_freeContext(sws);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    if (cc) avcodec_free_context(&cc);
    if (fmt) avformat_close_input(&fmt);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (frames.size() < 2) { frames.clear(); delays_ms.clear(); return false; }
    return true;
}

} // namespace luxium::media
