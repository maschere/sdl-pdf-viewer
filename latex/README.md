# GIF/APNG attachments for SDL PDF Viewer

Copy `sdlpdf-animation.sty` to your LaTeX project and load it in the preamble:

```tex
\usepackage{sdlpdf-animation}
```

Replace the separate black rule and empty attachment with one command:

```tex
\fbox{\pdfanimation[fps=12,autoplay=false,interpolation=nearest]
  {5cm}{5cm}{img/yummyslime.gif}}

\fbox{\pdfanimation[fps=24,autoplay=true,interpolation=bicubic]
  {5cm}{5cm}{img/acid_arrow.apng}}
```

`\pdfanimation[options]{width}{height}{file}` reserves an invisible box of the
exact width and height and embeds the GIF/APNG in its PDF file-attachment
annotation. The viewer scales the entire source canvas to that box, including
any transparent source padding. The surrounding `\fbox` is optional and adds
its normal border and padding outside the specified dimensions. Do not put a
black `\rule` underneath the command.

Missing files produce a package error naming the missing attachment, before
calling `attachfile2`. Use `-halt-on-error` to stop compilation on that error.

| Option | Default | Meaning |
| --- | --- | --- |
| `fps` | `24` | Fixed source frames per second, from `0.1` to `240`; fractional values such as `29.97` work. |
| `autoplay` | `false` | Show frame 1 until play is clicked; `true` starts on entering the slide. |
| `interpolation` | `nearest` | `nearest` preserves pixel-art edges; `bicubic` uses smooth Catmull-Rom interpolation with alpha-aware filtering. |

Playback loops until paused. The chosen FPS replaces the animation's native
frame delays. Each animation has its own small play/pause control at the bottom
center of its box. Resize and ink clearing preserve playback. Returning to a
slide restarts its animations according to their autoplay settings.

The command is compatible with XeLaTeX and `attachfile2`. It does not require
shell escape, external video files, JavaScript, position-tracking auxiliary files
or a second pass. Other viewers retain access to the ordinary attached file.

From the repository root, compile the example using the included tiny fixtures:

```powershell
xelatex -output-directory=build latex/animation-example.tex
.\build\Release\sdl-pdf-viewer.exe --windowed build\animation-example.pdf
```

To override all embedded FPS values while testing:

```powershell
.\build\Release\sdl-pdf-viewer.exe --fps 15 presentation.pdf
```

The viewer also recognizes legacy empty `\textattachfile` annotations next to
black rectangular rule placeholders, including slides 2 and 3 of the supplied
test presentation. That inference cannot describe arbitrary layouts reliably;
the command above supplies exact coordinates and per-animation options.

## PDF metadata convention

The file-attachment annotation's `/Rect` is the animation box in PDF page
coordinates. `/FS` refers to its embedded file stream using `/EF /F` (or `/UF`).
The annotation's `/Subj` is an ASCII or UTF-16BE PDF string such as:

```text
SDLAnim fps=24 autoplay=false interpolation=nearest
```

The viewer validates these values. The package disables the extra strut normally
added by `attachfile2`, so the annotation measures exactly `width` by `height`.
No filename is opened by the viewer: only the embedded bytes are decoded.
