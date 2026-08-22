# Stardom3 widescreen UI layout strategy

The game renders its 3D scene natively at 16:9 while most GUI resources still
assume an 800x600 canvas. Widescreen layout therefore changes **positions by
anchor** and keeps texture-backed controls at their native size by default.

## Coordinate model

The original canvas centre is `(400, 300)`. For a control with centre
`(cx, cy)`, proportional position-only mapping is:

```text
u = (cx - 400) / 400
v = (cy - 300) / 300
targetCx = outputWidth  / 2 + u * usableHalfWidth
targetCy = outputHeight / 2 + v * usableHalfHeight
```

The control width and height are unchanged. Optional UI-size presets can be
added separately, without coupling size to screen resolution.

## Anchor families

| Family | Rule | Examples |
| --- | --- | --- |
| top-right HUD | preserve top/right margins | `TodayDate` |
| bottom-right HUD | preserve bottom/right margins | `GameMain`, `GameShort`, all toolbar sublayers |
| modal | centre on the output | dialogs and confirmation windows |
| edge panel | preserve distance to the selected edge | left/right information panels |
| world-following | do not remap | speech bubbles, character markers, floating labels |
| full-screen | resize or provide a dedicated widescreen background | fades, menus, minigames |

All members of one functional family must share a layout group. Moving only a
parent form name is insufficient because Stardom3 creates some sublayers as
independent root forms.

World-following controls use a runtime coordinate-space registry. Known world
structures (location labels, interaction controls, compact status tags and
speech bubbles) seed the registry, and the GUI move hook remembers both the
object and the upstream move source. Later moves from that source bypass legacy
anchoring even when the projected point happens to be numerically inside the
old 800x600 canvas. Source matching is a last-resort fallback after all known
screen-space surfaces, so a shared caller cannot override a schedule panel or
toolbar's specialized transform. Compact early-game toolbars use their native
six-item 275px width, while the temporary five-item construction state and the
later seven-item toolbar use 230px and 320px respectively. Object identity is retained when auto-sized
tags change width with their text.

The title tutorial reuses the same 165x100, arrow-plus-text speech-bubble
structure as projected NPC dialogue, but its native `(320,60)` coordinate is
relative to an 800x600 tutorial page rather than the 3D viewport. The tutorial
overlay's unique pair of 400x530 stage layers, central prompt, lower explanation
layer, and compact top-right control identifies this context. The page and its descendants use
the same 4:3 aspect-fit transform as the title screen; at 1920x1080 the page
becomes `(240,0) 1440x1080`, while the detached dialogue bubble becomes
`(816,108) 297x180`. Child rectangles and hit targets are transformed with
their owning root. Ordinary NPC bubbles continue to use final world-projection
pixels unchanged.

Tutorial questions are created later as a separate direct-root 800x600 page,
not as descendants of the tutorial artwork. Its structural signature is three
200x90 answer buttons at native x positions 50, 300, and 550 plus one 464x151
question panel. That complete page receives the tutorial's same aspect-fit
transform, keeping the outer answers aligned with the three enlarged arrow
decorations and scaling the labels and hit targets together.

The birthday month/day and blood-type lists are also detached direct-root
objects. Each list has five 25px rows and two 19x29 scroll arrows, with native
widths of 70, 60, and 80 pixels respectively. This seven-child signature keeps
them out of the generic bottom-edge anchoring path: their native coordinates
receive only the centered 800x600 canvas offset, so a popup created at
`(268,434)` lands at `(828,674)` on 1920x1080 instead of being pushed down to
`(828,914)`.

The render clip used by the title screen remains active while the tutorial
root is visible. Every frame clears the area outside `(240,0) 1440x1080` and
clips tutorial rendering to that rectangle, preventing the title animation or
cursor trails from leaking into the pillarboxes.

The scene-transition curtain is distinguished from centered legacy pages by
structure: it is a direct child of the primary root, sits at the origin, is
approximately 802x602, and has no descendants. This leaf surface is resized to
the complete output resolution. Legacy pages with child controls keep their
native 800x600 size and centered placement.

