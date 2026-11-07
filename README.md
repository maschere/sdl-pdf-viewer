# SDL PDF Viewer

A **C11-only application** using **SDL 3.4.20** for borderless fullscreen,
keyboard/mouse input, a visible pointer, and browser launching. The PDF parser,
content interpreter, scanline rasterizer, image placement, link resolver and ink
smoothing are implemented here from scratch. The application has no macOS PDF APIs,
Objective-C, Swift, C++, PDFium, MuPDF or Poppler dependencies. SDL's own platform
backends are upstream code and may use native implementation languages.

## Build on Apple silicon

Install Xcode Command Line Tools if needed (`xcode-select --install`), then:

```sh
brew install cmake jpeg-turbo zlib giflib libpng
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix jpeg-turbo);$(brew --prefix zlib)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/sdl-pdf-viewer tests/fixtures/demo.pdf
```

CMake downloads the exact SDL **3.4.20** source archive and verifies its SHA-256.
zlib, libjpeg/libjpeg-turbo, giflib and libpng are used only for stream and image
decoding; the PDF renderer itself is application code. All are C libraries and
must be installed first. SDL is
linked statically; codec libraries may be dynamically linked. This builds a
command-line executable, not a signed/notarized `.app` bundle.

## Linux and Windows

The application and renderer use the same portable C code on all platforms.
On Debian/Ubuntu, install `build-essential cmake libjpeg-dev zlib1g-dev libgif-dev libpng-dev` and SDL's
desktop development dependencies (listed in `.github/workflows/build.yml`), then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/sdl-pdf-viewer file.pdf
```

On Windows, install Visual Studio's C compiler, CMake, and vcpkg. Install
`libjpeg-turbo:x64-windows zlib:x64-windows giflib:x64-windows libpng:x64-windows`
with vcpkg, configure with its
`CMAKE_TOOLCHAIN_FILE`, and build with `--config Release`. The executable is
`build/Release/sdl-pdf-viewer.exe`; place the codec DLLs beside it if necessary.
The Windows runtime folder needs `jpeg62.dll`, `z.dll`, `gif.dll` and
`libpng16.dll` when using the dynamic vcpkg triplet; vcpkg's CMake integration
copies them beside the executables automatically. SDL remains statically linked.
The included GitHub Actions workflow configures builds/tests for all three
platforms. That workflow is supplied for running on your repository; its remote
jobs have not been executed as part of this delivery.

For an offline build, unpack the verified SDL release locally and pass
`-DFETCHCONTENT_SOURCE_DIR_SDL3=/absolute/path/to/SDL3-3.4.20` to CMake.

## Controls

| Action | Input |
| --- | --- |
| Next page | Right, Down, Page Down, Space, N |
| Previous page | Left, Up, Page Up, Backspace, P |
| First / last page | Home / End |
| Draw smooth red ink | Hold left or right mouse button and drag |
| Follow a PDF link | Left-click without dragging |
| Draw a dot | Click a blank area, or right-click |
| Play / pause an animation | Click its small bottom-center icon |
| Clear current ink | C or Delete |
| Toggle fullscreen | F |
| Quit | Escape or Q |

A four-window-coordinate-pixel drag threshold distinguishes link clicks from
strokes. Links show a hand cursor. Ink uses midpoint quadratic smoothing,
converted to cubic curves and adaptively flattened. Both taps and strokes use a
7-output-pixel-diameter red brush: the inner 70% of its radius is solid, with a
smooth cubic fade through the outer 30% and a pixel-filtered edge. Coverage is
combined within each stroke so overlapping segments do not darken its feather.
The diameter is measured in physical output pixels, including on high-DPI displays.
Ink is clipped to the current page. Ink is discarded on navigation,
resize or fullscreen transitions, and is never written to the PDF. Returning to
a page shows the clean page. The file is opened read-only.

Pages are fitted and letterboxed without changing the display resolution.
Mouse coordinates are converted through SDL's high-DPI coordinate conversion
and the inverse PDF page transform, including crop origins and rotation.

```sh
./build/sdl-pdf-viewer --windowed file.pdf
./build/sdl-pdf-viewer --check file.pdf
./build/sdl-pdf-viewer --render page.ppm --page 2 file.pdf
```

`--page` is 1-based. `--check` validates the whole document without initializing
SDL video. `--render` likewise works without a display and exports a 1600×1000
PPM image of the chosen page. Text outlines, filled shapes, strokes and path
clips use coverage antialiasing at native output resolution, including in
headless exports. The scan converter uses eight vertical subpixel samples,
analytic horizontal coverage and an active edge list; solid spans are accumulated
with row differences. Stroke segments, caps and joins are filled together to
avoid overlap seams. There is no resolution-dependent antialiasing cutoff or
full-frame supersampling/downsampling pass. Pen updates touch only the narrow
band around each curve segment.

## In-place GIF and APNG animations

Embedded GIF/APNG file attachments play directly within their reserved slide
boxes. Animations start paused on frame 1; click the small play/pause icon at
the bottom center of the box. Playback loops while playing. Each frame advances
at the specified fixed FPS (0.1 to 240), overriding the file's original frame
delays. Multiple animations have independent controls. Resizing and clearing ink
preserve playback; leaving a slide and returning starts its animations again
according to their autoplay settings. Ink appears above the animations.

Copy [latex/sdlpdf-animation.sty](latex/sdlpdf-animation.sty) beside your TeX source:

```tex
\usepackage{sdlpdf-animation}

