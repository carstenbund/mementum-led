# Matrix capability catalog, feasibility & color/gradient evaluation

**Status:** evaluation / RFC. No code yet. Purpose: weigh what the Adafruit matrix stack
can do against the constraints of our time-synchronized, server-controlled display loop, so
we can decide *what is worth building* and *how destructive* each option is before touching
the proven render path.

---

## 1. The constraints that drive every score

Our display is not a generic framebuffer; it is a **phase-locked swarm**. Five things bound
what is feasible:

1. **Shared-clock determinism.** Every panel renders a frame purely from
   `step = (serverNow() - displayAt) / SCROLL_INTERVAL_MS`. Anything visual must be a
   deterministic function of `step`, screen position, or `serverNow()` — *never* of local
   `millis()`/`random()`, or panels desync.
2. **v1.3 transport is sacred-ish.** The wire is `/play?seq&at&data`. We can enrich the
   **grammar of `data`** for free (firmware parses it); adding new query params or feedback
   channels is a real protocol change.
3. **Server stays the brain.** Orchestration (who shows what, when) lives in `server.py`.
   The firmware should stay a dumb, deterministic *renderer*.
4. **Per panel = 8×8 = 64 px.** Compute budget per frame is enormous relative to the work;
   per-pixel math every 120 ms step is free. RAM is a non-issue (a 16-bit canvas is 128 B).
5. **Stateless-per-frame render.** `renderScheduled()` redraws the whole frame from scratch
   each step. Effects that need *accumulated history* (true decay trails, cellular automata)
   fight this; effects expressible as a closed-form function of (x, y, step) fit perfectly.

### Scoring rubric (Feasibility = likelihood of success in *our* system, 1–5)

| Score | Meaning |
|---|---|
| **5** | Trivial. Pure `data`-grammar or server-side change, fully deterministic, low risk. |
| **4** | Easy. Small additive firmware change; fits the loop; maybe a version bump. |
| **3** | Moderate. Needs the render-path rasterizer or per-panel server math + version gate; contained risk. |
| **2** | Hard. Strains determinism or needs new protocol params / cross-panel coordinate frame. |
| **1** | Fights the architecture. Needs per-frame history, synced randomness, or breaks the shared clock. |

Each entry also notes **Protocol impact** (None / data-grammar / new-param) and **Blast
radius** (how much of the proven loop it disturbs).

---

## 2. Catalog A — Adafruit drawing primitives

What the `Adafruit_GFX` + `Adafruit_NeoMatrix` (which is *both* a GFX canvas **and** a
NeoPixel strip via multiple inheritance) actually expose, and how each lands here.

| Primitive | Feas. | Protocol | Use case in our wall | Notes |
|---|---|---|---|---|
| `fillScreen` / `drawPixel` | 5 | None | The foundation everything else is built on. | Already used; per-pixel is the unlock for color. |
| `drawLine` / `drawRect` / `fillRect` | 5 | data-grammar | Backgrounds behind text; bars; wipes; a "fill" effect that sweeps the wall. | Trivial, deterministic. |
| `drawCircle` / `fillCircle` / `drawTriangle` | 4 | data-grammar | Icon-ish accents (a dot that pulses, a ball that bounces). | Cheap; mostly a content question. |
| `drawBitmap` (1-bit) | 4 | data-grammar / asset | Logos, glyphs the font lacks (heart, arrow, mascot). | Needs the bitmap stored in firmware/SPIFFS; server picks by name. |
| `drawRGBBitmap` (16-bit) | 3 | new-param/asset | Full-color sprites/animation cels. | Payload is large for the wire; better stored on-device and referenced by id. |
| `drawChar` (per-char color) | 4 | data-grammar | Per-character coloring → cheap "blocky" gradients & rainbow words. | One color per glyph; the easy first step toward gradients. |
| Per-pixel glyph rasterizer (custom) | 3 | data-grammar | **Smooth** per-pixel gradients / rainbow / shimmer on text. | The real render-path change (see §4); reads `glcdfont` columns. |
| `setTextSize` (scale) | 4 | data-grammar | Bigger glyphs across a tall stack; emphasis. | 8 px tall limits size 1; matters only if panels are stacked vertically. |
| Custom `GFXfont` | 3 | asset | Nicer/− narrower typefaces; true variable width. | Replaces our `getCharWidth` hint table; would need server-side width parity. |
| `setRotation` | 5 | config | Already used (`matrix_rotation`). | Per-panel mounting orientation. |
| `getTextBounds` | 5 | None (server already mirrors) | Exact scroll-width math. | Server's `scroll_duration_ms` is our port of this. |