The title screen is recognized by its seven 100x100 menu buttons, version
label, paired 800x600 background layers, and paired animated middle strips.
It is uniformly enlarged into the same centered 4:3 viewport used by other
aspect-sensitive full-page scenes. The entire subtree, including button hit
rectangles, is scaled together; unrelated 800x600 menus retain the ordinary
centered-page behavior. On a 1920x1080 output the title occupies 1440x1080 at
x=240. Title rendering is scissored to that viewport because the original
page contains two 2000-pixel looping animation strips; after uniform scaling
those strips would otherwise leak character silhouettes and scan lines into
the side wings. Clipping the complete title pass keeps the animation intact
inside the authored canvas. The pillarbox rectangles are also cleared to black
at the start of every title frame so pixels emitted before title detection do
not survive as stale scan lines in the side wings.
The detector accepts both mirrored menu arrangements used by the attract
animation. Cached native child dimensions are also reapplied when a character
cycle rebuilds or resets individual title layers, so the widescreen layout is
not limited to whichever animation frame happened to be visible first.
Title discovery is unthrottled until the root is found, preventing a native
800x600 frame from appearing before the aspect-fit layout.
The first frame completed before title discovery is masked at presentation time
and replaced by the following aspect-fit frame, eliminating the brief native
800x600 flash. The two animation strips retain the original controller's exact
native pair state: a render-frame clock reproduces its 30 Hz, two-pixel steps,
the primary wraps at +/-1000, and the follower is always derived as primary
/-2000 according to the primary coordinate's sign. The frame clock is necessary
because the original controller reads the enlarged object coordinate back as
its next native input. Each paired native coordinate is scaled once and written
together, preserving direction, strip identity, speed, separation, and hand-off
order without two independent motion loops.

The Star Photo Album is an exception to centered legacy pages. Its table and
book share one 800x600 viewport with its GUI page and hit rectangles. They are
scaled together with a single aspect-preserving factor and centered in the
output. At 1920x1080 the content becomes 1440x1080 at x=240, avoiding the
2.4x/1.8x anisotropic distortion. The page is recognized by its bottom 800x92
filmstrip, 355x445 book page, and 92x32 exit button; unrelated menus retain
native-size centered layout.

The portrait carousel's generic animation controller interpolates between an
already-scaled source position and a native-coordinate target. Its known
intermediate call site is therefore remapped by recovering animation progress
in that mixed space and applying it between consistently scaled endpoints;
settled layout calls use ordinary absolute scaling. The album viewport remains
latched across brief root-visibility gaps and is reasserted when returning to
the main render target and before draws, preventing a legacy 800x600 frame from
flashing while a character page is rebuilt.

Character selection also cross-fades through a textured 800x600 snapshot quad.
While the album session is active, that pre-transformed quad is remapped to the
same aspect-fit rectangle as the live album instead of being mistaken for an
announcement background. Announcement correction is gated by the actual
announcement-screen signature. Its 800x600 control root and result-card subtree
are mapped to the same aspect-fit viewport, while the renderer separately maps
the background and any in-canvas 360x200 preview quad. Preview positions are
left dynamic so the same path covers one through four artists without a slot
coordinate whitelist. Captions and stat deltas are
dynamic child controls: each later game-authored move is converted from native
coordinates instead of freezing the construction position or treating local
glyph vertices as full-canvas coordinates. The first caption, which the game
does not move after construction, is placed from the visible result-card count.
Cached announcement validation intentionally uses the four stable caption and
result-card sizes rather than their coordinates, because the game rearranges
all result cards after construction for each artist count.
Structural validity is also kept separate from visibility: the four-artist
sequence temporarily hides and restores the already-scaled root. The cached
root survives that transition, and discovery accepts either its native
800x600 geometry or its aspect-fit geometry.
At 1920x1080 the announcement
occupies 1440x1080 at x=240, while the uncovered side regions continue to show
the 3D scene.
The snapshot texture is captured from the complete widescreen output and thus
already contains the live viewport's pillar bars. Its U coordinates are cropped
to the viewport fraction before expansion, so those embedded bars are not
scaled into the transition and the snapshot content matches the live width.

