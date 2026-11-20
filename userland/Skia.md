# Skia on Omi

Skia's CPU backend builds against the musl target and rasterises correctly on
Omi. The proof is a pixel-and-hash comparison, not "it linked": `userland/skiahello.cpp`
draws a gradient, an antialiased rounded rect and a stroked star, reads the
pixels back, checks the two colours it expects, then PNG-encodes the surface.

```
[skia] box=fff03c3c corner=ff15205b
[skia] png=8181 bytes crc=f7ac5947
[skia] 240x160 ok
[ring3] task skia exited with code 0x0000000000000000
```

The same binary run on Linux/musl prints the same three lines, PNG CRC included -
Skia's raster pipeline is deterministic across the two, which makes that CRC a
usable regression check.

## Build

Skia's gn expects a single executable in `cc`/`cxx`/`ar`, so cross compiling goes
through three tiny wrappers in `~/opt/zig/` (`musl-cc`, `musl-cxx`, `musl-ar`).
`musl-cc` also rewrites `-march=x86-64-vN` to `-mcpu=x86_64_vN`: Skia's gn emits
the gcc/clang spelling, zig does not accept it.

```
cd ~/skia
gn gen out/musl --args="$(tr '\n' ' ' < args.musl.gn)"
~/opt/ninja/ninja -C out/musl skia -j3      # ~10 min, 265 targets
```

`args.musl.gn` is the whole story: `cc`/`cxx`/`ar` wrappers, `is_official_build`,
CPU raster only, every `skia_use_system_*` off so Skia's bundled third_party is
used, and the features that pull in a desktop stack off:

- **`skia_use_partition_alloc = false`** - it includes `sys/cdefs.h`, a glibc
  header musl does not ship. This is not a shim you can paper over; switch it off.
- **`skia_use_icu = false`, `skia_use_fontconfig = false`, `skia_use_freetype = false`** -
  no font stack yet, so no text. That is the next thing Skia needs on Omi, not a
  build problem.
- **`skia_use_libwebp_* = false`** - the include path for bundled libwebp is not
  wired up in this configuration; it fails on `webp/decode.h`. Off until it matters.
- **`skia_enable_gpu = false`, `skia_use_gl/vulkan/metal = false`, `skia_enable_pdf = false`,
  `skia_enable_sksl = false`** - CPU raster only.

Output: `out/musl/libskia.a`, 231 MB.

## Linking

The embedder links the monolith plus the codec libs it still needs, all static:

```
zig c++ -target x86_64-linux-musl -std=c++20 -static -O2 -no-pie -I ~/skia \
  -o skiahello.elf skiahello.cpp \
  ~/skia/out/musl/libskia.a ~/skia/out/musl/libpng.a ~/skia/out/musl/libzlib.a \
  ~/skia/out/musl/libjpeg.a ~/skia/out/musl/libskcms.a ~/skia/out/musl/libwuffs.a \
  -lpthread -lm
```

60 MB static ELF. The ISO is 102 MB with it, which is why `RFILE_MAX` matters -
see `docs/linux-abi.md`.

## API notes

This Skia is post-refactor and the old recipes do not compile:

- `SkSurface::MakeRaster(info)` -> `SkSurfaces::Raster(info)`
- `SkPath` has no `moveTo`/`lineTo` -> `SkPathBuilder`, then `.detach()`
- `SkGradientShader::MakeLinear(pts, colors, pos, count, mode)` ->
  `SkShaders::LinearGradient(pts, SkGradient(SkGradient::Colors(span, mode), SkGradient::Interpolation{}))`
  (colours are `SkColor4f`, not `SkColor`)
- `SkCanvas::flush()` is gone; a raster surface draws immediately.

## Text

