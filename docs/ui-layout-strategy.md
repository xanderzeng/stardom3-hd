# Stardom3 widescreen UI layout strategy

The game renders its 3D scene at the selected supported 16:9 or 4:3 output
mode while most GUI resources still assume an 800x600 canvas. Layout therefore changes **positions by
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
later seven-item toolbar use 230px and 320px respectively. Visible toolbar
buttons are normalized into the authored `5, 51, 96, 141, 186, 231, 276`
slots whenever the active count changes. This repairs early saves whose
five-to-six transition expands the background but leaves the last two buttons
overlapping. The identified toolbar root is retained and its seven direct
children are checked once per rendered-frame interval, because entering a
venue and loading a save can both run the scene controller after the periodic
root layout pass and overwrite an already-correct slot. Object identity is
retained when auto-sized tags change width with their text.

Toolbar width changes use Stardom3's native GUI `SetSize` path. Writing the
width field alone updates bounds and anchoring but does not invoke the Plane
virtual that rebuilds its background geometry, leaving a seven-slot backing
behind a compact five-button toolbar. The native rebuild still maps the full
seven-slot `7001` texture onto the shorter quad, so the render hook recognizes
only that bottom-right background quad and limits its horizontal UV range to
`5/7` or `6/7`. This preserves the authored slot spacing instead of squeezing
seven backing cells underneath five or six buttons.

The team-formation friendship tag is another world-following control, but its
66x19 root has two children: a 20x18 numeric label and a 15x15 relationship
icon. That complete signature bypasses page anchoring and keeps the position
submitted by the team controller. In particular, a native `(320,411)` tag at
1280x720 remains `(320,411)` instead of being shifted to `(560,531)`.

Centered 800x600 pages retain their authored dimensions after their initial
placement. A child move under such a page is not treated as new-page
construction once the parent has been processed. This is important for world
dialogue: advancing one line moves several dialogue children, and replaying a
complete primary-root reflow for each move causes a visible input stall.

The random phone event uses a zero-sized `BababaCallOut` root whose phone
button is dynamically parked at `(-40,-40)` while idle. The root is identified
by its complete five-child structure rather than by the button's transient
position. The zero-sized structural root keeps its visibility byte cleared
even while its children render, so the ringing phase is identified from the
visible phone button with all dialogue panels hidden. The retained button is
restored immediately to its bottom-left widescreen anchor, and later
off-screen position writes are corrected in the GUI move hook before they can
overwrite it. Once a dialogue,
confirmation, countdown or question panel is visible, the event controller
again owns the button position.

`BababaCallOut` can be loaded after the ordinary layout tree has become stable,
and entering the ringing phase only changes `BtnPhone` visibility without a
move callback. While the overlay cache is missing or invalid, a lightweight
250 ms discovery pass therefore checks only the primary root's direct children.
Once the five-child resource is found, discovery stops and the retained button
is refreshed every frame without another tree scan.

The retained phone child differs from ordinary root controls: the base
`GuiMove` routine returns without changing its canonical position fields even
when it is passed the correct widescreen coordinate. Position maintenance and
the live move hook preserve the native call, verify the result, and write the
two canonical fields only when that call demonstrably rejected the move. The
same verification runs again during first-draw maintenance so controller-side
parking cannot survive into the rendered frame.

The four post-answer panels under the same zero-sized root have the same move
restriction. When a panel becomes visible while still parked exactly beyond
the top-left edge, per-frame maintenance restores the panel position associated
with its complete resource signature. Hidden panels and visible panels already
inside the output remain controller-owned.

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

The map and save-load progress page is a different 800x600 control tree. Its
top status badge, central artwork and bottom progress strip identify it without
matching ordinary legacy pages. While that page is visible, rendering outside
its centered rectangle is clipped and the complete output is cleared to black
before the page is drawn. This preserves the authored loading artwork while
preventing the previous 3D scene and HUD from showing around it.

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

The artist profile is identified by its complete direct-root signature: four
110x35 tabs, one 549x281 profile card, and the hidden 100x500 side strip. The
whole 800x600 page is aspect-fitted rather than handled as a native-size
centred dialog. Geometry is retained for every descendant, so the portrait,
attribute values, radar grid, filled data polygon, six labels, and hit-test
controls all use the same 4:3 transform. This avoids the former mixed layout
where the profile surface remained at 800x600 while the renderer used the
widescreen viewport for some attribute-chart submissions.