The full-screen CG viewer exposes a dedicated visible canvas under the album
root. Narrative captions are submitted afterward as raw pre-transformed
800x600 primitives rather than GUI descendants. They retain their authored
pixel size and are translated as one centered 800x600 overlay canvas instead
of being scaled with the 1440x1080 CG image. This keeps the panel, borders, and
glyphs undistorted while preventing left-edge clipping; already-wide
coordinates are left unchanged.

Story CGs shown during normal gameplay use a different GUI path. The active
page is recognized as a visible direct-root 800x600 surface containing exactly
two childless 800x600 image frames. Both frames and their parent are enlarged
uniformly into the same centered 4:3 viewport (1440x1080 at x=240 on a
1920x1080 output). Immediately before the textured CG quad is submitted, the
two uncovered output rectangles are cleared to black so the previous 3D scene
cannot show through the 4:3 pillarbox area. The narration bar is a separate
800x76 root with one inset 780x60 text child; it uses the same native 800x600
overlay translation as the album CG caption. This keeps the image and caption
aligned without stretching the panel or glyphs and prevents repeated refreshes
from scaling the bar's already-translated y coordinate. The 800x76 caption is
also bound directly in the GUI move hook, before the periodic tree refresh, so
its first visible frame is normally centered. Some story events toggle the
surface visible by writing its coordinates directly and do not invoke that
move function. A lightweight CG-overlay refresh therefore runs at `BeginScene`
and before each draw family, independently of the one-second general tree
scan. It moves the retained GUI surfaces before their vertices are built and
does not inspect or alter unrelated pre-transformed effects. The structurally distinct
800x128 item-acquisition notice (380x60 memo plus 200x140 illustration) uses
the same native overlay translation; at 1920x1080 its authored `(0,316)`
position becomes `(560,556)`.

Other story-CG strips use a conservative generic fallback only when they are
visible direct children of the primary root, are approximately 800 pixels wide
and 32-180 pixels high, contain child controls, and still begin near the legacy
canvas x=0 edge. Matching strips are retained and translated every frame in
the native 800x600 coordinate system. Full-width strips nested inside an
already-centred 800x600 secondary page are deliberately excluded, preventing
double offsets and avoiding unrelated world-space effects.

Publication and award-list panels form a 509/510x350 direct-root family; the
one-pixel resource variation is intentionally accepted so all of their native
entrance frames bypass generic lower-right anchoring. Portrait publication
events use a separate 330x450 direct-root cover made of
three full-height image layers and one shorter header layer. They follow the
same rule as the older 510x350 newspaper panel: reflow translates the
controller's current origin once, then later controller-authored frames pass
through unchanged. The cached object identity also covers the construction
window in which the four image layers are not complete yet. This preserves the
original entrance animation without applying the generic lower-right anchor a
second time or depending on visibility, which is toggled only after the start
position has been submitted.

Schedule hover cards may arrive with mixed coordinate spaces: their compact
surface uses legacy page coordinates, while the detail surface can already
carry a centered output-space x coordinate. Its horizontal transform is
therefore idempotent (only legacy x values receive the center offset); vertical
placement remains relative to the legacy 600-pixel canvas.

## Reference HUD positions

| Output | `TodayDate` | `GameMain` |
| --- | --- | --- |
| 1920x1080 | top-right, 24 px margins | bottom-right, 24 px margins |
| 2560x1440 | top-right, 32 px margins | bottom-right, 32 px margins |

The GUI-resource wrapper moves visual nodes and hit-test nodes together. A D3D9
guard discards only the two known proxy sizes when they are textureless and use
the renderer's `0xFFD6D3CE` fallback grey. The textured HUD backgrounds and
unrelated transparent effects are left untouched.

## Rollout

1. Complete the in-scene HUD family and verify every secondary layer.
2. Adapt centred dialogs and office screens.
3. Adapt edge panels and navigation screens.
4. Treat world-following UI as an explicit exclusion set.
5. Audit menus and minigames at both 1920x1080 and 2560x1440.

All resource tools preserve raw bytes so Big5 text and original line endings
are not decoded or rewritten.