% In the document/frame; the dimensions describe the animation itself.
\fbox{\pdfanimation[fps=12]{5cm}{5cm}{img/yummyslime.gif}}
\fbox{\pdfanimation[fps=24,autoplay=true,interpolation=bicubic]
  {5cm}{5cm}{img/acid_arrow.apng}}
```

The command is `\pdfanimation[options]{width}{height}{file}`. Defaults are
`fps=24,autoplay=false,interpolation=nearest`. Choose `nearest` for crisp pixel
art or `bicubic` for smooth interpolation. The entire image, including transparent
padding in the source, is scaled to the exact supplied width and height. The
surrounding `\fbox` and its padding are outside those dimensions. A frame is
uploaded only when it changes; bicubic kernels and intermediate storage are
reused. Nearest-neighbor scaling uses SDL's texture sampler.

The package uses `attachfile2` and works with XeLaTeX. Its annotation rectangle
records the placement directly, and its subject records the options. It needs
no position files or second TeX pass. Other PDF readers see an ordinary file
attachment. See [latex/README.md](latex/README.md) for details and the compiled
example source [latex/animation-example.tex](latex/animation-example.tex).

Headless `--render` exports the first frames without playback controls. Files are
decoded from their embedded bytes; the viewer does not open attachment filenames
or launch external players. GIF transparency, local palettes, interlacing and
restore-background/restore-previous disposal are supported. APNG supports frame
offsets, SOURCE/OVER blending, all three disposal modes and a separate default
poster image. libpng decodes the reconstructed frame PNGs; no APNG-patched libpng
is required. The [PNG animation specification](https://www.w3.org/TR/png-3/#11AnimationChunks)
and [giflib documentation](https://giflib.sourceforge.net/gif_lib.html) describe
the underlying formats and decoder API.

Decoded animation frame storage is limited to 128 MiB per PDF, 4096 frames per
animation and 16 million pixels per frame. Temporary decode and bicubic working
buffers are additional. Invalid/truncated data, invalid frame bounds and APNG
CRC/sequence errors are rejected.


## Supported PDF subset

| Feature | Support |
| --- | --- |
| File structure | Unencrypted PDF with classic xref tables or xref streams, compressed object streams, generation-aware references |
| Pages | Inherited MediaBox, CropBox, Resources and Rotate; rotations 0/90/180/270 |
| Streams | Uncompressed or FlateDecode; content stream arrays share graphics state |
| Paths | Moves, lines, cubic curves, rectangles, closed/open paths |
| Painting | Nonzero/even-odd fills; solid strokes, butt/round/square caps, miter/round/bevel joins |
| Transforms | Affine transforms for fills, glyphs, forms and images; uniform scale/rotation/reflection for strokes |
| Colors | DeviceGray, DeviceRGB; interpolated default DeviceCMYK conversion for vector paint |
| Graphics state | q/Q, widths, caps, joins, miter limit; rectangular and arbitrary path clipping |
| Text | Embedded CFF Type 2 outlines; CIDFontType0/Identity-H and simple Type1C encodings; PDF widths, text matrices and TJ adjustments |
| Forms | Nested Form XObjects with resources, Matrix and BBox |
| Gradients | Axial and concentric growing radial shadings; exponential and stitching functions |
| Transparency | Constant fill/stroke alpha and luminosity Form soft masks |
| Images | Image XObjects, raw/Flate/JPEG, DeviceGray/DeviceRGB/Indexed and D65 CalRGB |
| Sample depth | Gray/Indexed 1, 2, 4, 8 bits; RGB 8 bits per component |
| Image predictors | TIFF predictor 2 for 8-bit data; PNG predictors 10–15 |
| Image masks | DeviceGray SMask alpha, including Decode inversion; resampled to image dimensions |
| Image Decode | Per-component ranges, including inverted samples |
| Links | Direct Dest, GoTo actions, legacy destination dictionaries, destination name trees |
| Browser links | URI actions for HTTP/HTTPS through SDL_OpenURL |

Images use nearest-neighbor sampling. CFF hints are parsed but no grid fitting is
applied. DeviceCMYK uses a sampled default output conversion table, interpolated
in portable C. It is an explicit approximation, not document-specific ICC color
management. D65 CalRGB images are converted through their gamma/matrix into sRGB.
The optional `tools/make_cmyk_table.py` can regenerate the numeric color samples;
it uses a development-time reference library, not a runtime PDF dependency.
Destination view modes (Fit, XYZ, etc.) navigate to the correct page and fit that
whole page. Link hit testing uses the annotation Rect; QuadPoints and annotation
appearance streams are not rendered. Non-navigation actions and additional action
chains are ignored. Links do not trigger JavaScript, file launch, embedded-file
opening, or remote-PDF navigation.

Unsupported features still include TrueType/OpenType font programs, Type 1
eexec programs, non-Identity composite encodings, simple-font encoding overrides,
inline images, CMYK/ICC/Separation images, stencil/color-key masks, Matte images,
non-normal blend modes, general transparency group isolation/knockout, arbitrary
radial circle arrangements, dashed strokes, patterns, encryption, hybrid xrefs
and incremental updates. Font clipping modes, CFF arithmetic/flex operators and
some predefined font encodings are also outside the current subset. 

Limits include 64 MiB input/decoded streams, 256 MiB PDF allocation budget,
100,000 object slots, 10,000 pages, bounded nesting, 16 million pixels per source
image, and 64 million samples per render surface. A from-scratch subset parser is
not a replacement for a mature, hardened PDF engine for arbitrary untrusted files.

## Tests and source layout

```sh
# Renderer-only tests, without downloading/building SDL:
cmake -S . -B build-core -DSDL_PDF_VIEWER_CORE_ONLY=ON
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure

