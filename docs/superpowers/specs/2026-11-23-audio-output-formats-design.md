# Audio output: sample formats, devices and state

Status: design, 2026-11-23. Repos: PRISM, prism-esp32, prism-mp3-player.

## Why

prism-mp3-player carries its own audio-output interface (`player::PcmOutput`) with three
implementations: the desktop's SDL one, the Korvo's speaker and the Korvo's Bluetooth headset.
It exists only because PRISM's `audio::Output` cannot do what a board needs. The goal is to move
that infrastructure into PRISM and prism-esp32, so the next audio app on a board (or the user's
own ESP32-S31 board) gets the speaker and the headset for free, and the player keeps only player
code.

The user asked for the output to support several sample formats "like pixel formats do", so an
app can pick what is cheapest on its platform: 16-bit integers on the ESP32 (the codec and A2DP
both take them), floats on the desktop (mixing, synthesis). And for a Qt-like shape:
`QMediaDevices` (a device list) and `QAudioSink` (an output with a state).

## What exists

- `prism::audio::Output(Format, RenderFn)` (include/prism/audio/output.hpp,
  src/backends/sdl_audio.cpp). It starts at construction on SDL's default device and calls
  `RenderFn = std::function<void(std::span<float>)>` on SDL's audio thread. When no device opens,
  a timer thread calls the same function at the block rate, silently (`playing() == false`). The
  destructor stops the thread; render is never called after it returns. `load()` measures the
  render cost. Nothing allocates on the rendering thread (tests/test_audio_output.cpp checks it).
  Users: examples/model_synth_rack, the tests, the player's desktop output.
- doc/design/porting-and-devices.md §3 already decided that `audio::Output` keeps its
  render-function API, and that sample streams are typed by their sample (`typename S::sample`,
  `int16_t`, `float`, …).
- The player's interface (prism-mp3-player core/audio.hpp): `accepts(PcmFormat)`,
  `start(PcmFormat, PcmRing&)`, `stop()` (must not return while the ring is read),
  `connected()`, `underruns()`, `set_volume(float)`. The Player recreates the output for each
  track's rate (44.1 or 48 kHz), pauses when `connected()` turns false, and skips tracks whose rate
  the output does not accept. Its Bluetooth output also carries pairing, AVRCP buttons and a
  state text for the Settings screen.

What `audio::Output` lacks for the player: choosing a device, telling "the device is gone" apart
from "no device, rendering on a timer", asking which rates a device plays, 16-bit samples, and a
volume.

## Goals

1. A render function in `int16_t` or `float`, chosen by the app. When it matches the device's
   native type, samples go straight through; otherwise `Output` converts, in one place.
2. A device list, Qt-style: `audio::devices()`, each device with an id, a name, the rates and
   channel counts it plays, its native sample type, and whether it is the default.
3. `Output` opens a chosen device (the default when none is named).
4. `Output::state()`: playing on the device, rendering on the timer (no device), or lost (the
   device went away). `error()` says why.
5. `Output::set_volume(float)`: the device's own volume when it has one (the Korvo codec),
   otherwise a gain applied with the conversion.
6. prism-esp32 implements the board's devices: the speaker (codec + amplifier) and the Bluetooth
   headset (A2DP source), plus the Bluetooth link (bring-up, pairing, reconnect, AVRCP buttons,
   state text) the headset device needs.
7. A device a test drives by hand, in PRISM, so an app (the player) tests its audio without a
   thread or a sound card.
8. The player drops `PcmOutput`, its three implementations and RingTap, and uses
   `prism::audio::Output`.

## Non-goals

- Audio input (microphones), several simultaneous outputs, mixing inside PRISM.
- Resampling inside PRISM: a device lists the rates it plays; an app that has another rate
  resamples itself or skips (the player skips, as today). SDL resamples on its own; its device
  lists the common rates.
- Hot-plug notifications for new devices. A device going away is reported (`lost`); a new one
  appears in the next `devices()` call.
