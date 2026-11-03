# <video> / <audio> in the own engine

Pages without JavaScript (our HTML/CSS/LuxScript engine) play media with the
same `media::Player` the CEF bridge uses.

- `src/media/own_media.{hpp,cpp}`: registry of players keyed by the DOM element.
  `attach` (called while the box tree is built) opens/updates the player and
  returns the video's display size; `sweep` closes players whose element left
  the tree; `reset` runs on every navigation. `tick` (once per shell frame)
  polls events, starts `autoplay` on `canplay`, converts new NV12 pictures with
  swscale (BT.601/709/2020, limited/full) to the RGB `ImageBitmap` the display
  list already paints (capped at 1280 px wide), and reports a size change.
- Layout (`build.cpp`, `paint.cpp`): `<video>` and `<audio controls>` are
  `Replaced` boxes. Intrinsic size: the video's, else the poster's, else 300x150
  (audio 300x54); CSS width/height/aspect, `object-fit` and radius apply as for
  `<img>`. The box paints the poster (or black) and records a `MediaBox`; the
  shell swaps the current frame into that display-list item each repaint
  (`media_patch`), so a playing video never forces a relayout. A relayout
  happens once, when the size becomes known.
- Controls (`layout/media_ui.cpp`, only with `controls`): a bar over the bottom
  32 px (the whole box for audio) with play/pause, progress (buffered + played +
  knob) and `m:ss / m:ss`, built as a display list drawn over the page.
  Clicking the bar buttons/progress is handled by `Shell::handle_click`; any
  other click on the element toggles play/pause (`Page::on_media_click`).
- Attributes: `src` or first `<source src>`, `autoplay`, `loop`, `muted`,
  `controls`, `poster`, `width`/`height`. Local pages may play `file:` media.

## Testing headless

    SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy build/luxium examples/media/own-engine.html \
        --size 1000x520 --click-at 300,366@1000 --wait 1800 --shot out.ppm

`--wait ms` pumps the players before the dump (frames change with it);
`--click-at x,y[@ms]` clicks at window coordinates `ms` after start.

## Not done

- LuxScript API (`el.play()/pause()/seek(t)/current_time()/duration()`):
  `src/lux_script/browser_api.inc` is owned by Lux. Needed on its `luxium`
  branch: element methods `play()`, `pause()`, `seek(t: float)` and getters
  `current_time() -> float`, `duration() -> float`, `paused() -> bool`. The
  browser side is ready: `media::own::play/pause/seek/state(element)`.
- Volume/rate/fullscreen controls, text tracks, `<video>` in fixed/transformed
  contexts beyond what `<img>` supports, `<source type>` selection.