## 3. Catalog B — Color machinery

| Feature | Feas. | Protocol | Use case | Notes |
|---|---|---|---|---|
| `Color(r,g,b)` → 16-bit 565 | 5 | data-grammar | All named colors today; gradient stops. | 5/6-bit per channel — fine for 8–40 px gradients. |
| `ColorHSV(hue,sat,val)` | 5 | data-grammar | **Rainbows / smooth gradients** by lerping hue, not RGB. | NeoMatrix inherits it; the right tool for spectra. |
| `gamma32` / `gamma8` | 4 | None | Perceptually even brightness ramps & fades. | Apply to computed colors for non-muddy gradients. |
| `setBrightness` (global) | 4 | data-grammar/new-param | Global dim, pulse, fade-in/out (as f(serverNow)). | Global only — not per-pixel; cheap mood control. |
| `setPassThruColor(uint32_t)` | 3 | data-grammar | True 24-bit per-pixel color (bypass 565). | Clunky per-pixel (set→draw→reset); verify API in your lib version. |

## 4. The gradient question (the trigger for all this)

### Span axes
- **Along the text** (color = f(column within message)). Self-contained, travels with the
  text, tiles correctly in marquee mode. ← matches your `@gradredblue` idea.
- **Across the wall** (color = f(physical panel position)). Fixed in space.
- **Over time** (color = f(serverNow())). Cycling, phase-locked across the wall.

### Two architectures
**A. Intrinsic gradient in `data`.** Firmware parses a paint spec and colors each
glyph-column by its position in the string. *Feasibility 3* (needs the rasterizer). Protocol:
data-grammar only. Use case: a scrolling rainbow word, a red→blue headline.

**B. Server-sliced wall gradient.** The server already sends **per-panel `data`** (that's how
`/identify` works). It computes each panel's slice of a global gradient and sends each panel a
simple 2-stop ramp for its own 8 px. *Feasibility 4* — firmware only needs a left→right 2-color
fill; the server does the math and stays the brain. Protocol: data-grammar. Use case: "the
whole wall is one sunset / one rainbow," static or slowly cycling. Doesn't travel with
scrolling text — best for fills/holds.

> A and B are complementary: **A** = colorful moving text; **B** = the wall as one canvas.
> They share the same paint-descriptor parser, so building A's parser gets B nearly for free.

### Color math
HSV-hue interpolation (`ColorHSV`) for rainbows and any multi-hue sweep; RGB/565 lerp for a
literal A→B two-stop. Run results through `gamma32` so mid-tones aren't muddy.

### Grammar proposal (extends `data`, wire format unchanged)
```
@grad:red-blue            two named stops, along text
@grad:red-yellow-blue     N stops
@hex:FF0000-0000FF        arbitrary 24-bit stops
@rainbow                  full hue sweep along text
@hsv:cycle                hue sweep that rotates over serverNow() (phase-locked)
```
Parser returns a **paint descriptor** (`solid | grad[stops] | rainbow | hsvcycle` + direction)
stored in `DisplaySchedule`, instead of a single `uint16_t`.

## 5. Catalog C — Higher-level effects (built from the above)

| Effect | Feas. | Determinism note | Use case |
|---|---|---|---|
| Solid-color text (today) | 5 | f(none) | Baseline messaging. |
| Per-character gradient/rainbow | 4 | f(char index) | Lively headlines without the rasterizer. |
| Per-pixel gradient/rainbow | 3 | f(x, step) | Premium look on scrolling text. |
| Wall-spanning gradient (server-sliced) | 4 | server math | "One big canvas" ambient fills. |
| HSV color **cycle** | 4 | f(serverNow()) | Slow breathing color across the whole wall, in sync. |
| Brightness pulse / fade in-out | 4 | f(serverNow()) | Attention pulse; gentle idle "heartbeat". |
| Background fill behind text | 4 | f(x,y) | Highlighted/banner messages. |
| Comet **trail** (closed-form) | 3 | f(x, step) | A head + fading tail drawn as a function, not accumulated. |
| Per-panel sparkle/twinkle | 3 | f(hash(x,y,step)) per panel | Festive shimmer; only needs *per-panel* determinism. |
| Wall-coherent sparkle/plasma | 2 | needs global (x,y) frame | "Whole wall" noise fields — wants panels to know their slot. |
| Vertical motion / rain | 2 | needs vertical stacking + scroll rewrite | Only meaningful if panels stack in Y. |
| RGB sprite animation | 2–3 | asset-on-device + frame timing | Mascot/logo loops; heavy if streamed. |
| Accumulating decay trails / automata | 1 | needs frame history | Fire, true particle systems — fights stateless render. |

