# XOVI Content Inserter

`xovi-content-inserter` receives text or a device-local image path through
`xovi-message-broker` and inserts the content into the page currently open in
xochitl.

## API

Read capabilities first:

```sh
timeout 10 cat /run/xovi-mb-out &
printf '%s\n' '>exovi-content-inserter$capabilities:' > /run/xovi-mb
```

`currentPageAvailable` is `true` only when a notebook, PDF, or ebook page is
loaded and visible. The extension follows xochitl's active `DocumentView`
Loader and its Qt Quick visual item tree; it deliberately does not select a
hidden preview or a controller retained from a previously opened document. A
library, settings, sleep, or loading screen therefore reports `false`.

All write requests use `xovi-content-inserter$put` with a JSON object. The
broker command, signal name, separator, and JSON must fit within the broker's
1024-byte request limit.

### Insert typed text into the current page

```sh
timeout 10 cat /run/xovi-mb-out &
printf '%s\n' '>exovi-content-inserter$put:{"target":"current-page","type":"text","text":"Meeting notes","coordinateSpace":"normalized","x":0.2,"y":0.3}' > /run/xovi-mb
```

Normalized text coordinates use the initial logical notebook page bounds: `(0,0)` is the
top-left and `(1,1)` is the bottom-right. `coordinateSpace:"scene"` accepts
xochitl scene coordinates directly. If the page has no typed-text root, the
extension asks `SceneController` to create one before focusing and pasting.
The same operation accepts `"path":"/tmp/notes.txt"` in place of the inline
`text` field.

Every text request must contain exactly one of `text` or `path`. UTF-8 files
with or without a BOM are supported; an empty file or invalid UTF-8 produces a
structured error response.

### Native placement geometry

Capabilities include `scenePage.bounds` (`x`, `y`, `width`, `height`) and
`scenePage.boundsSource`. Native insertion and preview use the active scene
view’s `sceneExteriorBoundary` (also exposed as `exteriorBoundary`). Only a
visible view bound to the active page controller is accepted. On older firmware
without these properties, the union of `paperNoteBounds`, `defaultNoteBounds`
and the content `boundingRect` is used, so extended content is not excluded.
The initial `paperNoteBounds` alone may end above the actual page bottom.
The `canvas` field
is the screen canvas used for editable ink; it is not the native scene paper.
Native previews should use the aspect ratio of `scenePage.bounds` when present.
The native insertion response also reports `geometry` and the mapped
`scenePosition`. These describe the requested insertion point, not the final
asynchronously created image item's extent or bottom-edge alignment.

### Insert a native image into the current page

```sh
timeout 30 cat /run/xovi-mb-out &
printf '%s\n' '>exovi-content-inserter$put:{"target":"current-page","type":"image","path":"/tmp/logo.png","representation":"scene-image","coordinateSpace":"normalized","x":0.5,"y":0.5}' > /run/xovi-mb
```

`scene-image` is the default. It calls xochitl's native
`SceneController.insertImageFileAsSceneItem()` API, so the result is a native
image SceneItem that can be moved and resized with the selection tool. Its
coordinates are the insertion point in normalized full-page scene bounds or
direct scene coordinates. The bottom edge maps to `bounds.y + bounds.height`;
this positions the insertion point, not the bottom edge of the resulting image. The native API chooses the initial image size.

The request's `representation` selects the mode; image content or format does
not select it automatically. A failed `scene-image` request returns an error
and does not fall back to pen-stroke tracing. Native insertion creates an image
object; byte-for-byte preservation of the source file is not guaranteed by this
extension.

To trace the image into editable pen strokes instead, explicitly request
`"representation":"editable-ink"`. This mode injects horizontal strokes
through the marker input device and uses the currently selected writing tool;
do not touch the marker while it is running.

Editable-ink options:

| Field | Default | Meaning |
|---|---:|---|
| `representation` | `scene-image` | Set `editable-ink` to enable tracing options below |
| `coordinateSpace` | `normalized` | Fractions of canvas width/height, or `screen` pixels |
| `x`, `y` | `0.1`, `0.1` | Top-left position |
| `width` | `0.8` normalized; source width in screen mode | Output width |
| `height` | preserve aspect ratio when omitted | Output height |
| `center` | `false` | Only in normalized mode: overrides `x`/`y` to center the image; scales both dimensions down if height exceeds 80% of the canvas |
| `threshold` | `150` | Range 0–255; normally draws pixels with grayscale ≤ threshold; increasing it includes lighter pixels |
| `alphaThreshold` | `32` | Range 0–255; pixels with alpha below this value are ignored |
| `invert` | `false` | Draw light pixels instead of dark pixels |
| `rowStep` | `2` | Range 1–16; sample every Nth row; smaller values produce denser tracing and usually take longer |
| `sampleLimit` | `384` | Range 32–768; longest side of the sampling raster; larger values allow more detail and usually take longer |
| `maxRuns` | `20000` | Range 100–50000; maximum horizontal stroke runs; exceeding this rejects the request before any strokes are injected |
| `canvasWidth`, `canvasHeight` | detected canvas size | Optional positive dimensions overriding the coordinate-mapping canvas; normally omit these |
| `inputDevice` | auto | Restricted override such as `/dev/input/event2` |

The five integer options with ranges above are clamped to those ranges. The
output rectangle must fit entirely within the canvas. `sampleLimit` controls
sampling detail, while `width`/`height` control placement size.

Pen type, color and thickness come from the currently selected xochitl writing
tool; there are no separate request parameters for them. Pressure and event
timing are also not configurable through the API. The implementation currently
injects a fixed pressure value of 2630 and uses fixed delays around each stroke;
changing sampling density affects total work, not a configurable pen speed.

Example request for denser, centered tracing (potentially slower):

```json
{
  "target": "current-page",
  "type": "image",
  "path": "/tmp/picture.png",
  "representation": "editable-ink",
  "coordinateSpace": "normalized",
  "center": true,
  "width": 0.7,
  "sampleLimit": 512,
  "rowStep": 1,
  "threshold": 150
}
```

## Responses and safety

Every API returns one compact JSON object. Errors use:

```json
{"ok":false,"error":"no-current-page","message":"no active notebook SceneController was found"}
```

The extension refuses image injection when no active page is detected, when
the target rectangle is outside the canvas, or when the thresholded image is
too complex. Only one input injection can run at a time.

Native typed-text insertion uses private xochitl QML APIs. Capability discovery
can verify that an active controller exists, but a firmware update may still
change those private methods. Image-to-ink input injection follows the proven
evdev approach used by
[`smart_remarkable`](https://github.com/yangg1224/smart_remarkable). Directly
rewriting a live `.rm` file was deliberately avoided: append/rewrite tools such
as [`remarkable-mcp`](https://github.com/SamMorrowDrums/remarkable-mcp) operate
on document storage rather than the currently open xochitl scene and can lose
unsupported scene data when performing a full rewrite.

## Limitations / TODO

- Clipboard transfer is currently unsupported for both text and images. The
  API accepts only `target:"current-page"`; `target:"clipboard"` returns
  `invalid-target`. Use direct page insertion for now.
- TODO: implement and verify clipboard copy/paste in xochitl before documenting
  clipboard transfer as a supported feature.

## Copyright and License

Copyright © 2026 NFJ / Nuanfengjia, for original contributions.

Licensed under GPL-3.0-only. See [LICENSE](LICENSE) for the full license text.
Third-party code and contributions remain copyrighted by their respective
copyright holders and retain their existing copyright and license notices.