- A general `prism::bt` module (discovery, BLE, RFCOMM). Bluetooth here is the board's A2DP link
  only; a portable Bluetooth model is a later design.

## Anti-goals (would make this a failure even if it works)

- Any allocation, lock or blocking call on the rendering thread, for either sample type.
- Breaking existing float users: `Output(Format, RenderFn)` with a float render function keeps
  compiling and behaving as today.
- A headset that disconnects while its ring drains silently on a timer: a lost device stops
  calling render and says so.
- Template code in headers: `Output` stays a non-template class, its code in src/ (AGENTS.md).

## Approaches considered

1. **Sample type picked by the render function, one non-template `Output`** (chosen). Two
   constructors, one for `std::function<void(std::span<std::int16_t>)>`, one for `float`; the
   implementation holds a variant. Conversion happens once per block, which costs little next to
   decoding, so nothing is gained by compiling `Output` per sample type.
2. **`BasicOutput<Sample>` templated like `BasicCanvas<P>`.** Zero dispatch, but every backend is
   compiled per type, the code moves into headers or explicit instantiations, and the per-block
   cost it saves is noise. Pixel formats are templates because blending runs per pixel, in the
   innermost loop; audio conversion is not that loop.
3. **Raw bytes plus a runtime format enum** (as SDL does). Untyped spans: a 16-bit render function
   handed a float buffer compiles. Rejected: PRISM spells units as types.

Why plain `int16_t`/`float` and not trait structs like `Rgb565`: a pixel format's storage
(`uint16_t`) is not its meaning (5-6-5 bits), so it needs a trait; an audio sample's storage is
its meaning. The type is the format. A `concept Sample` names the two allowed types.

## The design

### API (include/prism/audio/output.hpp)

```cpp
namespace prism::audio {

template <class T> concept Sample = std::same_as<T, std::int16_t> || std::same_as<T, float>;
enum class SampleType { s16, f32 };
template <Sample T> using SampleRenderFn = std::function<void(std::span<T> interleaved)>;
using RenderFn = SampleRenderFn<float>;   // today's name keeps today's meaning

struct Format { int sample_rate = 48000; int channels = 2; int block_frames = 512; };  // as today

struct DeviceInfo {
    std::string id;            // stable within a run; "" never names a device
    std::string name;          // for a menu: "Speaker", "WH-1000XM3", "Built-in Audio"
    std::vector<int> rates;    // what it plays; empty: any rate (the backend resamples)
    int max_channels = 2;
    SampleType native = SampleType::f32;
    bool is_default = false;
    bool accepts(const Format&) const;
};
std::vector<DeviceInfo> devices();   // safe from any thread; may block briefly (enumeration)

enum class State { playing, timer, lost };

class Output {
public:
    // The default device. With none, the timer renders (as today).
    Output(Format, RenderFn);
    Output(Format, SampleRenderFn<std::int16_t>);
    // A named device. If it cannot open, or does not accept `format`: state() == lost, nothing renders.
    Output(std::string_view device_id, Format, RenderFn);
    Output(std::string_view device_id, Format, SampleRenderFn<std::int16_t>);
    ~Output();   // render is never called after it returns (unchanged contract)

    State state() const;
    bool playing() const { return state() == State::playing; }   // kept for existing users
    const std::string& error() const;
    Format format() const;
    Load load() const;
    void set_volume(float);   // 0..1, any thread; applied from the next block
};

// Converting, exposed because backends and apps both need it. Saturating, round-to-nearest.
void convert(std::span<const float> in, std::span<std::int16_t> out, float gain = 1.f);
void convert(std::span<const std::int16_t> in, std::span<float> out, float gain = 1.f);

}
```

Decided:
- The timer fallback applies only to the default-device constructors (a desktop with no sound
  card keeps working as today). A named device that fails is `lost`: the app asked for that one.
- `lost` is sticky for that `Output`. To play again the app makes a new `Output` (the player
  already makes one per track). Polling `state()` is the notification. simplify: no state-change
  callback. Upgrade path: a `core::Shared<State>` or a sender the UI can observe.