## 6. The "destructive redesign" assessment

The worry is replacing the proven `Matrix.print()` path. Reality check:

- **The loop structure does not change.** `renderScheduled()` already: computes `step`,
  `fillScreen`, positions, draws, `show()`. Only the **"draw the glyphs"** inner step changes
  from one `print()` call to a per-glyph/per-pixel paint using the descriptor.
- **It can be strictly additive.** Keep the solid-color path; branch to the rasterizer only
  when the paint descriptor isn't `solid`. Old behavior is byte-identical when no `@grad` is
  present. **Blast radius: one function.**
- **Server parity stays mandatory.** `scroll_duration_ms` already mirrors `getCharWidth`; a
  variable-width custom font would force us to keep that mirror exact (this is the one place a
  font change *is* genuinely invasive — see the still-disabled 7 px `m/w` hint).
- **Migration is clean via capability negotiation.** The server already captures each client's
  firmware version on `/register`. Bump renderer-capable firmware to **1.5**; the server sends
  `@grad/@rainbow` only to ≥1.5 panels and degrades to the nearest solid `@color` for older
  ones. Mixed walls keep working; no flag-day reflash.

**Net:** the gradient renderer is **contained, reversible, and version-gated** — not a
destructive rewrite, provided we (a) branch rather than replace, and (b) hold server↔firmware
width parity.

## 7. Feasibility leaderboard (what to build first, by value ÷ risk)

1. **Per-character gradient + `@grad`/`@rainbow` grammar** — Feas 4, big visual payoff, no
   rasterizer yet, data-grammar only. *Best first step.*
2. **HSV cycle & brightness pulse** (f(serverNow())) — Feas 4, "alive" idle wall, trivially
   deterministic.
3. **Per-pixel rasterizer** — Feas 3, upgrades #1 to smooth and unlocks shimmer/comet-tail;
   the real (but contained) render change.
4. **Server-sliced wall gradient** — Feas 4, "one canvas" look, reuses the parser.
5. **Sprites/icons via `drawBitmap`** — Feas 4 for 1-bit on-device assets; nice for logos.

Avoid early: accumulating trails/automata (Feas 1) and wall-coherent noise (Feas 2) until/
unless we give the firmware a notion of its **slot/global coordinate** (a separate, larger
decision).

## 8. The slot question (a fork in the road)

Several high-value "whole wall" effects (coherent gradient that *moves*, wall-wide plasma,
sparkle that flows across joins) need each panel to know **where it sits** (its slot). Today
the server fakes this by sending per-panel `data`. Two paths:

- **Keep firmware slot-blind** (server slices everything). Simpler firmware, more server math,
  works for static/slow wall effects.
- **Teach firmware its slot** (config `slot,N`, already sketched in the failover doc). Unlocks
  self-computed wall-coherent effects with tiny payloads, but is a real protocol/config step.

This choice gates the Feas-2 tier; the Feas-3-to-5 tier needs none of it.

## 9. Decisions to converge on

1. First build = **per-character `@grad`/`@rainbow`** (Feas 4) before the rasterizer? 
2. Color math = **HSV** default (prettier) with optional literal RGB two-stop?
3. Version bump to **1.5** + server capability-gating via the version we already capture?
4. Do we want the **per-pixel rasterizer** in the first firmware change, or ship per-character
   first and upgrade?
5. **Slot question** (§8): commit to slot-blind for now, or invest in per-panel slot to open
   the wall-coherent tier?

Recommended opener: **#1 yes, #2 HSV, #3 yes, #4 per-character first then rasterizer, #5 stay
slot-blind** — highest visual payoff for the least disturbance, with the rasterizer and slot
as deliberate, separately-evaluated next steps.