The artist contract page follows the same aspect-fit policy. It is identified
by its single 522x290 contract card, the 100x120 portrait, and the complete
three-button action column. The page, card, portrait, labels, action buttons,
and hit rectangles are cached and scaled as one subtree, including later
runtime moves submitted by the contract controller.

The signing and renewal dossier is a separate 802x602 page recognized by its
single 434x410 panel, 100x120 portrait, and paired 98x28 decision buttons. The
dossier subtree is aspect-fitted with its labels and hit rectangles, while the
full-height character cut-in remains at its authored size as a separate root.

The airport selector is a direct-root 800x600 page recognized by its 700x100
flight-information strip, paired 95x28 go/leave buttons, and direct map-location
controls. The complete page is aspect-fitted so the world map, location markers
and labels, flight data, action buttons, and hit rectangles retain their native
relationships. The airplane is controller-animated and its controller reads
the GUI rectangle back while interpolating and deciding visibility. Its node
retains native dimensions, while the controller derives each live position
from the already-fitted route endpoints. The final 54x58 quad is therefore
scaled around the submitted rectangle's center without transforming the live
route position again. Maintenance passes do not replay the initial route position.
While the selector is visible,
drawing is clipped
to the fitted viewport and the uncovered output regions are cleared to black
instead of exposing the underlying airport scene. The native-size `TodayDate`
HUD remains anchored at the output's top-right. Primitives wholly inside its
336x35 surface temporarily disable the inherited airport scissor across
regular, user-memory, and indexed D3D9 draw paths, then restore it immediately.
The date remains visible over the right bar without revealing the scene around
it.

The large-studio event manager is a direct-root 800x600 editor page. It is
identified by its 760x567 ground panel, ten 480x20 event rows, 92x32 exit
button, and 28x226 scroll track. The complete page hierarchy is fitted as one
layout group so row buttons, pagination, scroll controls, labels, and their
hit rectangles retain the authored relationships. Runtime row arrangement is
kept controller-driven; maintenance passes do not replay the resource-template
positions over the visible list. While the page is visible, drawing is clipped
to the fitted viewport and the uncovered output regions are cleared to black
rather than exposing the renderer's blue clear colour.

The event editor is a separate direct-root 800x600 `EventUnit` overlay rather
than a child of the event list. It is identified by its 760x577 panel, paired
700x190 dialogue sections, and five 98x28 bottom action buttons, then fitted to
the same viewport. Its controller remains responsible for live selection and
edit-control movement; only new coordinates are transformed after discovery,
so opening dropdowns or editing event units does not restore template slots.
The five-row `Editor/SelectList` controls are detached direct-root overlays as
well. All five pre-created width variants retain independent geometry caches;
their game-supplied native anchors, popup frames, option rows, arrows and hit
rectangles receive the editor's aspect-fit transform together. If a popup was
centered before its children finished construction, its native anchor is
recovered only when the stored position actually contains both canvas offsets,
so hidden controls initialized at `(0,0)` are not converted to negative space.
Because the tutorial and editor reuse both the seven-child resource shape and
pooled object addresses, a currently visible tutorial root takes precedence
over every retained editor-dropdown record. Tutorial birthday selectors thus
keep their centered native size and placement when revisiting character setup.

Training selection already uses the expanded 800x600 page viewport, but each
activity launches a separate 426x369 minigame frame. The frame is recognized
by its 215x32 timer, 426x333 full-frame layer, and 400x230 play surface. Its
authored (187,126) position is recovered from the generic centred-canvas
translation before the frame, activity layers, labels and hit rectangles are
aspect-fitted together. The actual activity animation is hosted by a separate
set of direct-root 800x600 pages, each containing a 400x300 playfield at
(200,175); those pages are detected only while the frame exists and are fitted
to the same viewport so the rendered game, animation geometry and frame remain
aligned. When the training page's fitted 800x600 background is submitted, the
renderer clears the two regions outside that viewport after the office scene
and before the training UI, keeping both the activity and result screens on
black side bars without changing ordinary announcement pages.

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
5. Audit menus and minigames at representative 16:9 and 4:3 modes.

## Output resolution policy

Display configuration is validated before the window, backbuffer, or hooks are
changed. The supported 16:9 modes are 1280x720, 1920x1080, 2560x1440, and
3840x2160; the supported 4:3 modes are 800x600, 1024x768, and 1600x1200. An
invalid or partially specified pair disables the patch for that CreateDevice
call and forwards the original D3D9 parameters unchanged. The 800x600 values
elsewhere in this document describe the authored GUI coordinate system, not a
fixed output resolution.

All resource tools preserve raw bytes so Big5 text and original line endings
are not decoded or rewritten.
