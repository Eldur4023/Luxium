#pragma once
// MediaApi (script/engine.hpp) over media::own: what <video>.play()/seek()/
// audio_tracks()... from LuxScript reach.  The players belong to the Shell
// (media::own::Scope), keyed by DOM node.

#include <script/engine.hpp>
#include <dom/node.hpp>

namespace luxium::shell {

class OwnMediaApi : public MediaApi {
public:
    explicit OwnMediaApi(const void* owner) : owner_(owner) {}
    MediaState state(const dom::Node* n) override;
    void play(const dom::Node* n) override;
    void pause(const dom::Node* n) override;
    void seek(const dom::Node* n, double seconds) override;
    void cycle_audio(const dom::Node* n) override;
    void cycle_subtitle(const dom::Node* n) override;
private:
    const void* owner_;
};

} // namespace luxium::shell