Shapes are not a browser. Text is, and text needs a font stack, so Skia is now built
with **FreeType** inside it (`skia_use_freetype = true`, system FreeType off, so the
copy that ships is the one in `third_party/externals/freetype`). Fontconfig stays off:
Omi has no fontconfig and ring 3 cannot enumerate a directory, so a program opens the
face it wants and hands FreeType the bytes:

    SkFontMgr_New_Custom_Empty()            -> a manager with no searching in it
    SkData::MakeFromFileName("/fonts/...")  -> the TTF, straight off the ISO
    mgr->makeFromData(data, 0)              -> a typeface, parsed by FreeType
    SkFont(face, 40) + canvas->drawString() -> glyphs in the surface

`skiatext.cpp` is the proof, and it is a *number*: the same static binary runs on
Linux and on Omi, and both print the same four values.

```
Linux                                    Omi
advance=208.00 bounds=206.00x30.00       advance=208.00 bounds=206.00x30.00
ink=4383 of 54000 px                     ink=4383 of 54000 px
surface crc=a5e72215                     surface crc=a5e72215
png=7607 bytes crc=8b408c86              png=7607 bytes crc=8b408c86
```

Nothing in that is a tolerance. FreeType is deterministic and Skia's raster pipeline is
deterministic, so two machines that get the same pixels produce the same hash; when they
stop, one of them is guessing. The face reports `Bricolage Grotesque`, 597 glyphs, 1000
units per em, and the mono line comes from `JBMono.ttf` through the same manager, so both
faces are exercised.

What is still missing for real web text: **HarfBuzz** (no shaping - Arabic, Devanagari and
ligatures will be wrong), **ICU** (no line breaking or bidi), and a font *matching* story
(today a program names its face; a browser has to pick one from a family name).

## On screen

`skiawin.cpp` points the same pipeline at a real window: `SYS_WIN_OPEN`, then
`SYS_SURF_CREATE` for the window's pixel buffer, then `SkSurfaces::WrapPixels` over
that buffer. So the chain under test is

    SkCanvas -> Skia's raster pipeline -> the window's shared pages
             -> SYS_SURF_PRESENT -> the compositor -> the framebuffer

and it is checked at both ends. Inside: read the buffer back at coordinates Skia was
told to paint and compare the colours. Outside: a QEMU framebuffer dump has to
contain the one colour nothing else on that screen draws - a four-pixel neon-green
border - in the right place.

```
[skiawin] ok   a window opens
[skiawin] ok   its pixels are a user mapping
[skiawin] ok   Skia painted into the window
[skiawin] border=ff39ff14/ff39ff14 block=ff8a2be2
[skiawin] ok   the marker border is in the window buffer
[skiawin] ok   a filled block is in the window buffer
[skiawin] ok   the window moves itself out of the cascade
[skiawin] placed at 560,31 400x291 - the marker border is at x=560..959, y=31..321
[skiawin] ok   the surface presents
```

Measured in the dump: the green border occupies exactly `(560,31)-(959,290)` - 400x260,
the size of the surface - and the blueviolet block sits at `(590,51)`, which is the
window origin plus the 30,20 Skia was given. Colours match bit for bit, because Skia's
N32 is BGRA in memory and that is the word Omi's surfaces carry; had that been wrong
the red and blue channels would have come back swapped and the checks would say so.

Two API traps worth knowing, both found by this program:

- **`SYS_WIN_RECT` is (id, x|y<<32, -, (w<<16)|h)** - x and y share one argument and
  the size rides in the *fourth*, which the header comment had wrong. Omi's `int $0x80`
  door also carries that fourth argument in **r8**, not `r10` where the Linux ABI puts
  it; a caller using r10 gets a resize to zero and the compositor's clamps (120 wide,
  screen - 120 tall) instead of an error.
- **A window's outer height includes the title bar.** A surface of exactly `(W,H)` in a
  window opened at `(W,H)` gets its bottom 31 rows clipped. Open at `H + 31`, or measure
  the client area, but do not assume the surface fits.

## Where this goes next

The engine now draws into memory. Step 7's surface API is the other half: blitting
that surface to Omi's framebuffer is what turns "Skia runs" into "Skia is on
screen", and it is the first thing a browser needs from it. After that, the font
stack (`skia_use_freetype = true` with bundled freetype) because a browser without
text is not a browser.