- Volume: a device with hardware volume uses it; otherwise the gain multiplies during conversion
  (and, for a native-type render function, in one extra pass over the block). Volume 1 with a
  native type touches nothing.
- The conversion buffer is allocated at construction: `kMaxBlockFrames × channels` of the other
  type.

### Devices outlive Outputs (call made after the builder check)

An `Output` is cheap and short-lived: the player makes one per track and destroys it at every
pause. The hardware must not follow it. Opening the codec, starting a writer thread or
restarting the A2DP stream per track would click, gap, fragment internal RAM, and some headsets
drop a stream restarted too often. Today's board code already works this way: the codec and the
writer start once, the A2DP stream keeps sending silence between tracks, and only the ring is
attached and detached.

So a backend opens a device's hardware on the first `Output` that uses it and keeps it open. An
`Output` only attaches its render function to the open device (through the `RenderGate`
handshake) and detaches it in its destructor; between Outputs the device plays silence. A new
`Output` at another rate reconfigures the device only when the rate differs. simplify: a device,
once opened, stays open until the program ends. Upgrade: close after an idle timeout, for power.

### The seam between Output and a backend (call made after the builder check)

`Output` itself lives in PRISM (src/audio/output.cpp) and owns everything every platform shares:
the sample-type variant, conversion, the volume gain, the conversion buffer, `state()`, `load()`,
the timer fallback. Below it, one small interface a platform implements:

```cpp
struct DeviceBackend {                         // prism/audio/backend.hpp
    virtual std::vector<DeviceInfo> devices() = 0;
    // Starts calling `pull` on the device's thread for `format`; the returned stream's destructor
    // must not return while `pull` runs. Null + `error` when it cannot.
    virtual std::unique_ptr<DeviceStream> open(std::string_view id, const Format&,
                                               SampleType, PullFn pull, std::string& error) = 0;
};
void register_backend(DeviceBackend&);         // the platform's, at startup; ManualDevice's, in tests
```

`PullFn` hands the device's native buffer; `Output` fills it from the app's render function,
converting when the types differ. Virtual calls happen at open time and once per block, which
costs nothing next to the block itself. SDL registers its backend in the SDL build; prism-esp32
registers the board's; `ManualDevice` registers itself while it lives. `devices()` merges them.

### What listing does on the board (call made after the builder check)

Listing never starts hardware. The Bluetooth controller costs a large share of internal RAM, so
it starts only when the app calls `prism::esp32::bluetooth::start(name)`, as the player does
today from its boot setting; the headset device is listed from then on. The speaker is always
listed; its codec opens on the first `Output` on it. The player keeps its boot setting (speaker
or headset) and brings up only that one.

### A headset still connecting (call made after the builder check)

An `Output` opened on the headset while it is not connected is `lost` at once, with error
"headset not connected". That is today's behaviour: the player checks `connected()` before it
starts and pauses with "Headset disconnected". A later "wait for the link" state is an upgrade,
not part of this design.

### Backends

Each platform registers one `DeviceBackend` (above):

- **SDL (desktop, src/backends/sdl_audio.cpp).** Native `f32`. `devices()` from
  `SDL_GetAudioPlaybackDevices`, ids from the SDL device ids. `rates` empty (SDL resamples).
  `lost`: SDL's device-removed event for the opened device. `int16_t` render functions are
  converted to float in the callback.
- **A test device (PRISM, `prism::audio::ManualDevice`, built in every configuration).** Created
  by a test; while it lives, `devices()` lists it (`id "manual"`). An `Output` opened on it starts
  no thread; the test calls `pull(frames)` to run one render block and reads the samples back, and
  `lose()` to simulate a disconnect. This replaces the player's FakeOutput.