# Regenerate the included PDF fixtures; Python is not needed to build/run:
python3 tests/make_fixtures.py
python3 tests/make_animation_fixtures.py
```

- `src/pdf.c`: tokenizer, objects/xrefs, page tree, interpreter, font integration,
  forms, gradients, image codecs, predictors, masks and destination/link resolution.
- `src/cff.c`: CFF indexes, dictionaries, charsets, encodings, CID maps and Type 2 outlines.
- `src/cmyk_table.h`: sampled numeric data for default DeviceCMYK conversion.
- `src/raster.c`: affine math, path flattening, coverage scanline fills, stroke unions and images.
- `src/brush.c`: pixel-filtered soft brush and per-stroke coverage compositing.
- `src/animation.c`: giflib/libpng decoding, GIF/APNG compositing, FPS clock and
  cached bicubic resampling.
- `src/ink.h`: portable midpoint smoothing and drag threshold.
- `src/main.c`: SDL event loop, fullscreen, high-DPI input, temporary ink, CLI.
- `tests/`: pixel assertions, image orientation/predictor checks, destination
  resolution, cropped/rotated page transforms, malformed input rejection,
  repeated rendering, subpixel coverage/soft-brush checks, and an SDL dummy-driver
  interaction test. `benchmark_render file.pdf width height repetitions` measures
  CPU page rendering without disk output or SDL upload.

Windows smoothing validation: all five CTest suites pass in the x64 Release build,
including small CFF text, fractional fills/clips, translucent stroke joins, taps,
and event-spacing-independent soft strokes. The four renderer-only suites also
pass with MSVC AddressSanitizer. Rendered shape edges and brush output were
visually inspected. On this Windows machine, 30-render
runs at 1280×800 took approximately 7.8 ms/page for the demo, 11.1 ms/page for
vectors and 4.2 ms/page for the CFF fixture. The previous renderer at the viewer's
2× working resolution took 25.0, 19.8 and 13.9 ms/page respectively, excluding its
additional downsampling pass. These are fixture measurements, not a guarantee
for arbitrary documents.

Original delivery validation: Linux build against SDL 3.4.20;
the original four CTest suites and the 116-page acceptance test; headless validation/rendering of the demo; visual inspection
of rendered output; and AddressSanitizer/UndefinedBehaviorSanitizer on the renderer
tests and the attached 116-page PDF. LeakSanitizer could not run under the execution environment's tracing.
Apple silicon compilation, real macOS fullscreen/Retina behavior, physical mouse
input, and system-browser launching remain to be verified on a Mac.

