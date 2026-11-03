// luxium_mediatest file_or_url [seconds] [--seek t] [--dump out.ppm] ...
// Headless self-test of the media engine: plays for N seconds with SDL's
// dummy audio driver, polls like a page would, prints the events in order and
// the frame statistics.  Exit code 1 if the media errored or no frame arrived.
#include "media/player.hpp"

#include <SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace luxium::media;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: luxium_mediatest url [seconds] [--seek t[,t...]] [--rate r] [--dump f.ppm] [--audio]\n"); return 2; }
    double secs = 5, rate = 1;
    std::string dump, seeks;
    bool real_audio = false, quiet = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seek" && i + 1 < argc) seeks = argv[++i];
        else if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--audio") real_audio = true;
        else if (a == "-q") quiet = true;
        else secs = std::atof(a.c_str());
    }
    if (!real_audio) setenv("SDL_AUDIODRIVER", "dummy", 0);
    SDL_Init(SDL_INIT_AUDIO);

    Player p;
    const auto t0 = Clock::now();
    auto at = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    p.open(argv[1], "", true);
    if (rate != 1) p.set_rate(rate);
    p.play();

    std::vector<double> seek_at;
    for (size_t pos = 0; pos < seeks.size();) {
        size_t c = seeks.find(',', pos);
        seek_at.push_back(std::atof(seeks.substr(pos, c == std::string::npos ? c : c - pos).c_str()));
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    size_t next_seek = 0;
    double seek_time = secs / (seek_at.size() + 1);
    uint64_t seq = 0, shown = 0;
    std::shared_ptr<const Frame> last;
    int errors = 0;
    double first_frame = -1, max_gap = 0, last_present = 0, frame_ms = 0;
    bool done = false;
    while (!done && at() < secs) {
        for (auto& e : p.poll()) {
            if (e.type == "timeupdate" && quiet) continue;
            std::printf("%7.3f  %-15s t=%.3f ready=%d%s%s\n", at(), e.type.c_str(), p.current_time(),
                        p.ready_state(), e.detail.empty() ? "" : "  ", e.detail.c_str());
            if (e.type == "error") ++errors;
            if (e.type == "ended") done = true;
        }
        const auto f0 = Clock::now();
        if (auto f = p.frame(seq)) {
            frame_ms += std::chrono::duration<double, std::milli>(Clock::now() - f0).count();
            seq = f->seq;
            ++shown;
            last = f;
            if (first_frame < 0) first_frame = at();
            if (last_present > 0) max_gap = std::max(max_gap, at() - last_present);
            last_present = at();
        }
        if (next_seek < seek_at.size() && at() >= seek_time * (next_seek + 1)) {
            std::printf("%7.3f  >> seek(%.2f)\n", at(), seek_at[next_seek]);
            p.seek(seek_at[next_seek++]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    auto st = p.stats();
    std::printf("-- %s %dx%d dur=%.2f t=%.3f hw=%d decoded=%llu presented=%llu dropped=%llu first_frame=%.3fs max_gap=%.0fms buffered=",
                st.codec.c_str(), p.video_width(), p.video_height(), p.duration(), p.current_time(), (int)st.hw,
                (unsigned long long)st.decoded, (unsigned long long)st.presented, (unsigned long long)st.dropped,
                first_frame, max_gap * 1000);
    for (auto& r : p.buffered()) std::printf("[%.1f,%.1f]", r.start, r.end);
    std::printf("\n");
    if (!dump.empty() && last) {
        // NV12 -> grey PPM of the luma plane is enough to see "not black"
        if (FILE* f = std::fopen(dump.c_str(), "wb")) {
            std::fprintf(f, "P5\n%d %d\n255\n", last->w, last->h);
            std::fwrite(last->data.data(), 1, (size_t)last->w * last->h, f);
            std::fclose(f);
        }
    }
    p.close();
    return errors || (p.has_video() && !shown) ? 1 : 0;
}