- **prism-esp32 (board).** Under `components/prism` (or a sibling component), next to the other
  board code:
  - *Speaker:* the BSP's codec through esp_codec_dev. Native `s16`, rates {44100, 48000},
    hardware volume. A writer thread on core 0 (internal-RAM stack, as today) calls render and
    writes the codec; I2S DMA paces it. Moved from the player's speaker_output.cpp.
  - *Bluetooth headset:* A2DP source, native `s16`, rates {44100}, gain volume (AVRCP absolute
    volume stays the noted upgrade). Listed when Bluetooth is enabled; `lost` when the headset
    disconnects. The A2DP data callback calls render. Moved from the player's bt_*.cpp.
  - *The stop handshake:* each board backend must guarantee render is not running when the
    destructor returns. The player's RingTap handshake (seq_cst `inside` flag + pointer, Dekker)
    becomes a small `RenderGate` around the render function inside prism-esp32, with its
    ThreadSanitizer test moved along.
  - *The Bluetooth link* (bring-up, bonded-headset reconnect, scan for a configured name,
    AVRCP buttons, state text + revision) becomes a prism-esp32 API, e.g.
    `prism::esp32::bluetooth::start(name)`, `on_remote(handler)`, `state_text()`,
    `state_revision()`. Moves only: the link logic is board-verified and has no host tests yet.

### The player after

- `Player` takes a device id (or nothing: the default) instead of a `PcmOutput&`. At each track
  start it builds `audio::Output(id, {rate, 2}, SampleRenderFn<int16_t>)`; the render function pops its
  `PcmRing` (the ring stays: the decoder runs on the logic thread). Destroying the `Output` is
  `stop()`.
- `accepts` → the device's `DeviceInfo::accepts`; `connected()` → `state() != lost`;
  `underruns()` stays the ring's; `set_volume` → `Output::set_volume`, kept across `Output`s by the
  Player.
- Deleted from the player: core/audio.hpp's `PcmOutput`, desktop/sdl_pcm_output.*,
  boards/korvo1/main/{speaker_output,bt_*}.*, core/ring_tap.hpp (moved), tests' FakeOutput.
- Player tests drive `ManualDevice` instead of FakeOutput; assertions keep their meaning.

## Constraints

- C++26; core headers SDL-free; non-template code in src/; never a bare `detail` namespace.
- PRISM builds for ESP-IDF with `-Dsdl=disabled -Dfreertos_port=disabled`: there the SDL backend
  is absent and prism-esp32 supplies the implementation. The `ManualDevice` and `convert` build
  everywhere.
- prism-esp32's ` D subprojects/prism` and `?? host/subprojects/.wraplock` are never committed.
- Builds and tests on the Coder workspace `prism`, one per build directory.

## Testing

- PRISM: `convert` both ways (rounding, saturation at ±1.0 and beyond, gain); a native-type render
  function receives the device's buffer (no conversion pass); `ManualDevice` pull / lose / state;
  the timer only for the default device; no allocation on the rendering thread for both sample
  types (extend the existing test); existing float tests unchanged.
- prism-esp32: `RenderGate` with its ThreadSanitizer test on the host; the board backends are
  tried on the board (speaker plays at 44.1 and 48 kHz; headset connects, plays, pauses, lost on
  power-off, reconnects, buttons work).
- Player: the full suite on `ManualDevice`, then the Korvo build tried on the board.

## Order

1. PRISM: sample types, `convert`, the two-type `Output`, `devices()`/`DeviceInfo`, `state()`,
   `set_volume`, `ManualDevice`; the SDL backend and its tests updated. Synth rack unchanged.
2. prism-esp32: speaker device, `RenderGate`, Bluetooth link + headset device (moved from the
   player). Its own smoke app plays a tone on each device.
3. prism-mp3-player: switch to `audio::Output`, delete the old outputs, bump both submodules.

## Left to the builder

- Exact file names and the component split inside prism-esp32.
- Whether `ManualDevice` lives in `prism::audio` or a `prism::audio::testing` namespace.
- SDL device-removed detection details.
- How the board's `devices()` decides the headset is listed (always when Bluetooth is built,
  or only once bonded).
