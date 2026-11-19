# NFS III: Hot Pursuit — Android: technical details

An Android port of the static recompilation of **Need for Speed III: Hot Pursuit**.
The original 1998 Win32 x86 executable is translated into portable C++ that runs
on a virtual x86 CPU, with the Win32, DirectDraw, DirectSound and Glide 2x
calls it makes reimplemented on top of SDL3 and OpenGL ES. DirectInput is not
reimplemented: the game's controls are native, and ask the port's own pads,
mouse and keyboard directly.

Based on [motor-dev/nfs-recompiled](https://github.com/motor-dev/nfs-recompiled),
which does the recompilation itself and targets desktop. This project adds the
Android target: a launcher, touch controls, gamepad handling, widescreen races,
and a number of fixes in the Glide layer that only showed up once the game ran
on a phone.

NOTE: the Android implementation is not meant to be modified. In this repo, the
PC port only is maintained. Android serves only as a reference for
recompilation. Features are exclusively implemented in the PC version.

## Game data you supply

The executables are in the repository, as upstream, so a clone builds and runs
as-is. The game content is not: bring `FEDATA` and `GAMEDATA` from a retail disc
of the 1998 release — the Europe "Sold Out Software" disc is known to match, and
a CD image works. Copy both folders somewhere writable and clear the read-only
attribute, or the game cannot write its settings and saves. That folder is what
you import in the launcher.

`install.win`, the table of data paths the game reads before anything else,
comes from the installer rather than the disc. The Android build packages the
copy at `android/app/src/main/assets/gamefiles/install.win`; for a desktop run
put it next to the game data. `tools/make_install_win.py` regenerates it and
documents the format, reverse-engineered from the executable. So a complete
desktop game folder is `fedata/`, `gamedata/`, `install.win` and the four
executables from `nfs3hp/`.

## Playing on a phone

The game's data is imported once, from a folder holding `FEDATA` and
`GAMEDATA`, a ZIP of them, or an image of the disc: an `.iso`, or the `.bin`
(`.img`) of a BIN/CUE pair, read in place (`DiscImage.java`: ISO 9660 in
2048-byte sectors or whole 2352-byte MODE1/MODE2 ones; the CUE sheet is not
needed).

Android 8.0 or newer on a 64-bit ARM device (ARMv8, arm64-v8a) with OpenGL ES
3.0. Testing happens on Android 13, and
Android 10 was confirmed by a tester; 8 and 9 install but have not been played
on yet.

1. Put the game data folder on the device, e.g. `/sdcard/nfs3-og`.
2. Launch the app, import that folder, make it the active data set.
3. Play.

An import leaves the game ready for a phone: the gamepad control set (see
Controls), View Distance at Full, races at 1280x720 and a HUD arranged for a
phone — the standings and the maps in the corners rather than under your thumbs
— are written into its `config.dat` once, as the last step. Data straight off
the disc has no `config.dat` yet — the game makes it the first time it runs —
and gets the same at that first start. From then on the file is the player's,
and the game's own Heads Up Display screen rearranges it.

A race also starts sooner than the original's: the game used to copy the whole
of the track it was about to play, 7 to 14 MB, into a file of its own and play
from the copy. That was for a 1998 CD drive; here the track is played where it
lies.

The launcher holds what the game cannot ask for itself: **Controls → Touch** (the
on-screen controls, their layout editor, the key each one sends),
**Controls → Gamepads** (which pad is which player, Analog or Digital for each,
the key each button sends), **Controls → Force Feedback** (where the game's
effects are felt: off, the phone or the gamepad) and **Display** with **Screen
adjustment** (orientation, frame rate cap, gamma, brightness, contrast).

The menus carry no buttons of the port's at all: a finger works them as a
pointer, and the system's own back is the game's Escape. When the game asks for
a name, a tap on the line being typed in brings up the phone's keyboard and a
tap beside it puts it away; a box opened from a pad brings the keyboard up at
once, and with a real keyboard connected it never comes up. A tap on the
player's name beside its tab opens the same box. A pad's buttons follow the same
two worlds — the racing set while a race is driven, confirm and back everywhere
else.

The first screen's **NFS Web Site** button opens **Options** instead, the way
the race screen's Options button does, so the options are a tap away from the
start. The game reads the port's version of `MENUS/SETUP.MNU` in place of the
imported one (`src/nfs3hp/menu_data.cpp`, as for `MAINSPLT.MNU`): that button
takes the race screen's text and `nextmenu=options`, and loses its codelink, by
which the screen's code (`sub_452f80`) would otherwise make it open the web
site again. The imported data is left as it is.

### Controls

The game always sees two joysticks, `NFS Gamepad 1` and
`NFS Gamepad 2`, each with two axes: X steers, Y carries both pedals — the right
trigger pulls it up to accelerate, the left one down to brake. Gamepad 1 is
where the touch controls steer and accelerate too. Every other button arrives as
a keyboard key, chosen per button under Controls → Gamepads → Buttons: player
1's from the first pad, player 2's from the second.

*Gamepad ON* writes a matching control set into the game's
`fedata/config/config.dat`, backing the file up first, and the description of
both devices with it — the game regenerates every binding whenever the saved
device list differs from what it finds. *Default in game* writes the game's own
keyboard defaults instead. Those put split screen's second player on keys the
first pad's buttons send, so while a split-screen race runs on them the pads
send nothing but Escape.

Each pad is set to *Analog* or *Digital* under Controls → Gamepads. Analog is the
above: the stick steers, the triggers work the pedals' axis. Digital drives a
race as a keyboard does, and only a race — the menus are the same either way:
the D-pad steers left and right, D-pad up and the right trigger accelerate,
D-pad down and the left trigger brake (a trigger counts as held from a third of
its travel), and the stick does not steer. Each of those sends the player's own
key, and the control set binds it: player one's are the keys the touch
controls send, so a digital first pad is written as the set the on-screen
controls drive with when there is no pad; player two's are End, Page Down, Home
and Page Up — the keypad's 1, 3, 7 and 9 to the game, which hands those out in
its own split-screen defaults — bound for split screen's second player while
Gamepad 2 is digital. The game sees keys steering and steers them its own way,
easing to full lock and back, in a single race, split screen, a network race
and a replay alike. The launcher rewrites the set as the game starts whenever a
pad's setting changed. The two players never share a key: the second pad never
sends one of the first player's, so its D-pad's arrows, which steer player one
whenever player one drives on keys, stay quiet in a race.

On screen, the game decides which controls are up. In a race: two steering
buttons, the pedals and the rest, and the steering buttons turn the wheel the
way the game's keyboard steering does, easing to full lock and back, while a
pad's stick stays analog. In the menus a finger is the game's own pointer: a tap
clicks where it lands, resting still for a quarter of a second presses and
drags, and a swipe clicks nothing; no controls stay on screen. The layout
editor shows the controls exactly where the game puts them.

The phone's back button or gesture is Escape, in the menus and in a race; with
the on-screen keyboard open it closes the keyboard. A real mouse moves the
game's cursor to its own pointer and clicks with its buttons, races included.

### Vibration

The game's own force-feedback effects are implemented
(`src/lib/forcefeedback.cpp`) and translated to rumble. Only
the first force-feedback device gets them — Gamepad 1 or the phone — so Gamepad
2 never vibrates. Where they are felt is one choice under Controls → Force
Feedback: *Off*, *Phone* or *Gamepad*, never the phone and a pad at once. A pad
plays the game's jolts, road and engine; the phone plays jolts, the engine by
its revs, and how hard the car corners. How strong a pad plays is set in the
game's own Force Feedback menu. The choice takes over from the two switches
0.76 had, one under Controls → Touch and one under Controls → Gamepads: the pad
if its switch was on, otherwise the phone if its switch was. Nothing vibrates on
a button press.

### Widescreen races

Options → Graphics → Screen Size offers 1280x720, 1600x900, 1920x1080 and
Dynamic Fullscreen beside the original 4:3 modes, and the choice survives a
restart. Dynamic Fullscreen fits the race to the screen it runs on, whatever
its shape -- 16:9, 20:9, a tablet's 16:10 -- with no bars: its height is the
screen's up to 1080 and its width the screen's proportion of it, worked out
again as each race opens its mode, so a foldable opened or shut between races
gets its new shape at the next one (during a race the picture keeps its shape
between bars). Its size is in the Voodoo2 driver's mode table's last entry
under a resolution id of the port's own; the settings keep a marker for it
rather than a width (`src/nfs3hp/dynamic_fullscreen.h`), and a fresh import
starts with it. A race then
opens the view sideways instead of stretching it: the vertical angle stays what
the game draws and the horizontal one widens with the screen, in every view —
chase and in-car, the mirror, both halves of split screen. The menus stay 4:3;
only the race switches mode, and its loading screen is shown at 4:3 in the
middle, between black bars.

The HUD keeps the proportions it was drawn with: frames, text and the points on
the map are sized as on the 4:3 screen that fits, and the gauges drawn as
pictures keep their shape. The layout saved in `config.dat` is never touched, so
a 4:3 mode looks exactly as it did. The HUD editor frames each element by what
it draws, so a line of text can stand next to a gauge or at the edge of the
screen. The in-car cabin fills the width at its own proportions, centred, with a
little of the roof and the dashboard's lower edge cut off.

`NFS_WIDESCREEN=0` takes the wide modes back out wherever an environment
variable can be set — the launcher does not set this one.

View Distance at Full draws the whole track out to the far distance in every
mode, split screen and night included, as the Modern Patch does; the other
settings keep the original distances.

Alpha intensity in Advanced Graphics works here as well. The original applies it
only on its Direct3D driver, so on this one the slider used to move and change
nothing.

### Full colour

The picture is drawn with eight bits a channel, races and menus alike, with no
dithering. Textures that are 32-bit on the disc stay 32-bit: most of the cars'
paint, the HUD, smoke, lights and sky, and the menus' track pictures, car
comparison and logos. The original let only its Direct3D drivers take them and
shrank them to 16 bits for a Voodoo2; here the Voodoo2 driver takes them too
(`tools/apply_full_colour.py`), so Screen Size reads "x 32". Art that is 16-bit
on the disc — the track surfaces and most of the menu backgrounds — stays as it
is. `NFS_TEXTURES32=0` goes back to 16-bit textures.

### Texture filtering and the road's grain

The game hands every texture to the Glide layer as a single level -- the
track, the cars, the sky -- so far-off surfaces were sampled from the
full-size picture: the road's markings broke into blocks towards the horizon
and the road and fences shimmered as the car moved. The levels below are now
made as each texture is uploaded (`ThrashRenderer::upload`): opaque 565 ones
by the GPU, those with alpha on the CPU with each colour weighed by how
opaque it is, since a plain average takes in the black of the see-through
texels and left smoke and cut-outs with dark specks. They are sampled
trilinear with 8x anisotropic filtering (`NFS_ANISOTROPY`, at most what the
GPU offers). Chroma-keyed draws still filter by hand from the game's own
level. `NFS_TEXTURE_FILTER=0` is the Voodoo's sampling again.

**The key colour is per picture, not per frame.** Glide's chroma key is state
the game sets as it changes material, and its pictures do not agree on one
colour: the cut-outs are keyed by black, the weather's drops
(`gamedata/render/pc/weather.fsh`, 565 texels with a blank 0x07e0 green round
them) by green. The port used to key a whole frame by the last value set, so
every picture but that one drew its blank background as a rectangle over the
view. Each batch now carries the key in force as its triangles were queued
(`Batch::chromaKeyColor`, `setChromakeyValue`), and the shader is given it per
batch. A 565 texel's byte is compared both as its 5 or 6 bits shifted up (0 to
248) and scaled to 0 to 255, since the game's keys come in one of the two
(`keyed()`). `NFS_CHROMA_TRACE=1` prints each colour the game keys out, once.

Over that, the road and the ground get a fine grain (`NFS_ROAD_DETAIL`, 0.35):
a tiling 256 x 256 noise made at start, read in each surface's own texture
coordinates at eight to a texture, and lightening or darkening it by up to
that much. Its mipmaps average it to grey, so it fades by itself with
distance. It goes on the solid, wrapped, depth-written surfaces -- the
track's, not the HUD's, the sky's or a cut-out's -- and never on a car: the
cars' vertices are noted as `sub_49da30` lights them each frame
(`native_vertices.cpp`), and THRASH's drawing (`native_thrash.cpp`) marks a
triangle whose first corner is one of them (`ThrashRenderer::setCarSurface`).
`NFS_ROAD_DETAIL=0` leaves it out.

The rest of the world -- walls, buildings, trees, fences, grass, rocks, all
of it depth tested but not a car and not the road's grain -- gets the same
noise, finer and fainter (`NFS_OBJECT_DETAIL`, 0.3), at two to a texture and
on its opaque texels only, so smoke and glass are left alone. The track's
textures are 64 x 64 (1555 on the disc), stretched over a whole wall or
hillside, and where one is seen larger than it is -- a texel bigger than a
pixel -- the GPU's bilinear melted it into a cross-hatched blur. There the
shader draws it Catmull-Rom instead (`sharpTexture`): the sixteen texels
around the point in nine bilinear reads, the overshoot clamped. Where a texel
is a pixel or smaller the GPU's own levels, trilinear and anisotropic, are
the better filter and are kept, with the two blended across the step.
`NFS_TEXTURE_SHARP=0` is bilinear again. Ground seen at a grazing angle is
magnified across and shrunk along at once, so it stays on the GPU's
anisotropic filtering. The grain leaves out what is added onto the picture
(blending off, which is additive here) -- the headlights' pool, the glows: a
grain over light lay on the road at night as a white noise.

### Haze, glow, colour grade and anti-aliasing

Five effects on a race's picture, each on by default and off with its
variable set to `0` (or `off`): `NFS_HAZE`, `NFS_BLOOM`, `NFS_GRADE`,
`NFS_AA`, `NFS_EXPOSURE`. The menus are never touched.

**Haze.** The fog was worked out at each corner and mixed into one flat
colour. Now, for fogged draws blended over the picture, it is worked out at
each pixel from that pixel's own 1/w (linear across the screen, so exact), and
it fades into the sky behind the pixel rather than into the flat colour: the
sky as the frame drew it before the world (`takeSky`, a copy of the
framebuffer at the world's first draw, with its levels), read blurred at the
pixel and brought back from the gamma the picture went out at. Far hills take
the sky's own colour where they meet it. Additive draws keep the old fog: haze
added onto the sky only brightened it. `NFS_HAZE` takes a number for how far
towards the sky, 0.8 by default.

**The whole-picture pass.** As the HUD begins -- the same moment the wet
road's reflection is taken -- the picture is copied and drawn back over itself
(`postProcess`), before the HUD and the mirror go on top:

- FXAA (Lottes's original): five reads a pixel, and a blur along the edge
  where it finds one (`NFS_AA`).
- A glow (`NFS_BLOOM`, 0.6): what stands brighter than the sky behind it --
  lamps, headlights, tail lights at night, the sun's disc, but not a pale
  horizon by day -- kept at half size, spread down that texture's levels and
  added back from five of them.
- A colour grade (`NFS_GRADE`): a contrast, a saturation and a tint for each
  track (`colourGradeTick`, nfs3hp_main.cpp): Hometown's autumn warmer,
  Redrock Ridge and Lost Canyons warmer still, Atlantica, Aquatica and the
  Summit cooler, Country Woods a touch greener, Empire City flatter and
  harder; bluer and greyer at night, greyer and softer in the weather.
- Auto-exposure (`NFS_EXPOSURE`, 0.7): the picture brightened as it goes dark
  and dimmed as it goes bright, as an eye adapts, so a tunnel's mouth or a
  dark stretch is not a black hole for as long as the car is in it. The
  picture is metered into 64x64 (`g_meterShader`), each texel the log
  brightness of sixteen pixels, weighted towards the middle of the view; its
  levels down to one texel are the scene's geometric mean. One texel kept from
  frame to frame (`g_adaptShader`, two in turn, never read back) holds two
  brightnesses: the eye's, following the scene within a second (0.8 s into the
  dark, 0.3 s into the light), and the track's usual, following it over many
  (60 s into the dark, 8 s out of it, so a long tunnel does not become the
  usual). The picture is brightened or dimmed by the eye's distance from the
  usual in stops, less 0.35 either way left alone, times `NFS_EXPOSURE` -- at
  most a stop brighter and half a stop darker (`exposure::` in
  `thrashrenderer.cpp`). A race's first picture, and the first after half a
  second without one, starts both from the scene. Before the glow and the
  grade, so lamps keep their look; never the HUD or the menus.
  `NFS_EXPOSURE_TRACE=1` prints the scene, the usual and what is made of the
  picture once a second (`[EXPOSURE]`).

### Headlights on what stands by the road

The game lights the road with the headlights (a patch of light it lays on it,
`sub_491bc0`) and the cars with each other's, but a wall, a fence, a tree or a
building stood in the dark however close. The shader lights them now. Each
frame `sub_491bc0` is asked for the player's car's patch with the view it is
drawn in; the lamps -- two metres ahead of the car and half a metre up, along
its forward axis dipped a little -- are turned into that view as `sub_4bf4c0`
turns a point (`noteHeadlight`, native_render.cpp). In the shader a pixel's
place in the view comes back from the screen and its own 1/w through the same
projection, and its facing from how that place changes across the screen. It
gets a cone of light from the lamps, bright on the axis and soft to the edge,
falling off with distance, brighter on what faces them, a quarter as bright on
what lies level (the road has its patch already); its texture's colour lit by
that is added. Only where it is dark (by the fog's colour), only on what is
blended over the picture, not on the cars or after the HUD has begun (the
mirror). `NFS_HEADLIGHT_OBJECTS`, 1.6.

### Shadows of what stands by the road

The cars cast shadows (quads the game lays under them, stretched away from the
sun by the port); nothing else did. Now the whole-picture pass casts them from
everything in the depth buffer, before the HUD. The depth buffer holds 1 - 1/z
(as the game's vertices give THRASH their depth), so each pixel's place in the
view comes back from it and the projection; from there a ray goes towards the
sun ([0x55e6a0], turned into the view as the sky is drawn, `noteSun`), its steps
growing to some fifteen metres, each brought back to the screen; where the
picture there is nearer than the ray by more than a hair and less than a
thing's thickness, the pixel is in shadow -- the darker the nearer. Cast only
onto what lies level (the road, the verges, the ground; a fence's own rails
went grey), not by the cars, which have their own (their pixels leave 0 in the
picture's alpha), and faded at the screen's edges, past which what casts may
lie. Fainter in the rain and at night as the cars' own are.
`NFS_OBJECT_SHADOWS`, 0.45.

A lamp's glow leaves the picture's alpha as it was: its texture is 8888 or
4444, which counts it as a car's, and the whole square of each glow and of each
point of a tail light's trail had marked the road under it as car -- left out
of the motion blur and these shadows, so a sharp piece of the car's own shadow
stood in each, two broken shadows behind the rear wheels.

### Sparks off walls

A car's collision effects come each frame from `sub_42ac40`. What the car
touches is `[car+0x170]`: a wall or barrier is 0x40000 in the high word with
its material in the low byte, and the table at 0x553740 gives each material's
particles -- 3 sparks (1, 2, 4: metal, and the stone walls of Atlantica and
the desert's canyons are 4), 2 dust (3, 5, 7), 0 nothing. The game threw a
burst (`sub_4cac20`, thirty of particle kind 3 at full strength) only while
scraping at over 22 m/s; a hit made one spark. `collisionEffects`
(`src/nfs3hp/native_effects.cpp`, hooked at the top of `sub_42ac40` by
`tools/apply_native_effects.py`) adds a burst as strong as a hit over 5 m/s
(full at 10), and one while scraping from 6 m/s, on spark and dust materials
alike. A burst takes the car's velocity (`+0xa4`), which sent it into the
wall, out of sight; for the added bursts the velocity is bounced off the wall
first (its normal from the car's centre to the contact point) and handed back
after. `NFS_SPARKS` (2) multiplies kind 3's burst size (`[0x560ce4]`) for
every burst, the game's too; 0 leaves the game's sparks alone.

### Dust, grit and sparks of the port's own

The game makes a particle with `sub_4ca850` (its kind in eax; thirteen
arguments, `ret 0x34`: where, three floats; a scale; two directions; how fast,
three) into its pool of 600 at `0x7ddfd0` and draws each as a picture from
`sfx.fsh`. Kind 2 is the dirt and dust thrown up off the road, kind 3 a spark
of a collision; the kinds' table is 128 bytes each at `0x560b64`.
`particleSpawn` (`native_effects.cpp`, hooked at the top of `sub_4ca850`)
hands those two kinds to `native_particles.cpp` and returns with none made
(eax 0), so the game decides when and where, and the port how:

- **Dirt** becomes grains -- small clumps, each outline a few waves round it
  set by its seed, thrown up and out, falling back under gravity, lying a
  moment where they land -- and now and then a puff of fine dust that drifts,
  spreads and fades. For each of the game's, 2 grains and one more for every
  3 m/s it was thrown at (times `NFS_PARTICLES`), thrown higher and wider the
  faster; a puff more often the faster.
- **Their colour is the ground's**: the renderer takes it from the picture
  in a ring round where they were thrown (world pixels only -- a car's are 0 in
  the alpha, and the car that threw them stands over the very spot), a grain
  darker, a puff paler: sand in the desert, snow on snow, mud on grass.
- **A spark** is a streak from where it was a frame ago to where it is, white
  hot at its head, cooling through yellow and orange to red as it falls, and
  skipping off the ground.

Each frame, as the main view's sky is drawn (`noteSun`, its view known), they
are moved by the race's clock -- standing still with it -- turned into the
view as `sub_492370` turns a lamp, sorted back to front, and handed to the
renderer (`glide2x::effectParticles`). `ThrashRenderer::drawParticles` draws
them over the world just before the whole-picture pass, a quad each, their
shapes made in the shader (`g_particleShader`, which tells the shapes apart by
names taken from `EffectParticle::Type` -- `kFlake`, `kDroplet` -- rather than
by its numbers, `particleTypeNames` passing them in as the shader's defines),
hidden behind what stands in front of them by the depth buffer, a puff fading
where it meets what is behind it, the picture's alpha left as it was. Not in
split screen, nor without the native THRASH renderer: the game's own then.
`NFS_PARTICLES` (1; a number up to 3 for more, `0`/`off` for the game's).

**No particle ever shows the edge of its quad.** A cloud's or a flake's ragged
rim is its noise's, scaled by it -- so where the noise came out small the rim
withdrew further than the quad's own side, and the straight side showed as a
turned square of dimmed picture over the view (tyre smoke crossing the camera
was the worst of it). Every shape whose edge is noise's is multiplied by
`inside()`, which is 1 well inside the quad's circle and 0 at its rim, and a
raindrop's dash fades at its ends as well as its sides: whatever a particle's
size and seed, nothing of it reaches its quad's edge. (Checked by rendering one
quad of each type offscreen and reading the alpha back: at a quad's mid-sides
every one of them is 0 but the streaks', whose sides are 1 to 5 of 255 and whose
ends are the dash's own.)

**In the rear-view mirror too.** A single-screen race draws two views: the main
one, then the mirror's (`sub_41d960`). They are moved and made once a frame, for
the main view (`particlesFrame`), and then -- as each later view's sky is drawn,
which is where a view's camera and projection are known -- turned into that
view again (`particlesMirrorFrame`): the mirror's own record carries its camera
and matrix, and the projection the game set for its pass puts the result inside
the mirror's rectangle, so a flame or a puff of smoke behind the car is behind it
in the mirror at the right place and size, the blended ones first and the added
ones after them, both views' in one buffer. Smoke in the mirror fades against
the mirror's own pixels, as it does against the main view's.

### The weather's flakes and drops, the port's own

What falls out of the sky is the game's particle kinds 7 and 8, and its pictures
of them are **squares with nothing cutting them out**: 565 texels, no alpha at
all, and (so far as the port's Glide state says) no colour key either -- so
every flake carried a box of dimmed picture over the view, and one that came
near the lens was a large one. The port now makes those itself
(`throwWeather`, `native_particles.cpp`), as it makes the dust and the spray:
the game still decides where and when, and how fast and how big
(`argument(3)`), and the port draws

- **a raindrop** as a short bright streak from where it was a frame ago
  (`EffectParticle::Droplet`, added to the picture like a spark),
- **a snowflake** as a small soft speck, ragged by its seed, its quad stretched
  the same way as a drop's -- from where it was a frame ago to where it is, soft
  at both ends of it (`EffectParticle::Flake`, blended like a puff). Standing
  still the flake's quad is square and it is the speck it was; at speed the
  flake streaks past the windscreen the faster the view goes, as the drops do
  (2026-11-18),

in a pale colour of its own, falling as the game threw it -- the rain hardly
slowed, the flake drifting. Which it is comes from the track's weather (its
`type` in `3trNNw.hrz`, 1 rain and 2 snow, `trackRains` of `nfs3hp_main.cpp`).
A storm makes a great many of them, so they are kept to 900 at once and never
crowd the dust and the sparks out of the pool. `NFS_WEATHER_PARTICLES`
(`0`/`off` for the game's own squares).

**A particle that comes onto the camera is left out**, not splashed: the game's
own particle drawing hands one within 3 of the camera to `sub_4ca500`, which
lays a splash of the kind's picture over the view and kills the particle -- and
that splash is a square of the same uncut picture, the box that showed for a
flake right on the lens. With the port's weather on, `particlesNative`
(`native_vertices.cpp`) drops such a particle instead and never calls
`sub_4ca500` (`ownWeather`), as the port's own are culled within 1.5 m of the
camera. With `NFS_WEATHER_PARTICLES=0` the game's splash is the game's again.

The same suspicion hung over every sprite the game draws with those pictures
(the "turned dark squares" of the port's wheel spray, TODOs.md) -- and it was
so: none of them is cut out. `NFS_SPRITE_TRACE=1` prints each kind of draw
once, with the texture the game gave it, its format, whether it is cut out and
by what colour, and what it is blended by.


**What falls is the port's own, round the camera** (`fallWeather`,
2026-11-17). The drops and flakes the game throws (kinds 7 and 8) are counted,
not drawn, and their rate a second, averaged over three seconds, is the mean:
rain at that rate (its drops took about as long to reach the ground as these
do), snow at 0.3 of it (these drift down for six seconds, its lived under two).
Where the game throws fewer than 20 a second, 700 drops or 120 flakes a second
are the mean. The rate is not steady: the mean times `lo + (max - lo) n²`, `n`
a smooth noise of the race's clock with a knot every 9 s, `max`
`NFS_WEATHER_MAX` (2.5) and `lo` what makes the mean 1 (0.36 for 2.5) -- lulls
and showers over half a minute or so. Each starts in a cylinder 14 m across
round where the camera will be half a second on, from a metre below it to seven
above, falling at 9 m/s (a flake 1 m/s), and is gone at the ground under the
player's car. Their floor was the world's 0 before: on a track lying below it
every drop was under the ground as it was made, put up there and bounced, and
the rain was gone. `weather::` in `native_particles.cpp`.

**A full pool.** The rain's spray -- every wheel of every car on a wet road --
fills the particle pool (9600). A particle added to a full one takes the place
of one already there, in turn (`add`); until 2026-11-17 the oldest was erased
from the front and every particle after it moved down a place, some 320 KB moved
for each one added, hundreds a frame: the rain's lag. Snow's frost, four times
the grains it first threw, is kept under 6000 at once of its own
(`s_frostCount`), so it cannot fill the pool and leave the smoke, the weather
and the sparks none of it.

**The soft ones at half the size.** The spray's mist and the tyre smoke
(`Puff`, `Smoke`) are drawn into a layer half the picture's size each way, a
quarter of its pixels, cleared to nothing: their colour premultiplied, its
alpha what they cover; the layer is then laid over the picture, filtered
(`drawParticles`, `g_softLayerShader`). Soft clouds lose nothing by it, and
near the lens the rain's spray piles up hundreds of them hundreds of pixels
across, which an integrated GPU at 1080p paid 5 to 18 ms a frame for
(2026-11-18, `NFS_PERF_TRACE`). The sharp ones -- drops, sparks, grit, flames,
glows -- stay full size, after the layer. Their noise is a 256 by 256 table
read once rather than worked out with `sin()` at four corners, and the corners
of every round one are thrown away before anything is read.
`NFS_PARTICLES_HALF=0` draws them all full size, as before, to compare.

**None under a roof.** In a tunnel, or under a bridge, no rain or snow is seen
(`meterSkyOpen`, `thrashrenderer.cpp`): in the whole-picture pass, before the
particles, a grid of 12 by 6 points over the top of the picture (3 to 43% of
its height, before the HUD) is looked up in the depth buffer, and the share of
them that is the sky's or as far (as the glare has it) is kept in one texel,
eased over a quarter of a second, never read back. The particle shader fades
the drops and flakes by it: none under 3% sky, all from 12%. `overhead::` in
`thrashrenderer.cpp`.

**Rain on the lens** (`NFS_LENS_RAIN`, `lensDrops` in the composite): drops
on the view, the port's own and nothing to do with the game's drops on the
view (cleared every frame). Two grids of cells over the picture: clusters, a
cell 1/2.7 of its height -- a main drop and two to five smaller ones broken off
round it -- and a fine mist of single droplets, a cell 1/10 of it. A cell has
its cluster or not by its hash; it lives 3 to 6 s and comes again elsewhere in
its cell. Each drop is a little narrower than it is tall, 0.8 to 1 of it
by its hash. A drop is a little lens: it shows the picture round it upside down,
gathered from 2.6 to 3.8 times its size (the more the bigger the drop), blended
into the picture over its outer quarter so nothing outlines it. Standing still
a drop creeps down; at speed (from 8 m/s, all of it at 30) it slides out
towards the edge of the view and is gone sooner, the small ones lagging behind
it. A cluster and its travel keep within half a cell of its middle, so a pixel
looks at the four nearest cells of each grid, passing over at once a cell
whose cluster is out of reach. How many there are is how wet the lens is,
which the sky texel keeps in its other two bytes: wetting over 1.5 s in the
rain under 6% of sky or more, drying over 5 s under a roof or out of the rain;
each drop fades in or out by its own hash. Rain only (the road wet), drawn
under the HUD. `NFS_LENS_RAIN` (1; up to 2 for more, `0`/`off` none); `lens::`
in `thrashrenderer.cpp`, the drops' sizes and lives in the shader's
`lensCell`.

**Snow on the windshield** (`snowOnScreen` in the same composite): while
screen-projected snowflakes are present, a deterministic screen-space field
over the view holds pixel-art clumps of wet snow falling down it -- each column
at the same speed give or take a fifth, so they do not move as one sheet. Each
cell's flake lives 6 s (`snow::kLife`): it lands, and from 4 s
(`kShrinkFrom`) melts down through the atlas's shrink stages to half its size,
the same outline and dark wet edge at every stage, and goes; the cell rests 0.5
to 4 s and a new flake -- its own sprite, size, place and mirroring -- lands
there. Big flakes (with a few gaps and loose crumbs round them) and small ones
(none) are different sprites. They are read texel by texel (`texelFetch`), each
sprite pixel a whole number of screen pixels (1 or 2 at 720 lines, more above),
and never turned, so they stay pixel art. How many cells hold one is the
weather's snow by up to 2.5 times as the view comes on -- `snow::kMost` at
`kMostSpeed` (300 km/h) -- both shares eased over a second
(`kEaseSeconds`): worked out afresh each frame from how many flakes were alive
and from the speed, they wavered, and a flake whose cell sat at the edge of the
share blinked on and off with them; one that drops out of it now fades. Where
the sky is shut out, in a tunnel or under a bridge, the sky texel's `g` (the
share of the weather still on the glass) eases them off over 2.5 s
(`kGoSeconds`), back within 0.2 s (`kComeSeconds`) in the open. All of these
are `namespace snow`, handed to the shader as constants.

The atlas (`assets/snowflakes/snowflake-atlas.png`, written by
`tools/generate_snowflakes.py`) is 768x768 of 32-pixel tiles: a
column a sprite (nine big, then nine small), a row a shrink stage (six, down to
half). Twelve tones each, lightest in the middle and darker and thinner every
step out, never a ring lighter than the one inside it; white, light grey or
pale cyan, edged in a wet blue-grey; a glint. The CMake target copies it beside
the executable.

### The game's own sprites are not drawn

Every picture in the game's particle set is a 565 texel with no alpha and
nothing cutting it out, and the pool turns each quad to its particle's own spin
(`+0x38`, `+0x3c`), so **each of them was a turned square of picture over the
view**, brighter or dimmer than what stood behind it -- and the drops that land
on the camera (`dropsNative`, `sub_4cb750`) were turned rectangles of blurred
view over the view, in the rain the worst of it. So neither the pool's quads
(`particlesNative`) nor the drops on the view (`dropsNative`) are drawn at all:
`NFS_GAME_SPRITES` (`1`/`on` for them back, for a comparison).

Those two stand-ins draw under `drawingPrecision`, not under the race's own
single precision, and that was the whole bug: a race's FPU is extended by
default (`NFS_FPU_EXTENDED`, `raceControl`), so while they asked for single
precision the generated code drew instead, in **every** race -- and it drew the
game's squares, and splashed the particles that came onto the camera into the
drops on the view, however the port's own drawing was switched off. They build
a picture's polygon list and touch nothing the simulation reads; only with
`NFS_GAME_SPRITES=1` do they still ask for the race's single precision, since
the game's splash (`sub_4ca500`) then kills a particle as the generated code
would.

Nothing the port draws goes through that path, so nothing else is lost: the
dust, the grass, the spray, the smoke, the exhaust flames and the weather are
all made and drawn by `native_particles.cpp`, and with the game's kinds taken
over (`particleTakeOver`) the game makes none of its own for the pool to draw
anyway. The tyre marks, the skid polygons, the car bodies and the tracks are
drawn as they always were.

The game's sprites are still drawn wherever the port leaves a kind to the game
-- `NFS_PARTICLES=0`, or one of the switches that hands a kind back
(`NFS_TYRE_SMOKE=0`, `NFS_WEATHER_PARTICLES=0`, `NFS_WHEEL_SPRAY=0`), or split
screen, where every kind is the game's: something rather than nothing, the
squares and all. `NFS_GAME_SPRITES=1`/`on` draws them everywhere, for a
comparison.

With the squares gone the port's spray is what a wet road throws, so
`NFS_WHEEL_SPRAY` is on by default now.

### Grass, and spray off a wet road

What comes off grass is the game's particle kind 5 and the spray off a wet
road kind 10; both are taken over as dust and sparks are. Off grass the grains
are blades (`EffectParticle::Blade`): long, thin, a little bent, tumbling as
they flutter down (`turn`), a greener shade of the ground -- where the sampled
ground is green; off snow or earth they stay grains. Where fewer than three of
the samples round the spot are the world's pixels (a ground of 32-bit textures
is 0 in the alpha, like a car), all of them are taken. The spray is a pale
mist that billows out behind the wheel and hangs a moment, and fine drops
thrown up and falling back, both more the faster the nearest car goes
(`throwSpray`), in a colour of their own.

The game's kind 5 comes off some grass only: on Atlantica's and Hometown's
(surface 3) a car driving over it throws nothing, and sliding or braking on it
only the game's dark smoke (kind 6). So each frame every wheel on grass
(`groundUnder`, `native_surface.cpp`) of every car faster than 6 m/s throws
up to three blades, more often the faster (`grassFromWheels`), and the game's
smoke off a wheel on grass (the nearest wheel to where it starts,
`groundAt`) is taken over as grass instead.
`NFS_WHEEL_SPRAY` (on; `0`/`off` for none).

### Frost off snow

Snow is surface 15 (`groundUnder` 3), and the game throws nothing for a car
merely driving over it either. So each frame every wheel on snow of every car
faster than 6 m/s throws frost (`frostFromWheels`, `native_particles.cpp`),
and the game's own dust off a wheel on snow (kind 2, or kind 5) is taken over
as frost too (`throwFrost`): sharp chips of ice (`EffectParticle::Frost`) --
small faceted solids, a few sizes, thrown up and out, tumbling as they fall and
gone where they land -- and a lot of fine snow spray after them: light grains
that drift and spread wider than dirt, and now and then a soft cloud of it.
All in a colour of their own -- white, cyan and light grey -- never the
ground's. A burst throws four times as many grains and twice as many chips and
clouds as it first did (2026-11-19), and the frost in the air is held under
6000 of the pool's 9600, so a road full of cars on snow still looks thick
without the smoke and the weather being crowded out. `NFS_SNOW_SPRAY` (on;
`0`/`off` for none).

### Tyre smoke

Smoke comes off a tyre whenever it is being scrubbed -- **whenever the game
lays a tyre mark**: cornering at the limit or sliding, a wheel spun up out of a
standstill, a wheel locked under braking, the handbrake pulled. The port asks
that of the car itself each frame (`tyreSmoke`, `native_particles.cpp`) rather
than waiting for the game's own smoke: how much of the velocity is across the
car (`+0xc0` is its right -- over 1.2 m/s the tyres are being dragged over a
road they no longer roll along, full smoke at 4), and for the player's car the
pedals -- throttle `0x79c339` with the revs over half the redline (or pulling
away in first) and the car under 12 m/s (spun up), brake `0x79c33a` above a
half or the handbrake in the bits of `0x79c33b` over 3 m/s (locked); the
opponents' hard braking is a fall in their speed. And every piece of a tyre
mark, as `sub_4c6cb0` lays it, calls back into the same thickness
(`tyreMarkSmoke`, from `skidLaid` in `native_skids.cpp`): the marks and the
smoke are the same event, so an opponent's slide smokes too and no wheel the
game marks -- or the port's own launch and pedal marks -- is left out. It comes
off one of the wheels that is scrubbing, on **pavement only** (`groundUnder`;
grass, dirt and snow throw their own dust) and at the driven end when it is a
wheel spinning -- the end found by where the contact point lies along the
car's forward axis, not by the wheels' order. The game's own particle kind 6
is taken over as well (`throwSmoke`), for the wheel it names with the speed it
names.

What comes off is `EffectParticle::Smoke`, blended over the picture like a puff
rather than added to it: a dark cloud of its own colour (`noGround`), almost
black at its heart and grey as the sky catches its top, ragged by its own noise.
How thick it looks is the speed it is scrubbed at and the seconds the brake has
been held: the speed bands are 0-50, 50-100, 100-150, 150-200 and 200 km/h and
up, and each thickens fully over 1, 2, 2, 4 and 4 s of braking (a third of the
way at once); the speed's own share goes from a third at rest to all of it at
200. A wheel spun up uses the throttle for its thickness and a slide its
sideways drag, so smoke comes off at a standstill too. More of it and thicker
the denser the scrub: one to three clouds a call, a cloud every three to six
ticks a car (the gap drawn afresh each time). Each cloud's life, size, and its
height over its width are its own within a little, so no two are quite alike.
It is **heavy**: it leaves the tread with almost no upward speed, drags hard,
falls and lies on the road, no longer growing -- and is gone in six and a half
to eight seconds, fading out gradually over its last seconds. A car that holds
its speed -- hardly accelerating either way -- for a second while its tyres
smoke is **drifting**, and its clouds are the big growing ones: they start
smaller, are lighter and billow rather than fall, and keep growing, unevenly,
until they saturate -- fully grown about 1.5 times as wide and twice as tall as
the ordinary cloud. `NFS_TYRE_SMOKE` (on; a number up to 3 for more and
thicker, `0`/`off` for none).

On grass the same kind is grass thrown up instead -- see above -- so a wheel
sliding on grass throws blades and no smoke.

The game's own particle is still made for each kind taken over (the hook
calls `sub_4ca850` itself, then marks the particle free at once): skipping it
left the snow's flakes drawn as dark squares. Two more causes of squares round
particles were in the renderer: a puff's noise alone did not bring its alpha
to 0 at its quad's edge (now faded out from 0.6 of its radius), and the game's
flake and spray sprites write depth over their whole square, so the port's
particles behind one were cut away in its shape. A batch that writes depth now
writes it only where its alpha is at least 0.08 (`u_depthCut`) -- and for the
world's batches (not the cars') at least 0.5 (`kDepthCutWorld`): a faint square
of the world's still left its whole square in the depth buffer at 0.08, and the
spray and the weather behind it showed as turned boxes cut out of them, in
the rain and snow (TODOs.md, 2026-11-17). The cut discards what is under it,
so a lamp's glow (`batch.light`) has none and writes no depth: the softened glow
is 8888, the world's by its format, and all but its bright middle was cut
away -- the tail lights unlit once a race got going, with no halo and no
trails, and the brake lights barely there.

### Exhaust flames when gearing up

A car shifting up under power spits a flame out of each of the exhausts its
folder marks (`tools/car_model.py exhausts`, Marking a car's exhausts): one per
tip, a hand's width out of the pipe. A car with no `exhausts.json` throws
nothing at all, and neither does one that is coasting or is shifted through a
gear the byte at `+0x50b` does not record as one -- `engineFlames`
(`native_particles.cpp`) watches that byte of each of the sixteen cars, 1 being
neutral, 2 the first forward gear and 0x64 "none yet", and fires when it has
gone up. **Every car of the race does it, the opponents' as much as the
player's**: the loop is over the sixteen slots, each car owning a race record,
and a car finds its exhausts by the body name the race gave it (`[car+0x1e4]`,
The car's own structure) before the game's id-to-folder road is tried
(`carExhausts`, `native_exhausts.cpp`). That name is the whole answer the game
itself makes of the car's id -- the folder a racer's model, textures and spec
came out of -- so the lookup is exact, not a guess: a car marked with four
exhausts gets all four (the trace prints every tip: `cout` and `32ac` four each,
`j220` two). Traffic and cops carry a body code instead -- `0005`, `vett` -- so
a body marked once serves both a racer and the traffic built on it.

Standing still at the line with the engine revved, a car pops them instead, one
exhaust at a time, every 0.15 to 0.35 s -- a limiter's backfire rather than a
jet. Any of three things says it is being revved, because a car standing still
does not keep a throttle of its own and what the game does with its revs there
is the game's to decide:

- **the throttle**: the first player's byte at `0x79c339`, 0 to 255, above five
  eighths -- the same byte the camera's pull is off. Asked only of the player's
  car, the one the first view follows (`[[0x5e10b0]+8]`).
- **at the redline**: revs (`+0x590`) in the top seventh of the car's own
  redline, `[car+0x528]+0x1c4` (the rpm the drivetrain drives to, redline less
  200 at full demand, next to the minimum rpm at `+0x1c0`, and the rev counter's
  full scale) -- near enough the peak of its torque.
- **rising**: revs up by 60 in a frame, a blip of the throttle, which no car over
  a walking pace can be doing without the speed to go with it.

Under 3 m/s (off the velocity at `+0xa4`, which every car keeps) is the start
line; the same test catches a launch, wheels still spinning. The timer only runs
while the engine is held there, so the limiter's bounce cannot turn into a
stream of flames. An opponent cannot be at any of the three at a standstill --
its revs are the game's own working out of the speed it is driven to
(`sub_4061b0`), nothing while it waits -- so the flames at the line are the
player's.

They are the port's particles, `EffectParticle::Flame`, drawn in the additive
pass of `drawParticles` and shaped in the shader: a ball of noise, ragged at its
edge, hotter towards its heart, white there -- the file's colour (or `#f2380a`
where it names none) out at the edge, near white in the middle. Each grows out
of nothing over 0.18 s (the square root of its age, so fast at first and then
slowing), holds its size for 0.8 s, trembles all the while on the screen -- a
dozenth of its size either way, from sines of its age and a seed of its own --
and fades and stops by its one second, no more. **They stay at the pipe they
came out of**, on the car: a flame remembers the car and where on it it burns,
in that car's own space, and is turned into the world afresh each frame from the
car's centre and axes, so it goes round corners and down the road with it
instead of hanging over the road behind. **Their own car hides them**, by
its body rather than the depth buffer: the high-detail parts of the car's
`car.fce` (out of `car.viv`, the copy the game reads; wheels left out, since
they turn) are read with its `exhausts.json`, in the same model space, into a
box tree, and each frame, in each view, five rays from the camera -- its place
in the world is -t M^T of the view's record, put into the car's space by its
axes -- to the flame's middle and four points round it are tried against them
(`exhaustBodyLetsThrough`, `native_exhausts.cpp`). The share that get through
is how much of the flame shows, and its pool of light goes with it. A hit
within 12 cm of the flame is its own pipe's mouth and does not count; a camera
inside the body's box (an in-car view, which draws none of it) cuts nothing.
The depth buffer had been left to do this and did not for the car the flame
burns on: from in front of it the flames showed through the car, and a side
pipe's through the rear bumper. Other things in front of a flame still cut it
by the depth buffer. A car on `NFS_EXHAUST_DEFAULTS`' pipes has no body read.
`NFS_EXHAUSTS_TRACE` says how many triangles each body came to.
`NFS_EXHAUST_FLAMES` (`0`/`off`) takes
them away. `NFS_EXHAUST_DEFAULTS` (off; `1`) gives every car with nothing marked
a pair of default pipes instead of none -- at the rear corners, `x` ±0.42 m,
`y` -0.27, `z` -2.05, 0.05 m across, which is where the game's own marked cars
have theirs -- so that every car on the grid can be seen to throw flames before
it has been marked; a car with a file keeps its own, four where it names four.
`NFS_EXHAUSTS_TRACE` (`1`) prints the table of what was read at startup, every
tip's place among it, and then a line the first time each car of a race shifts up
or revs at a standstill -- the folder it was found under, its revs against its
redline, how many flames, and which of the three said it was being revved --
which is how to tell that the opponents' are working, and what to tune the revs
to: a car at rest says what it climbs to, a line every 1000 rpm (`[EXHAUST] slot
0 "cout": at rest, 6820 rpm (redline 7500), 0.02 m/s`), and one the first time a
car pops on a downshift or crackles on the overrun.

**A hard downshift pops them too**, and so does lifting off at the top of the
revs: a real racing car's overrun, the unburnt charge catching in the exhaust
when the engine is dragged up to the lower gear's revs or down by the car.
A downshift from a gear into a lower one at over 4 m/s pops one exhaust or two
at once; lifted throttle at over 12 m/s with the revs in the top third pops one
every 0.1 to 0.35 s for as long as it lasts (`popFlame`). Each is **half a
flame** -- half the size and half the light of a gear change's, a crackle
rather than a jet. Only the player's throttle is a byte the port can read
(`0x79c339`), so only the player's car crackles on the overrun; every car of the
race pops on its downshifts, opponents included.

**They throw light on the road.** Each flame pools a soft disc of its own colour
under the car (`EffectParticle::Glow`, added to the picture as the flame is),
placed where the pipe is but at the height the car's wheels stand at -- a
wheel's contact point is at `+0x304`, 0x78 bytes each, and the lowest of the
four is the ground, worked out once a frame for each car that lights something.
The disc is as wide as the ball is big (a metre or so), squashed lengthwise as a
disc on the ground is seen from behind, and **faint** -- a third of the light the
flame itself is worth, so it reads as a tint spilling onto the road under the car
rather than a puddle of paint. It comes up with the flame, throbs with it and
goes out with it, and a pop throws half as much. `NFS_FLAME_LIGHT` (`0`/`off`)
takes the pools away; a number up to 3 makes them stronger (0.5 for half).

### Sun glare

By day with no weather (`noteSun`, `native_render.cpp`), the sun is where the
light the cars are lit by comes from (`[0x55e6a0]`, turned into the view, as
the shadows of what stands by the road use it). The whole-picture pass
(`glare` in the composite shader) puts it on the screen through the
projection and works out how much of it shows: of sixteen points round it,
those whose depth is the sky's or as far (over 250 m; the sky is drawn with
a depth of its own), so it dims behind trees, hills and buildings and
flickers through gaps. Then a small hot core, a warm haze round it, four faint
ghosts of the lens along the line from it through the middle of the picture,
and the blacks raised a little; faded out as the sun goes off the screen, none
when it is behind. `NFS_SUN_GLARE` (1; up to 3 stronger, `0`/`off` for none).
`NFS_GLARE_TEST=1` puts the sun straight ahead of the view, for testing.

### Grip by terrain

`src/nfs3hp/native_surface.cpp` (`surfaceFrame`, each frame from `noteSun`):
what each of the player's wheels stands on is `sub_429b40`'s -- on the ground
(`[car+0x164]` 0) and with its contact (wheels 0x78 bytes from `car+0x300`;
`[wheel+0x14]`, `[wheel+0x18]`), `[[wheel+0x18]+1] & 0x3f`, else the car's
own `[car+0x18c]`. The game's table of surfaces (16 of 0x24 bytes at
`0x553090`) has the paved ones 1 in their first dword. Seen: 1 the road, 10
and 12 paved verges, 3 Atlantica's grass, 13 Redrock's sand and dirt, 15
Summit's snow (2 grass, 8 and 9 dirt, 5 mud from the table).

The player's car, and with `NFS_GRIP_ON_OPPONENTS` (on) every other car in
the race too -- racers, cops and traffic, the list at `[0x5efc48]`
(`[0x5efda0]` of them), their driving left as it is -- and gently, never
enough to lose it. Each tick of
the race's clock (64 a second), by the share of the four wheels on each
surface, none below 15 km/h and all of it from 32 km/h:

| Surface | Drag (speed lost) | Steering softer | Slip kept | Shake |
|---|---|---|---|---|
| Grass | 0.030 | 0.9 | 0.10 | 0.10 |
| Sand, dirt, gravel | 0.020 | 0.5 | 0.25 | 0.12 |
| Snow | 0.017 | -0.45 (sharper) | 0.45 | 0.06 |
| Mud | 0.045 | 1.0 | 0.15 | 0.08 |
| Other, not paved | 0.015 | 0.4 | 0.10 | 0.08 |

- **Drag**: the speed along the ground less, each second, drag times the
  speed and 10 m/s more (to 40). Tested on Redrock: after 4.5 s of full
  throttle 24.6 m/s on asphalt, 23.2 as if on grass, 22.3 as if on mud, and
  slowing down faster off the throttle.
- **Steering**: the yaw rate (`[car+0xe8]`) less by this fraction a second;
  on snow (below 0) more instead -- a little oversteer, only while turning
  and below 1 rad/s. Tested with every surface as snow on Redrock: the
  same steering gave 0.26 rad/s of yaw against 0.21, twice the sideways
  slide (1.4 m/s against 0.7), and the car straightened as before; about 4%
  slower after 4 s of throttle.
- **Slip**: of the sideways speed (along `[car+0xc0]`) the tyres took away
  since the last tick, this fraction given back -- at most 0.3 m/s a tick,
  and never past 2.5 m/s sideways.
- **Shake**: up and down by up to this many m/s, and a hair of yaw, more
  with speed.

`NFS_SURFACE_GRIP` (1; up to 3 stronger, `0`/`off` for none).
`NFS_SURFACE_TEST=grass|dirt|snow|mud` treats every surface as that one -- its
grip, and the marks, dust and frost it throws -- and
`NFS_SURFACE_TRACE=1` (or a number of ticks between lines) logs the wheels' surfaces (`[SURFACE]`), for testing.

### Spike strips (in the making)

`src/nfs3hp/native_pursuit.cpp`, off unless `NFS_SPIKE_STRIPS=1`: a cop that
has kept ahead of the player lays a 7 m strip across the road two car lengths
behind it and waits beside one end; each wheel that crosses it counts, and a
car slows (top speed 12% and the speed it gains 15% less a tyre) for 8 seconds
and 4 more a tyre. It works in a test (`NFS_SPIKE_TEST=1`), but neither the
strip nor the waiting cop shows on the screen yet, so it stays off.

### Traffic lighter, cops heavier and faster

A car's mass is at `[car+0xb0]` with 1 over it at `+0xb4`, its three moments of
inertia at `+0xf0..+0xf8` with theirs at `+0xfc..+0x104` (read from a race:
1472 kg the player's car, 1700 and 1800 two traffic vans, 1668-1672 the cops).
The first time a traffic car (`[car+0x200] & 0x10`, set by `sub_428700`) or a
cop (`& 0x20`) is driven, `aiDrive` scales all of them -- traffic by
`NFS_TRAFFIC_WEIGHT` (0.5), cops by `NFS_COP_WEIGHT` (1.25) -- and again
whenever its mass is not the one it set. The game's driving of these cars
(`sub_4071a0`) never reads the mass: they are pushed by their pull, not driven
through their wheels, so only how they hit and are hit changes.

The cops are also faster (`copTune`, `NFS_COP_PACE`). An opponent's pull is
read from its car's table by its speed (`sub_4064f0`: `[car+0x528]+0x2ec`, a
float for each m/s to 111, at `[car+0x7c4]`), and it aims for a speed the game
sets (`[car+0x7b0]`). A cop's pull is the table read at its speed over `top`
-- the curve stretched up the speeds, so it keeps pulling where the stock car
runs out -- times `pull`, and its aim is times `top`: the Pursuit Corvette
(`traffic/pursuit/0005`) 1.20 and 1.12, the Pursuit Diablo (`0001`) 1.15 and
1.15, the rest of the force 1.10 and 1.08. Which car by the folder the race
gave it (`[car+0x1e4]`). On Redrock's Hot Pursuit the patrolling cops' top
speeds went from 34.6-37.3 m/s to 37.4-41.5 m/s.

### Speed you can feel

**The camera pulled back.** `sub_422850` turns a camera -- its place, then its
axes a row each: right, up, the way it looks -- into a view's matrix.
`cameraToView` (native_effects.cpp, hooked at its top) moves the first
player's main chase camera back along the way it looks, and up a quarter of
that, before the matrix is made, so everything after -- culling, the copy of
the view the game keeps -- agrees. How far eases towards the throttle's share
(`0x79c339`, 0 to 255) of `NFS_CAMERA_PULL` (1.2 m), out over about a second
and in over half of one, by the race's clock. A chase camera is one 3 to 15 m
from the car and looking at it: not the cockpit or bumper views, not the
replay's television cameras.

**Motion blur.** In the whole-picture pass (`motionBlur`), the world streaks out
from the middle at speed, as a star field does. A band down the middle of the
picture is left sharp: the whole width at 80 km/h and under (nothing blurred),
narrowing with the first player's speed (`[car+0xb8]`, sent each frame by
`colourGradeTick`), smoothstepped, to the middle half at 250 km/h and over (none at night, `[0x6fd4c8]`) -- the outer
quarters streaked. A pixel in a side band is averaged with eight samples along
the line back towards the middle, their start jittered per pixel; the streak is
longer the further past the band's edge, the faster (the speed's share times
`NFS_MOTION_BLUR` times 2) and the nearer the pixel (20 m over its distance, 0.3 to
1.6), at most 6% of the view's height. Never blurred nor taken into a blur: the
cars (0 in the picture's alpha), the lamps (the pixel itself, where the light
layer is lit, while it is drawn) and the sky, so the sun and its glare (added
after the blur) stay sharp. An ellipse round the middle came first; the bands
blur half the picture at full speed instead of more than four fifths, which is
most of the pass's cost. `NFS_BLUR_TRACE=1` logs the speed, strength and band
once a second (`[BLUR]`); `NFS_BLUR_TRACE=2` draws what the blur did in
colours (dark: sharp; green: blurred, brighter the longer; red: a car; blue:
the sky; yellow: a lamp; a magenta tint: no blur sent).

The speed sent used to be 0 on any frame where the race's clock (64 ticks a
second) had not moved since the last one -- at a frame rate over 64 most
frames, so the blur was mostly not there. It now keeps the last speed, and goes
to 0 only once the clock has stood a quarter of a second (a pause, the
countdown).

### The lamps' glow, and their trails

**A soft glow.** The game draws a lamp's glow -- head and tail lights, the
street's lamps -- as a sprite added onto the picture: `glw0` (64) and `glw3`
(32) from `gamedata/render/pc/sfx.fsh`, grey with its alpha the same as its
grey, tinted red for the tail lights by the vertex colour. Both are a star of
hard spikes round a white dot. They arrive twice: in 32 bits (8888), and as
the race's own copy in 16 (4444), which is the one the cars' lamps use.
`setTextureData` knows them by their texels (FNV hash: 8888 `0xfe572da0` and
`0xe0a827c2`, 4444 `0x0ddae155` and `0xedcd8669`); a 4444 one is widened to
8888 first, so that the new falloff has the steps it needs. `softenGlow`
redraws each as a hot core in a wide, slow falloff, gone before the circle's
edge, with 12% of the star left in, and builds every smaller level the game
supplied again from the first (the GPU uses those for a lamp drawn small, and
the game's kept the star). As what such a sprite adds is its texel squared,
the texel is the square root of the light wanted.

**The halo.** The glow textures are marked (`Texture::light`), and every batch
drawn with one (`Batch::light`) is drawn a second time onto a layer of the
lamps alone (`m_lightLayer`, full size, with levels; it shares the picture's
depth buffer, so a lamp behind a car stays hidden). In the whole-picture pass
the layer's smaller levels (2, 3.5 and 5) are added back round each lamp, times
`NFS_LIGHT_GLOW` (1) by 0.8, and the layer is emptied for the next frame.

**Tail lights' trails.** `lightGlow` (native_vertices.cpp, sub_492370's
stand-in, which `sub_4b8740` calls for a car's lamps) draws, after a red
glow, the same glow again at up to 16 points behind it along the car's way
(`[car+0xa4]`), half a glow apart, each fainter (to nothing) and a little
smaller: where the light was a moment ago, as a long exposure keeps it. The
car is the one of the 16 at `0x5e1120` within 3.5 m of the light (the cars'
lamps are drawn by generated code in a race at extended precision, so there is
no caller to tell). None below 80 km/h; from there the trail grows 0.35 m for
every m/s more, times `NFS_TAILLIGHT_TRAILS` (1), at most 12 m. A point within
3 m of the camera ends it, and it fades over the 2 m before, so the player's
own trail does not blot out the screen. White and amber lamps leave none.

**The streak at speed.** A light going as fast as the camera -- the player's tail
lights, a car alongside -- leaves its trail on the road behind it, which on the
screen lies further from the view's centre than the lamp, on the same line
from it. So each pixel takes the brightest of 16 samples of the lamps' layer
(level 1.5) along that line towards the centre, at its distance from it over
1 + k t, t from 0 to 1, fading as 1 - t, times 1.6, and adds what of that is
not a lamp itself. k is 0.45 times `NFS_LIGHT_TRAILS` (1), coming in from
10 m/s and whole from 35; the centre is the
projection's (`[0x5600a4]`, `[0x5600a8]`). It costs a pass over the screen and
a second draw of the few lamp sprites.

### The cars' paint

A car is drawn twice: its body with its own texture, then the reflection the
game adds over it (blending off, additive) from a sphere map, whose
coordinates `sub_49db70` works out from each vertex's normal turned into the
view (`envMapCoordsNative`). With those turned normals at hand, the port keeps
how squarely each vertex faces the view -- `|z|`, a byte, 1 edge-on to 255
face-on -- for the frame (`noteCarFacing`, `carFacing`), and THRASH's drawing
passes it with the vertex (`thrashBatchVertex`, the vertex's spare third
combine byte; every other vertex carries 0). In the reflection pass the shader
shapes the reflection by it (`u_carShine`): faint where the paint faces the
eye, strong towards the edges as a lacquer's is (a Schlick-like
`0.3 + 2.2 (1 - facing)^3`), and its dim parts brought down and its bright
ones up (`0.45 r + 1.9 r^2`), so the highlights are spots and streaks along
the fenders and the deck rather than a wash over the whole car. The facing is
interpolated across each triangle, so the shine follows the body's curves and
not its facets. `NFS_CAR_SHINE` scales it, 1 by default; 0 is the game's own
reflection. The models themselves are the game's: their polygon counts are
what they are.

### Longer headlights

The headlights' patch on the road (`sub_491bc0`, `roadLightNative`) lit at
most 30 units ahead, a few car lengths, so at night the road went dark just
past the bonnet. Its length and that ceiling are doubled
(`NFS_HEADLIGHT_REACH`, 2) and it is 1.4 times as wide
(`NFS_HEADLIGHT_WIDTH`); 1 and 1 are the original. `NFS_NATIVE_CHECK` reports
the patch as differing from the generated code unless both are 1.

The edge of that patch, the pool the projected headlights lay on the road
(`glw2` in `gamedata/render/pc/sfx.fsh`) and the lights' streaks on a wet road
(`glw4`) were bright to within a few texels of their square, so the lit road
ended in a box. They are recognised by their texels as they are uploaded and
faded to nothing towards their sides and far end (`soften`,
`src/lib/thrashrenderer.cpp`); their reach is unchanged.

### Car shadows

A car's shadow (`shad`, same file) was one hard-edged dark square under every
car whatever the light. Its alpha is now a soft ellipse inscribed in that square
-- gone before the picture's edge, so that no angle the quad takes can show a
corner or a side of it: it was a rounded *square* until 2026-11-17, and a turned,
dark, translucent square the size of a car on a bright road reads as a rectangle
of paint, which is what it was reported as (`NFS_CAR_SHADOW=0` leaves the cars
with no shadow at all). The quad is stretched away from the sun: the sun the game
lights the cars with (`0x55e6a0`) is turned into each car's own axes as
`sub_49da30` lights it (`native_vertices.cpp`), and when THRASH draws the
shadow quad (`stretchShadow`, `native_thrash.cpp`) the corners on the side away
from the sun are pushed out along the quad's own screen edges, so the shadow
follows the road's perspective and turns as the car does. 1/w and depth are
taken from the quad's plane, the corners keep their texture coordinates.
`NFS_SHADOW_STRETCH` (0.7) sets how far; `0` the original shape.
`NFS_SOFT_EFFECTS=0` leaves all three pictures as the game has them.

Where the sun is not shining the shadow is fainter (`dimShadow`): in the rain,
under its overcast sky (Weather, `0x6fd4cc`), its corners' alpha is scaled by
`NFS_SHADOW_RAIN` (0.8), and at night (`0x6fd4c8`) by `NFS_SHADOW_NIGHT`
(0.4) -- both together when both hold; 1 keeps the daylight shadow.

### Wet roads

The game lays its own lights on a wet road: `glw4`, and a 64x64 4444 picture
(texel hash `0xc4bb8dc1`, `kWetLightHash`) in long strips from every car down
to the bottom of the view, added red under the tail lights -- lit or not. Both
are flagged `wetStreak` and, by day (`0x6fd4c8` clear,
`glide2x::wetLightsLit`), not drawn. A batch left undrawn
(`skipDraw`) passes over its vertices too: before, the next batch drew them as
its own -- the dark strips that stayed, and a skipped shadow
(`NFS_CAR_SHADOW=0`) drawn with whatever texture followed it.

At night they are drawn as light (`Batch::wetLight`, as a lamp's glow): no
depth written, nothing cut, and the picture's alpha left as it was. The 4444
strips are added with depth on, and by their format counted as a car's, so
each wrote 0 into the picture's alpha -- the road under it marked as car, left
out of the haze and the shadows -- and stood as darker, solid patches round the
red in the snow and the rain at night (2026-11-18, `NFS_SPRITE_TRACE`: the
strips `fmt 3 blend 0 depth 1/1 car 1`).

Nor do those lights mark the HUD's start (`Batch::wetLight`): the game draws
the red strips after the road unfogged and writing no depth, as the HUD's
first draw is told by, so the whole-picture pass -- exposure, glow, grade --
ran between a car's strips, and the one drawn after it was the darker half of
the streak.

The road stays wet through up to 30 frames of the weather reading dry
(`kDryAfterFrames`, `wetRoadTick`): a single dry frame made the whole road dry
for that frame. `NFS_WET_TRACE=1` logs each time the weather reads dry or wet
again (`[WET] swap N`), a wet frame drawn dry and why, and a race frame whose
HUD was never seen, its reflection taken at the end -- with how many draws the
frame took and what came after the scene, a letter a run (`F` a draw taken for
the HUD's start, `e` a flat draw before the road, `f` fogged with no depth test
or write, `t` depth-tested with no write, `W` the world). `NFS_FRAME_DUMP=<folder>`
saves `NFS_FRAME_DUMP_COUNT` race frames in a row (90) from the
`NFS_FRAME_DUMP_AT`'th (600) at half size as `frame-NNNNN.bmp`, with a
`[FRAME]` line each of how it was drawn.

The HUD's start is the first draw, once some of the road has been drawn
(`batch.detail`), that is unfogged and writes no depth, or neither tests nor
writes depth. In the rain the game draws something flat after the sky and
before the road, for frames on end; taken for the HUD's start, the wet road's
reflection was the sky alone and the whole-picture pass ran before the road,
which was then drawn over the rain's spray and glow -- the road dry for those
frames, found with the frame dump (2026-11-17).

With Weather on, on a track whose weather is rain (the `type` in its
`3trNNw.hrz`; 2, snow, is left dry), the road is wet (`wetRoadTick`,
`src/nfs3hp/nfs3hp_main.cpp`; `NFS_WET_ROADS`, 1, how wet, `0` never). On the
surfaces the grain goes on: darker, with puddles from a second, coarser read of
the grain texture, and a reflection of the last frame. That frame is copied
once on the GPU as the HUD starts (its first unfogged draw that writes no
depth), so the HUD is never reflected. 1/w is linear down the screen over a
level surface and is 0 at the horizon, so each pixel finds the horizon from
its own 1/w and its rate down the screen, and reads the copy at five points
from itself up to its mirror image about the horizon: far things mirror
exactly, near ones blur upward, and lights draw the streaks a wet road shows.
It is strongest at a grazing angle and faint looking down; surfaces whose 1/w
changes across the screen (walls) are left out, as is the top band where the
mirror and the HUD's bar stand.

### Projected headlights

With projected headlights the game draws the lit road a second time over
itself, and that pass hands the Glide layer texture coordinates up to thirty
repeats of the road texture away from the origin. On Mali GPUs that was enough
to lose the fraction of a texel per pixel, and the lit area broke into moving
blocks — the same artefact the Modern Patch shows under dgVoodoo. A triangle
whose wrapped coordinates lie that far out is moved back by a whole number of
repeats before it is drawn (worked out in double, so nothing it samples
changes), and the lit road draws clean. `NFS_TEXCOORD_REBASE=0` turns this off.

The headlights' light pool, a car's shadow and the glow of its lights in the
rain are laid on the road with a less-or-equal depth test. The Voodoo kept 16
bits of depth, and a decal came out equal to the road under it; the finer depth
a phone's GPU keeps differs in the last bits from pixel to pixel, and the decals
flickered against the road and each other. The fragment shader rounds depth to
the Voodoo's 16-bit steps, but only where the driver gave a depth buffer of more
than the 16 bits asked for (Mali gives 24); a real 16-bit buffer already holds
those steps. Never on an Adreno 5xx: there the shader's depth write broke the
depth test, and the wheels showed through the body. `NFS_DEPTH16=0` or `1`
overrides the choice.

### Rear-view mirror

In a single race the mirror is drawn as the main view is: the same draw
distance, the near track model, cars at full size, the sky with its clouds and
lightning, the cars' lights and the headlights' light. The original cut all of
that down for the mirror (a reach of 100 against 500, the far track model, cars
at 0.6 of their size, a flat sky). There is no menu option;
`NFS_MIRROR_FULL=0` brings the original's mirror back (`tools/apply_mirror_detail.py`).

### The song now playing

A second into every race, a small panel slides in at the bottom left: the artist
in yellow and the song in white, in the HUD's own font, on the HUD's navy with
a yellow glow round its edge and the NFS emblem at its left. It stays five
seconds and slides back out; again if the race is restarted; a pause hides it
where it is. Which song is the file the race streams
(`gamedata/audio/pc/<track><rock|tech>.mus`, named by the game itself as it
opens it, `musicStreamFile`): the ten files are the game's ten race songs,
their titles the game's own (`text.eng`), their artists the soundtrack's. The
last song named is kept from race to race: a race on the track just raced may
not open its file again, and it still shows its song. The whole panel, glow
and emblem and all, is 70% opaque (`kOpacity`). The font is `gamedata/dashhud/cp16bi.ffn`
(4 bits a texel, the high half first; the low values are the characters' dark
edges, so it is drawn white with them kept), the emblem cut round out of
`hud98.fsh`'s picture `4444`, both read once
(`src/nfs3hp/native_now_playing.cpp`). The renderer lays it over the finished
frame, HUD and all (`ThrashRenderer::drawOverlay`, `include/lib/overlay.h`:
quads in HUD units, 480 to the picture's height). `NFS_NOW_PLAYING` (on, five
seconds; a number for how many, up to 30; `0`/`off` never).

**The next song.** In a race, `]` stops the music and starts the next of the
ten songs from its beginning (in the order homerock, hometech, lostrock, ...,
emprtech, round again), and the panel shows it at once. The key is bound under
Options -> Controls, a row of its own at the foot of the list, **Next song**,
bound as any other: Enter, then the key. The game's list already filled the
screen with its thirteen rows, so it scrolls now: thirteen show, up or down
past either end moves it a row, and a small yellow triangle of the menus' own
(`yup1`, `ydn1`) just right of the first or last row says there is more that
way (`src/nfs3hp/native_frontend_controls.cpp`: the list's drawing,
`sub_43c900`, and its keys, `sub_43cf80`, decompiled with the extra row; with
two players' columns, split screen, the game's own list as it was). The
binding is kept in `nfs3hp.ini` as the game writes one -- the key's
character, its scan code, 4 for the keyboard: `NFS_NEXT_SONG_KEY =
0x5d001b04` is `]` -- and the key is read from the game's own table of keys
held (`[0x564818 + scan code]`). Starting a song is the game's own, as its
audio screen does when the music is turned back up: `sub_4109d0` stops the
music, `sub_4107c0` starts the track's with the race music's volume, and the
name it builds -- folder, track, rock or tech, then the index's extension and
the stream's -- carries the next song's instead (`musicIndexOpened`). Calling
the game needs the guest's own stack, so it is done right after a frame's
swap, from the swap's call (`glide2x::setSwapGuestObserver`), every register
put back after. The game's stop fades the music out and then waits for the fade
(SNDstreamautovol to nothing, the sound's timers run until its level is 0),
which held the game a second; so the fade is started on the key, left to run
over the frames that follow, and the stop and the start come once it is out
(at most 3 s) -- the old song fading, the game never held. Pressed again
meanwhile, it goes a song further. Not while paused, nor with the music off.

### Reading the game's UI art

What the port draws in the game's own style it takes from the game's files,
read as follows (all little-endian).

- **Fonts, `.ffn`** (`gamedata/dashhud/*.ffn` for the race's HUD,
  `fedata/text/*.ffn` for the menus'): `"FNTF"`, the file's size (+4), how
  many characters (+8, u16), the line's height (+0x12, u16), the picture's
  offset (+0x1c, u32). From +0x20 a record of 11 bytes per character: its code
  (u16), width and height (u8 each), where it is in the picture (x, y: u16
  each), its advance, its offset from the pen and its offset from the line's
  top (s8 each). The picture: a 16-byte header -- 0x7a, width at +4, height at
  +6 -- then 4 bits a texel, the first texel in the high half of the byte. A
  texel is how bright, 0 drawn not at all: the characters are white with their
  low values round the edge, so drawn in a colour they keep the dark outline
  the HUD's names have. `cp16bi.ffn` is the HUD's bold italic at 16 points,
  `cp1bi.ffn` the same at half the size, `small6.ffn` the smallest.
- **Pictures, `.fsh`** (`"SHPI"`): the file's size, how many pictures (+8),
  then from +16 a name of four letters and an offset each, 8 bytes. A
  picture's 16-byte header: its format (the low 7 bits of +0 -- 0x7d 32-bit
  BGRA, 0x7e 1555, 0x78 565, 0x6d 4444, 0x7f 24-bit, 0x7b 8-bit with its
  palette attached after it), width at +4, height at +6; its pixels follow.
  The name says nothing of the format: `hud98.fsh`'s `4444` is 32-bit.
- **`.qfs`**: an `.fsh` packed with EA's RefPack (`refpackNative`,
  `native_loading.cpp`, does it for the game).

`tools/export_textures.py` writes every picture of the game out as a PNG, a
folder per file, to look for one. Found that way:

| What | Where |
|---|---|
| The NFS emblem, the ring and the letters, on a blue swoosh | `gamedata/dashhud/hud98.fsh`, picture `4444` at (130, 65), 64 square -- the race's own HUD art; also `fedata/art/mmnu.qfs`, `popu` (198x70, the menus' pop-up's) |
| Small triangles, 12 square: yellow up and down, blue up and down | `fedata/art/mmnu.qfs`: `yup1`, `ydn1`, `bup1`, `bdn1` |
| The menus' glowing yellow outlines (a square, triangles each way) | `mmnu.qfs`: `int2`..`int9`, `inta`.. |
| The HUD's bars (navy, a grey bevel) | `hud98.fsh`, `4444` from y 128 |
| The Controls list's row bar, and its blink while a key is waited for | `mmnu.qfs`: `con2`, `con1` |

**The Controls list.** A menu file's sections are typed by name (`button`,
`slider`, `cascade`, ... `controls`); the table at `0x557b6c` gives each type
its number, name and handlers -- `controls` is 0x14, its handlers
`sub_43d310` (made: the layout's globals -- row height `[0x555608]`, 25 or
22 in a race, the key column `[0x5f3d9c]` -- and the bars' images), `sub_43c900`
(drawn), `sub_43cf80` (a key) and `sub_43d2b0` (its box: 13 rows high). Its
thirteen rows are each player's control set (`sub_4904d0`: 13 dwords a
player, the second player's 0x34 on), named by lines 0x46 to 0x51 and 0x1bc of
the language's text in an order of their own, each row indented by the curve
at `[0x55560c]`; the selection is the row under the menus' pointer
(`sub_49c1c0`), and up and down move the pointer. A binding as the game keeps
it: a key's character << 24, its scan code << 8, 4 for the keyboard
(`sub_43cda0` makes one of a key; `sub_43c110` draws its name). The port draws
and keys the list itself, a row more and scrolling
(`native_frontend_controls.cpp`; The next song, above).

### Network races

The game's Connection screen offers IPX and TCP/IP; RaceNet, Modem and Serial
are hidden, as nothing carries them any more.

- **IPX** finds races on the local network, phone with phone. It is carried on
  UDP: an IPX socket is the UDP port of the same number (the game's 0x452 is
  UDP 1106), a node is the phone's IPv4 address, and a broadcast goes to every
  network's broadcast address. Only the port speaks it; a PC would need an IPX
  wrapper.
- **TCP/IP** connects to an address: phone with phone, or phone with a PC
  running the Modern Patch (tested both ways with 1.6.1). The port is 9803, as
  the Modern Patch has it; the launcher's Network screen shows the phone's
  address and switches to the original's 1030 (`NFS_NET_PORT`).

A race is lockstep: every machine simulates every car from the players'
inputs, so the port works the x87 as a PC does in a race (every player needs
the same precision, see below), and the game's
version is checked when a machine joins. The port reports nfs3.exe's own,
"27, 2.0" — the version resource the phone has no copy of, and the string the
Modern Patch keeps in its data. Winsock 1.1 is in `src/lib/winapi/wsock32.cpp`
over the phone's sockets (`src/lib/socket.cpp`); `NFS_NET_TRACE=1` writes a
join into the log in a debug build.

### The x87's precision

A PC of 1998 raced in single precision: before every race the game takes the
x87's precision control down to 24 bits (fldcw at 0x4a3e8d and 0x4a3ed3), and
every sum, product and quotient of the physics and of the opponents' driving
keeps a float's significand. Four functions that step out to the system mid-race
come back the same way (0x495179, 0x4955e9, 0x4b6239, 0x4bc811). The Modern
Patch drops that fldcw and races at the 64 bits fninit leaves. The port does as
the Modern Patch does at all six (`tools/apply_race_precision.py`);
`NFS_FPU_EXTENDED=0` brings back the original's single precision. Every machine
of a network race needs the same.

How wide "extended" is depends on the build. The default keeps the x87's
values in doubles: 53 bits. `WITH_PEDANTIC_FPU` (`./gradlew -Px87Exact
assembleDebug`) keeps them in 80 bits and does the x87's arithmetic in portable
C++ (`src/lib/x87soft.cpp`): addition, subtraction, multiplication, division,
square root, comparisons, frndint, fscale, fprem and every conversion come out
as an x87's to the bit, at every precision control and in every rounding mode
(`tools/x87soft_checks.cpp` compares a million random operands of each with a
real x87), and fsin, fcos, fptan, fpatan, fyl2x and f2xm1 within a last bit of
it — an arm64 long double has 113 bits, and the sine and cosine reduce their
argument by the x87's own 66-bit pi. It costs speed: every x87 operation of
the generated code is a call.

In an extended race the natives that only draw stay on; the ones the physics
and the opponents can reach — the vector helpers, the nearest block, the
ground's height, the particles — step aside for the generated code.

## Native code

The game runs on a virtual x86 CPU, and the parts that cost the most no longer
go through it:

- voodoo2a, the THRASH driver behind the game's renderer, is native whole and
  has no generated code: `disassemble_nfs3hp.py` binds its 34 exports to the
  port's functions of the same names (`src/nfs3hp/native_thrash.cpp`, its heap
  -- Watcom's, the game's too -- in `native_watcom_heap.cpp`) and writes only
  its data. It draws every way voodoo2a does -- Glide's own triangles and
  lines, antialiased, two passes for trilinear mipmapping -- through its own
  renderer (`src/lib/thrashrenderer.cpp`): an OpenGL texture for each of the
  game's, the texture formats as they are, hardware filtering.
  `tools/thrash_checks/run.sh` compares every export with voodoo2a's generated
  code, made afresh from the DLL, on random states, to the byte.
- softtria, the software renderer, is not built at all -- no code, no data,
  no library. The game loads one renderer, the one the registry's "Thrash
  Driver" names (`sub_4b5720`) unless a command-line option names another;
  the port sets voodoo2 and passes no options. The file itself stays among
  the bundled ones, which the game's integrity check looks for.
- About fifty of the game's own functions are native
  (`src/nfs3hp/native_vertices.cpp`): turning and projecting vertices,
  clipping, the track's and objects' polygons and their depth sorting,
  particles, the cars' lights and detail, and the small vector helpers called
  from everywhere. `tools/apply_native_vertices.py` hooks each at the top of its
  generated function, which stays behind it as the fallback.
- The rest of the drawing goes native function by function
  (`src/nfs3hp/native_render.cpp`, hooked by `tools/apply_native_render.py`):
  so far the clipping of a triangle whose corners carry z (`sub_4bf790`), a
  quad of them (`sub_4c20e0`), the triangles and quads of a polygon list
  (`sub_434120`, `sub_4332b0`, `sub_433e30`), a triangle of one past the near
  plane (`sub_4c3ad0`), the subdivision of a large quad (`sub_433060`,
  `sub_432720`), the sky (`sub_47a880`, `sub_47a190`, `sub_47a1f0`,
  `sub_47b850`, the lightning `sub_4946e0`), the solid lettering and the
  countdown (`sub_475b50`, `sub_482620`), the cars' lights (`sub_4b9bc0`,
  `sub_4bae00`, `sub_4bb200`, the colour at night `sub_4b97b0`), a view's
  pass (`sub_41b9b0`), the choice of polygon drawers (`sub_434870`), colours
  under the sun's glare (`sub_49d800`), the detail settings (`sub_472d10`),
  the sparks (`sub_4ddb40`), a light's glow (`sub_491190`), vertices under the
  glare (`sub_47b7d0`), the 2D buffers cleared (`sub_4cb670`), a 2D screen's
  bouncing pictures (`sub_4a4410`), a headlight's patch on the road
  (`sub_491bc0`), the race's views and their records (`sub_41d960`,
  `sub_41df10`), a view's smoke and spray streaks (`sub_4de070`,
  `sub_4de700`), the views' distances for View Distance (`sub_41d620`), the
  transform buffer (`sub_4b56e0`), a screen mode (`sub_4bef50`), a driver
  started (`sub_4b59c0`), and in the HUD an element's frame (`sub_47f650`),
  its rectangle in pixels (`sub_480910`), the elements laid out inside a frame
  (`sub_480fc0`, `sub_4812e0`, `sub_481aa0`), the standings' and speeders'
  tables (`sub_481c50`), a frame's inset (`sub_487210`), the start lights
  (`sub_48c7c0`), the cockpit's needles, bar gauges and overlay (`sub_4880b0`,
  `sub_488470`, `sub_489240`), a clipped 3D line (`sub_4c0b20`), the segment
  meter (`sub_48c0e0`), the band behind the HUD in split screen
  (`sub_4800a0`) and the HUD's own sequence (`sub_482b00`), with the HUD's and
  the cabin's ports kept; and, while a race loads, a car's textures
  (`sub_4b7d30`), the 2D pictures made textures (`sub_4d0a10`, `sub_4d3f50`,
  `sub_4d41a0`), the cabin's pieces and bars (`sub_4d4580`, `sub_4d4ae0`), the
  loading screen (`sub_494cb0`), with the car detail and the loading screen
  at 4:3 kept. The movie player (`sub_495bc0`) stays the port's own MAD
  player (`src/nfs3hp/native_movie.cpp`), not this one.
- eacsnd, the sound driver, is native whole and has no generated code:
  `disassemble_nfs3hp.py` binds its sixteen exports to the port's functions of
  the same names and writes only its data. The driver proper
  (`src/nfs3hp/native_sound.cpp`) puts what the game mixes straight onto an SDL
  audio stream, with no DirectSound and no ring buffer in between; the other
  twelve exports -- the version, the game's functions, recording and the 3D
  buffers -- are in `src/nfs3hp/native_isnd.cpp`, which
  `tools/sound_checks/run.sh` compares with eacsnd's generated code, made afresh
  from the DLL, on random states, to the byte.
- The game's side of the two drivers, and the logger they report through, is
  native and has no generated code: `disassemble_nfs3hp.py` binds the
  twenty functions of its `GAME_NATIVES` to the port's functions of the
  same names (`src/nfs3hp/native_game.cpp`), and the generated code calls
  them by name. They are `THRASH_opendll`, which loads the renderer and looks
  up its 34 functions; `iSNDloaddll`, `SNDcaps` and the focus and
  serve handlers, which load and drive eacsnd; `Error` and `Message` -- and the
  rest of what `iSNDloaddll` hands eacsnd, among it the sample converter --
  and the game's own fill at its three entries, which its code clears and
  fills memory with in hundreds of places.
  `tools/game_checks/run.sh` compares each with the game's code for it, made
  afresh from nfs3.exe, on random states, to the byte.
- The C runtime the game was linked with, Watcom's, goes native family by
  family (`src/nfs3hp/native_watcom.cpp`, in `nfs3hp::watcom`, bound as the
  game's own are, `WATCOM_NATIVES`). `src/nfs3hp/watcom.md` maps all of it in
  nfs3.exe -- 275 functions among EA's libraries, by Open Watcom's names --
  and says which are native: all of it now -- the string and character functions,
  `atoi`, `rand`, `memset`, `__CHP`, the step before every `fistp` of the
  game, `qsort` and `bsearch`, `errno` and the functions on paths --
  `unlink`, `mkdir`, `chmod`, `access`, `getcwd` and the like -- and on
  handles -- `lseek`, `__qread`, `__qwrite`, `isatty` -- time -- `time`,
  `localtime`, `mktime` and daylight saving's rules -- the x87's `floor`,
  `ceil`, `modf`, `exp`, `acos` and `atan2`, the streams -- `fopen` and
  `fclose`, `fgetc`, `fgets`, `fread`, `fputc`, `fputs`, `fwrite`, `fseek`,
  `ftell`, `fflush`, the buffer under them and the handles under that --
  the formatted output -- `sprintf`, `vsprintf`, `fprintf`, `vfprintf` and
  `__prtf` under them, all of it but a float's conversion, which goes to the
  x87's formatter -- the environment, the time zone and the way out --
  `getenv` and the multibyte functions it compares with, `__tzset`, `exit`,
  `atexit`, the finalizers, `__GRO`, `sleep`, temporary files' names,
  `setenv` with the wide environment -- the formatted input -- `sscanf`,
  `__scnf`, `strtod` -- the console's `getch`, `getche` and `kbhit`, the
  structured exception handling, the start-up from `_cstart_` to the game's
  main, the Pentium FDIV bug's atan2 and divisions, `__reporterror` -- and
  the heap, `_nrealloc` and `_nexpand` with it --
  `malloc`, `free` and what grows and shrinks it, in `native_watcom_heap.cpp`,
  which voodoo2a's heap is too.
  `tools/game_checks` holds them against their generated code as well.
- All of the game's code that uses MMX is native
  (`src/nfs3hp/native_mmx.cpp`, in `nfs3hp::mmx`, bound as the game's own
  are, `MMX_NATIVES`): the SND library's MMX routines -- a block added into
  the mixer's accumulator at its volume, the accumulator out to 8 or 16 bits,
  the interpolators, the XA decoders -- and the memmove the game's file reads
  copy with. They work on the CPU's MMX registers as their code does, leaving
  them as it leaves them, with `include/mmx.h`'s MMX in plain C++;
  `tools/game_checks` holds them against their generated code, the MMX
  registers too, and `tools/mmx_checks` holds `mmx.h`'s MMX against the
  x86's own. The disassembler writes MMX with it as well
  (`disasm/codegen/mmx.py`): there is no SSE anywhere, and on ARM nothing to
  translate.
- The game's controls are native whole, in `nfs3hp::controls`, bound as the
  game's own are (`CONTROLS_INPUT_NATIVES`, `CONTROLS_DEVICES_NATIVES`,
  `CONTROLS_RACE_NATIVES`, `CONTROLS_PIPELINE_NATIVES`): its side of DirectInput -- DirectInput started and
  stopped, the mouse opened and read, the joysticks found, described, read and
  acquired, their dead zones and saturation, force feedback's gain, actuators
  and its 30 effects created, changed, started and stopped
  (`src/nfs3hp/native_controls_input.cpp`); the control sets the game makes
  for each player's device, by its kind or for the seven it knows by name
  (`native_controls_devices.cpp`); the controllers kept in `config.dat`
  matched with the ones found, and the race's force feedback -- the car whose
  player has the force controller, its jolts, the engine's and the road's
  rumble, the steering spring, every frame (`native_controls_race.cpp`, with
  the port's change of `tools/apply_force_feedback_car.py` in it); and a
  race's controls every frame -- the players' sets laid out by action as the
  race starts, the joysticks read, each control held or how far, and each
  player's four bytes made of them (steering, throttle, brake and a bit for
  each of 8 more actions, at 0x79c338 and 0x79c33c) and handed to the race's
  queue of inputs that feeds the cars (`native_controls_pipeline.cpp`). Each is
  written as its code is, and `tools/game_checks` holds them against their
  generated code, with DirectInput's objects, methods and results stood in.
  There is no DirectInput under them: each call the game made of it --
  DirectInputCreateA, each method of its objects -- is
  `controls::directInput`, which the port answers itself
  (`src/nfs3hp/native_controls_host.cpp`) from its pads, mouse and keyboard
  (`lib/gamepad.h`) and its force feedback (`lib/forcefeedback.h`), the
  objects only handles the game keeps where it kept them; `tools/game_checks`
  answers it as the game's code did, through the objects' tables
  (`controls_host.cpp`).
- The game's sound mixer (`src/nfs3hp/native_mixer.cpp`): all of a sample's
  way through EA's SND library -- PCM and XA ADPCM decoding, pitch, volume and
  pan, the race's reverb, the 16 channels added up -- done as the x86 does it,
  to the bit. Starting and stopping sounds and reading streams stay the
  game's. `NFS_SND_MIX=0` goes back to the generated mixer; `NFS_SND_CHECK=1`
  mixes every block both ways and compares them.
- The loading of the game's data (`src/nfs3hp/native_loading.cpp`, hooked by
  `tools/apply_native_loading.py`): EA's RefPack decoder behind every `.qfs`,
  the alpha scan each FSH texture gets in each of its pixel formats, an FCE
  model turned into the game's polygon records, and a track's FRD read into
  its blocks. The formats are the ones OpenNFS reads; the structures they go
  into are the game's own, filled as the generated code fills them.
  `tools/loading_checks/run.sh` builds them on the desktop with the generated
  functions they stand in for and compares the two on made-up data, without
  the game.
- The movies (`src/nfs3hp/native_movie.cpp`): EA's MAD player, decoder and
  display, decompiled to C in `decomp/mad`. The player calls the rest of the
  game -- streams, sound, memory, clock, keyboard -- as the original did; each
  frame goes to the screen in full colour, scaled on the GPU, instead of
  through the 16-bit surface.
- Two functions of the opponents' driving are decompiled into C++ to be read
  and changed (`src/nfs3hp/native_ai.h`): sub_4064f0, the pull of an
  opponent's engine at its speed, and sub_4070c0, how far the way it wants to
  go is from the way it points. They come out as the generated code to the
  bit in every precision and both builds (`tools/native_ai_checks.py`);
  `NFS_NATIVE_AI=0` runs the generated code instead.

Each was compared with the generated code bit for bit before it was switched
on — memory, registers, flags, the FPU and the triangles drawn
(`NFS_NATIVE_CHECK`) — and keeps the x87's single-precision rounding. In a race
this is about three quarters of the game thread's time. `NFS_NATIVES=0` goes
back to the generated code, `NFS_THRASH_GL=0` to the old renderer.

### The car's own structure

The race's cars are sixteen structures of 0x9ac bytes from `0x5e1120`, in the
order the race made them (`car = 0x5e1120 + slot * 0x9ac`, slot 0 the first
player's). Everything about one -- where it is, what it is doing, how it is
dressed -- is in there, and most of the port's hooks read it directly. The
fields it reads, and what each is:

| at | what | how it is known |
| --- | --- | --- |
| `+0x98` | its centre, three floats: x, y (up), z | every world-to-screen of a car's own features starts here (`noteSun`, `native_particles.cpp`) |
| `+0xa4` | its velocity, three floats, m/s | Slipstreaming's, and the dust the wheels throw |
| `+0xb0`, `+0xb4` | its mass, and 1 over it | Traffic lighter, cops heavier and faster |
| `+0xb8` | its speed, m/s (kept up for the player's car alone) | Motion blur |
| `+0xc0`, `+0xcc`, `+0xd8` | its axes, three floats each: right, up, forward | a place in its own space is `centre + right*x + up*y + forward*z` (the exhaust flames are placed that way) |
| `+0xf0..+0xf8`, `+0xfc..+0x104` | its three moments of inertia, and 1 over each | Traffic lighter, cops heavier and faster |
| `+0x15c` | how far it is sliding, m/s | Collisions' effects |
| `+0x164` | held (a word, nonzero when it is not on its wheels) | `steeringAmount`, and lead-footed cornering |
| `+0x168` | the speed it lost into what it hit, m/s | A crash's note, its sparks |
| `+0x1e4` | **its body name**, a C string | `strcpy` of `sub_4392b0(id)` at `0x42876d`, once, when the car is set up -- see below |
| `+0x200` | flags: bit 3 (`8`) a racer, `0x10` traffic, `0x20` a cop | `sub_428700`, read by the AI and the cops' pace |
| `+0x220` | its race record (`0x6fd52c` + `0x6c` a car), or 0 when it is not in the race | The grid |
| `+0x4f4` | its speed as the game shows it, m/s, signed (set from `+0x7c0` when a race is made) | `carSpeedMph` (`0x472110`) makes it positive and times it by 2.24, mph |
| `+0x50b` | its gear, a byte: 1 neutral, 2 first forward, 0x64 "none yet" | The gearbox's kick, and the exhaust flames |
| `+0x528` | **its data table**, the car's spec as loaded: the redline in rpm at `+0x1c4`, the engine's minimum rpm at `+0x1c0`, its pull by speed from `+0x2ec` | the drivetrain (`sub_4061b0`) and the AI's pull (`sub_4064f0`) |
| `+0x590` | its engine's revs, rpm | The rev counter, the gearbox's jolt, its rumble, and the exhaust flames at a standstill |
| `+0x5c4` | something worked out of `+0x4f4`, per tick | not identified |
| `+0x7b0` | the speed it is driven to, m/s (an opponent's aim) | The cops' pace |
| `+0x7c4` | its own speed, m/s, as the drivetrain uses it | the AI's pull table's index |
| `+0x300` | **four suspension records**, 0x78 bytes each | `sub_429b40` (`wheelSurface`), the grass the wheels throw and the light the exhaust flames pool on the road |
| `+0x920` | its wheel's steering | `steeringAmount` |

**A wheel's record** (`car+0x300`, 0x78 bytes each, `car+0x300+n*0x78`) keeps
where that wheel touches the road at `+4`: **three 16.16 fixed-point ints**, x, y
(up), z -- whole units of 1/65536, which the game itself `fild`s and multiplies
by 1/65536 (`sub_4ca160`, the dust its own wheels throw, at `0x540380`). Read as
floats they are denormals, and anything placed by them lands at the world's
origin, so the port converts them (`wheelContact`, `native_particles.cpp`). The
surface the wheel stands on is a word at `+0x14` (nonzero when it has one) and a
pointer at `+0x18` whose byte at `+1` holds the surface's number, `& 0x3f`
(`sub_429b40`). The grass the wheels throw, the smoke's test for whether a wheel
is on grass, and the pool of light under an exhaust flame are all placed
off them.

**Its body name.** A car does not know its own folder: `sub_4392b0(id)` answers
that for whatever its race record's id (`[record+0x14]`) is, and what it
answers is the name the game builds the car's file names from (`%s\car.fce` and
the rest, `0x43b2e0`). A car whose id has a record in the game's own table
(`sub_439a50`, `car_slots.h`) gets that record's name at `+0x110` -- for a racer
its folder, `diab`; one with no record of its own, traffic and the cops, gets a
body code out of the game's tables instead (`0x55cfb0` for traffic, `0x55c92c`
for the pursuit cars: `0005`, `vett`, `cagli` -- the same bodies the racers are
made of). The car keeps a copy of that answer at `+0x1e4`, so it is there for
the asking without the id being looked up again, and every car of the race has
one. The exhaust flames find a car's exhausts by it (Exhaust flames when
gearing up); the AI's trace prints it; the cops' pace is tuned by it.

### The opponents' driving, tuned

Read from the generated code: every frame `sub_4088f0` sends an opponent to
`sub_408600`, which picks how it drives -- `sub_4076b0` at speed, `sub_407c30`
slowly or in reverse (while `[car+0x7d4]` is 1), `sub_407fa0` off its line or
sliding -- and `sub_4071a0` turns the force and torque they leave into velocity
(`+0xa4`) and spin (`+0xe4`): an opponent is pushed, not driven through its
wheels. It reverses (`sub_405fd0`) only when a collision says so, and a
collision while it reverses with no time set turns it forward again, so two
cars touching flipped each other back and forth and never parted -- the clumps
that sat in a pile. Nose to a wall with nothing at its side, it pushed on. The
game puts a car back on the road (`sub_429100`) only after nine reverses, or
off the road after 1000 frames under 3 m/s -- half a minute.

`aiDrive` (`src/nfs3hp/native_ai.cpp`, entered at the top of `sub_408600`)
watches each opponent's progress along the track (its node, `+0x1c`). One that
has stopped getting anywhere -- slow at the same node, or turned well away from
the road -- first steers for another line across the road (the lane the game
gives it, `+0x7b4`, held for four seconds): away from the car ahead of it if
there is one (from the cars' positions, `+0x98`, against its own axes), or a
few metres aside, to the roomier side first, if there is only road ahead -- a
post, a wall, the island between the two ways of a fork. It gets two chances
at that before it reverses, on the game's own reverse with a time to stop at,
which collisions do not cancel, each driver for its own time so a clump comes
apart; reversing straight away, in a crowd or in the wet, only backed into the
next car. Still stuck after three tries, the game's own counters are run up so
that it is put back on the road at once.

Splits ahead are seen before they are hit. Each track node (`[0x5dd948]`, 36
bytes) has at `+0xc` a word that is the road across it in slices, a bit each,
set where there is road: the lowest set bit at the right-hand edge (`+0x20`),
the highest at the left-hand one (`+0x1c`), evenly between. Most nodes are one
run of set bits; a fork, an island or a pillar is a gap in it (the game's
camera looks for exactly that, `sub_423fe0`; its opponents never did). Each
opponent looks about two and a half seconds down the road (`forkAhead`), and
when the line it aims for would cover a gap it keeps to the nearer way round,
as close to its line as that leaves room for, until it is past.

Held up right behind the player -- 3 to 16 m behind, in the same lane, facing
the same way, wanting to go faster than it is, with no other car in between --
for two seconds, an opponent flashes its lights (`signal`): its headlights,
`[car+0x660]` (0 off, 1 dipped, 2 full beam; set at the start of the race by
`sub_42bcb0` and left alone after for an opponent), flick to full beam and
back four times, about a tenth of a second each, then go back as they were.
Three seconds on and still held up, it does it again.

A notch harder: every opponent's engine pull (`aiPull`) is times
`NFS_AI_PACE` (1.06), and the speed it aims for at each point of the track
(`+0x7b0`, from `sub_409330`) times half that again. And each driver -- the
racer in its slot (`+0x1f0`), one driver to a slot whatever car it drives --
has a character of its own (`kDrivers`): its pull, how fast it takes the
corners, how soon it gives up on a position it is stuck in and how long it
backs off -- a hothead, a smooth one, a late braker, a steady one, a bully, a
cautious one and an opportunist. The aim is written once for each value the
game writes, so it never builds up.

Network races are left as the game drives them (every machine has to drive the
opponents alike), and everything runs off the race's own clock
(`[0x7d3684]`, 64 ticks a second). `NFS_AI_TUNE=0` turns all of it off;
`NFS_AI_TRACE=1` logs each opponent once a second and every step it takes to
get unstuck (`[AI]`).

Tested on Hometown in the rain, seven opponents over two and a half minutes:
without it opponents sat stuck for six seconds and more at nodes 527, 561 and
the pile-ups behind them; with it none was held more than about two seconds,
and the field ran on average about 4% further (the leaders some 6%).

The game drives its traffic and its cops through `sub_408600` too. The tuning
-- backing off, looking for forks, flashing, the pace -- is for the racers
alone, whose flag in `[car+0x200]` is 8 (`sub_428700` sets it from the
car's kind); before, a traffic car stuck behind something was made to reverse
like an opponent. The cars looked at around one (another car ahead, in
between) are all the race's (`[0x6fd510]`), traffic and cops too, not only the
first eight.

### The grid: traffic with a full grid, Hot Pursuit's opponents

A race's cars are counted, then made one after the other into 0x6c-byte
records from `0x6fd52c`, sixteen at most (the end of the array is
`0x6fdbec`; the cars' own array, 16 of 0x9ac bytes at `0x5e1120`): the
players (`[0x6fd518]`), the opponents (`[0x6fd520]`), the cops
(`[0x6fd524]`) and the traffic (`[0x6fd528]`), the total in `[0x6fd510]`.
The opponent screen's Number is `[0x6fd4d0]` (0 none, 1 one, 2 a full grid)
and its Traffic `[0x6fd4e8]` (0 on, 1 off).

**Traffic with a full grid.** In a single race `sub_473bf0` makes the
opponents eight less the players for a full grid, else one or none, and with
traffic on as much traffic as makes eight cars in all -- none for a full grid,
which is why the menu greys Traffic out for it (`onOpponentFrame`).
`NFS_GRID_TRAFFIC` (on) lets it be chosen, and `gridTraffic`
(`src/nfs3hp/native_ai.cpp`, called where the count is settled, 0x473e3e,
`tools/apply_race_grid.py`) gives a full grid a third of what one opponent
would have had: two cars for one player, ten in the race. The traffic is the
game's as it always is -- parked at the start, sent out along the road after
the first thirty seconds -- and the opponents drive round it as they do with
one opponent.

**Hot Pursuit's opponents.** `sub_474640` gives a racer one opponent, up to
six cops and some traffic, and the menu forces Number to a full grid and greys
it out (`fitRaceSettings`, `onOpponentFrame`). The cops chase every car that
is not one of theirs -- with the player as the cop there are six racers -- so
`NFS_PURSUIT_GRID` (on) lets Number be chosen, and `pursuitGrid` (called where
every branch's counts come together, 0x474b15) makes a full grid seven racers:
the player and six opponents, the cops as the game picked them, the traffic
what is left of the sixteen (three). Not eight: every car that is not a cop
gets a bank of the cops' radio calls about it (`sub_40a010`, banks 0xf to
0x16), and the eighth stops the game ("VERYBIGERROR: TOO MANY RACER BANKS").
One or none keeps the game's own Hot Pursuit, and the player as a cop is left
as the game has it. Neither change is made in a network race, whose machines
must set up alike.

### Decompiled menus

The menus are being decompiled too, a screen at a time — not for speed, but to
be read and changed in C++. The menu engine finds a screen's handlers by its
name in a table at `0x555c48` (57 screens, one per `MENUS/*.MNU`): one as the
screen opens, one as it closes, one on every pass of the menu loop. A screen's
handlers and the functions only it calls go to `src/nfs3hp/native_frontend_*.cpp`,
hooked at the top of their generated functions by
`tools/apply_native_frontend.py`, the generated code kept behind them.

The menus' state stays in guest memory, where the rest of the game reads it,
and what the engine does for a screen — find an item by its codelink, a line of
the language's text, a dialog box — is a call into the game's own function at
its address (`src/nfs3hp/native_frontend.h`, which also lays out a screen and
an item: the fields the `.MNU` keywords fill are in the parser's table at
`0x557c10`). A decompiled function leaves memory and registers as the generated
one does, and calls the game in a frame of the same size, so even the stack
addresses the game's functions see are the same; each was compared with the
generated one byte for byte over all of guest memory, in every branch, before
it went in.

Done so far: the main menu in its three forms (`native_frontend_main.cpp`) —
`MAIN.MNU` for one player, `MAINSPLT.MNU` for split screen, `MAINMULT.MNU` for
a network race: the items fitted to the kind of race, the players' name
dialogs, the attract-mode movie, the network screen's race list and its
question before leaving. It is the first screen native whole: its 17
handlers are bound to their addresses with no generated code behind them
(`MENU_NATIVES` in `disassemble_nfs3hp.py`), and `tools/frontend_checks`
compares them with the game's code made afresh from `nfs3.exe`, keeping the
port's change among them — mainsplt's rows stand higher while it is up. The
menus' text it asks for — a line of the language's by its number, the names
of display modes, cars and controllers past them, and the check of a
player's name — is native whole too (`native_frontend_text.cpp`,
`FRONTEND_NATIVES`), compared by `tools/game_checks`. So is all the rest of
what the main menu calls of the menus' own: of the menu loop, a screen's
items shown and the shown screen's name (`native_frontend_loop.cpp`); of the
dialogs, a question, a message and a line to type in as it opens them, their
boxes and buttons laid out and the pointer put on the button highlighted
(`native_frontend_dialogs.cpp`) — these in `MENU_NATIVES`, compared by
`tools/frontend_checks` — and under them, in `FRONTEND_NATIVES`, compared by
`tools/game_checks`: the checks of a line typed in, the font a text is
measured and drawn in, a character at a time as a quad of its texture
(`native_frontend_text.cpp`), the pointer moved and the screens' art loaded
(`native_frontend_screen.cpp`). What is left under the main menu is the
game's: its sound, its art and files, its drawing, the attract-mode movie
and the network.

Then the options screens (`native_frontend_options.cpp`): `OPTIONS.MNU` and
the screens it leads to, most of them a race's pause screens too — the sound
(its five volumes, which play as their slider is held), the audio feed's
keys for the time, the position and the speed in a race (`audfeed`), the
function keys,
the controllers (their reset to the defaults, the dead zones of each axis, force
feedback) and the graphics. A screen keeps its settings as it opens and its
Cancel puts them back. Their 54 functions — the screens' handlers, the
handlers they give their items (a key, drawing, a slider's moves) and the few
only they call — were compared with the generated ones by
`tools/frontend_checks/run.sh`, the same way as the loop below, and keep the
port's one change among them: the effects' intensity slider is never greyed
out (`tools/apply_menu_unlock.py`). The controls' screens among them — the
function keys, the fixed ones, the controllers, their dead zones and force
feedback, 28 functions — are native whole now (`MENU_NATIVES`).

Then the screens a race is chosen on. The cars' (`native_frontend_cars.cpp`):
each player's car (`CAR1.MNU`, `CAR2.MNU` -- its colour, its showcase, the
way to its tuning), the opponents (`OPPONENT.MNU` -- how many, which car,
traffic) and a car's tuning (`CEDBASIC.MNU` -- its seven sliders, their
defaults and the advanced tuning they may differ from, from a player's car or
a race's pause screens). The track's (`native_frontend_tracks.cpp`): the
track and its laps, direction, mirror image, night and weather
(`LOCATION.MNU`, greyed in a series) and its picture, its map with its places
named and its spoken description (`TRKINFO.MNU`). With them, the developer's
recorder the car and track screens keep, which saves the turning car or the
track frame by frame as `.tga` files. Their 55 functions were compared by the
same `tools/frontend_checks/run.sh`, and keep the port's changes among them:
Download Car stays hidden (`tools/apply_hide_download.py`), the phone's
keyboard comes up while the game waits for a typed name
(`tools/apply_text_entry.py`), and the colour on `CAR1`/`CAR2` stands on the
Color row (`placeColourOnItsRow`). The rows' buttons and Transmission's list
are placed by their `ypos` from the row table at `0x559c94`, but the colour's
item keeps the fixed y of 220 its `.MNU` gives it — the Color row only in the
table with the rows 23 pixels higher. With the original table it sat between
Transmission and Color, over the bottom of Transmission's list: its name
printed under "Automatic", a click on it opened the transmission list, and Down
went from Color to the colour, whose box put the pointer on that list, and back
up — the screen could not be walked past Color.

The engine goes the same way, a layer at a time. First
(`native_frontend_menus.cpp`), what reads `FeData\Menus\*.mnu` into the table
of screens (`0x6040d0`) and the table of items (`0x604ee0`): opening a screen
reads its file and, by its buttons' `nextmenu`, every screen reachable from
it; a section (`[button]`) starts an item from its widget's template, a key
fills the item's field the parser's table names, by its kind (a number, a
pooled string, a line of the language's text, a float); names are matched
case and all. The original's quirks are kept: a number with any character
other than a digit in it, a minus included, reads as 0, and a line that is
neither a section nor a key with a value stops the game. With the loader came
the item lookup the screens use (an item by a name in its codelink) and the
screen's handlers found by its name.

Then the menu loop (`native_frontend_loop.cpp`), from the first screen until
the player leaves the menus. Each screen is opened (the art, the items' states
from their widgets', its onEnter) and passed over and over — its onFrame, the
items drawn layer by layer and given their own pass, the help box, the item
under the pointer selected, a key handed to the selected item or, when that
does not take it, Escape pressing the screen's Escape button and the arrows
moving the selection — until something leaves it: back through the 20
screens the history keeps, on to a button's `nextmenu` (asking for the
players' names first when they have none), into the race, or out (asked
first). The race settings are kept consistent on every pass.

Then the dialogs over the screens (`native_frontend_dialogs.cpp`): a question
and its buttons, a message that goes away by itself after its time, a line to
type a name in, a list of entries to tick. One is up at a time; the menu loop
hands it each pass and each key, and an answer — Enter, Escape, the message's
time run out — goes to the dialog's callback, which can keep it up. Its box is
laid out as it opens, in floats, in the x87's own arithmetic
(`fe::X87`): in a race's pause screens that rounds to a float's precision and
the box lands where the game puts it, to the pixel. The NFS emblem on the box
keeps the wide-screen fix (`tools/apply_widescreen.py`). The widgets come next.

The loop was compared in a world of stand-ins: the generated functions that
draw, wait for the next frame, read the keys, play music or talk to the
network replaced by ones that record every call and answer from a script, the
screens' and the items' handlers too. Besides memory and registers, the
decompiled loop had to make the same calls, with the same arguments, in the
same order. The dialogs were compared the same way, besides the x87's status
and control words, in each of its precisions and rounding modes; and as a
whole, the decompiled dialogs calling one another against the generated ones.

`NFS_FE_NATIVE=0` goes back to the generated code for every decompiled
function that still has it behind it, `NFS_FE_NATIVE_OFF=sub_437e00,...` for
the ones named; the log says which ran which way (`[FE]`). The screens that
are native whole have none to go back to.

### Binding letters to controls, and the brake on X

The Controllers screen never took a letter: choosing Brake and pressing X left
it waiting. The game reads the menus' keys from its own queue, and its
`WM_KEYDOWN` handler (`sub_4cd020`, through `sub_4ccaa0`) queues only the
arrows, the function keys and the keypad -- letters, digits and the rest it
takes as `WM_CHAR`, which Windows' `TranslateMessage` makes and the port only
made for control characters. A printable key now brings its `WM_CHAR` too
(`src/lib/window.cpp`), except while typed text is on (a name being entered,
where SDL's own text events bring it), so the capture (`sub_43cda0`: the
character, and the scan code `sub_451960` left at `[0x66465c]`) takes letters
as it did on Windows. And the keyboard brakes on X by default: the game's own
default set (data in the code at `0x490430`, the brake its fourth dword) is
patched from the down arrow (`0x5004`) to X (`0x58002d04`) at start; "Reset to
Defaults" and a new settings file take it from there, a settings file already
written keeps its own. `NFS_CONTROLS_TRACE=1` logs player one's race controls,
the actions they made and the keys held, twice a second.

### Slipstreaming

A car close behind another meets less air (`slipstream`, `native_surface.cpp`).
Each tick, for every car of the race over 80 km/h, the nearest car ahead of
it -- 3 to 37.5 m along its way, within 1.6 m to either side, heading the same
way (forward axes within 25 degrees) and itself over 80 km/h -- gives it a push
forward: 0.5 m/s2 at 200 km/h when right behind, growing as the square of the
speed, less the further back and the further aside -- about 5% more top speed
tucked in. Eased in and out over a quarter of a second. Every car, the
opponents, cops and traffic too; the AI is not told to look for one. The
speed is the velocity's (`+0xa4`): `+0xb8` is kept up only for the player's
car. `NFS_SLIPSTREAM` (1; up to 3 for more, `0`/`off` for none).

Round the player's car, while it slipstreams, air lines like a cartoon's
(`noteTrail`, `native_particles.cpp`; `NFS_SLIPSTREAM_TRAILS`, on): nine thin
white strands, three a side just outside its body and three behind it, wider.
Each has its own place (`kSlots`: metres right, up and forward of the car's
centre, and a width), so they do not overlap; where it was each frame is
joined up as streaks (`EffectParticle::Streak`, drawn added like the sparks)
that follow the way the car went, narrow at the newest end and up to 4-5
times wider at the oldest, fading within 0.36 s. Each strand lives
0.6 to 1.3 s, fades in over its first sixth and out over its last quarter;
once its last point has gone a new one starts at its place, a little off it
(up to 18 cm aside, 10 cm up or down, 35 cm along) and 25% wider or narrower.
At most three quarters opaque, times the slipstream. `NFS_SLIP_TEST=1` puts the player's car
in a slipstream all the time from 10 km/h, and `NFS_SLIP_TRACE=1` logs it.

### Trees and lamp posts knocked down

How the game knocks a sign down: a car touching one of the track's objects
(`sub_430da0`) asks `sub_41d350(car, index)` what to do. The index is the
object's REFXOBJ record in its block (`[[car+0xc]+0x5a4]`, 20 bytes each: its
global number at `+0xe`, its kind at `+0x13`), and each global number has
three bytes at `0x5dd230`: `0xff` standing, `0xfe` lying (the way it fell, its
yaw), anything else the slot of the animation it is flying through (24 bytes
from `0x56f234`: the animation, its start on the race clock, flags). Kind 2
is a sign: standing and hit at 11 m/s (|vx| + |vz|; a cop at any speed) it is
given a keyframed fall -- the slow one below 25 m/s, the fast one above -- and
the answer is -1, nothing to bounce off. Kind 1, everything else solid, trees
and lamp posts among it, answers 1: the car bounces off. A sign stands again
only at the next race (`sub_41de40`).

Now (`native_objects.cpp`, called round that call at `0x430f02` by
`tools/apply_object_knock.py`; `NFS_KNOCK_TREES`, on) a kind-1 object whose
model -- its XOBJ, type 4, crossno = index, in one of its block's lists at
`+0x564` -- is a lamp post (4 m or taller, within 0.6 m of its foot) or a tree
(2.5 to 25 m, within 10 m, at most 8 vertices: one billboard or two crossed --
Redrock's and Hometown's trees, Atlantica's palms (17-20 m), Country Woods'
pines (18 m, 8-9 m across); nearly every kind-1 object is one) is a sign for that one
question, so it is knocked down at the same speeds -- and, once down, driven
over as a fallen sign is (the answer 0: nothing to bounce off). Then it always takes the
slow fall, played at half the clock for a tree and 0.65 for a lamp post (the
animation's start pushed on each frame), so it spins and flies more lazily
than a sign; the car keeps v * 1400 / (1400 + mass) of its speed on the
ground (an inelastic hit; a tree 150 kg times `NFS_TREE_MASS`, a lamp post 70
kg times `NFS_LAMP_MASS`: about 90% and 95%); and after `NFS_KNOCK_RESPAWN`
seconds (10) it stands again, once no car is within 5 m. The sizes, masses
and timings are named constants at the top of the file. `NFS_KNOCK_TRACE=1`
logs every touch; `NFS_KNOCK_TEST=1` (or `lamp`, `sign`) puts the player's
car 12 m short of one, 4 s in, at 30 m/s.

### Tyre marks

The game lays a tyre mark as quads (`sub_4c6cb0`, from `sub_4db750` splitting
a wheel's step) into a ring of blocks at `[0x560110]` -- 0x340 bytes each,
`[0x7d5758]` in use, `[0x7d5754]` being filled; sixteen pieces a block, +0xc
how many, their four corners from +0x10, which of `skd0`-`skd3` from +0x310,
the alpha at each piece's start and end from +0x320 -- and draws them in
`sub_4c6f00`. Nothing fades: a piece lasts until its block is taken again, and
there were 64 blocks -- a few seconds of a whole field sliding, since one ring
is shared by every car and is written over first-in-first-out, so the
opponents' and the cops' churn filled it at once. `native_skids.cpp`
(`tools/apply_tyre_marks.py`) makes the ring 8192 blocks (the allocation at
0x4c70f5, the bounds at 0x4c6dfc and 0x4c6e16) -- about 6.5 MB of the guest's
memory, freed with the race, two minutes of four cars sliding at racing speed,
so what ends a mark is its life and no longer the ring being taken -- and, at
`sub_4c6cb0`'s end (`skidLaid`), reshades the piece just
laid by the nearest car's speed: 0.9 to 1.15 of its width about its middle,
1.6 to 3.4 times its alpha (the game's was 28 of 255), full at 40 m/s; each
piece a little lighter at random, one in twelve all but a gap, starting at the
darkness the same wheel's last piece ended at. `skidFrame` fades a piece out
over its last six seconds of `NFS_TYRE_MARKS_LIFE` (60). `NFS_TYRE_MARKS`
(on; `0`/`off` the game's ring and shading). `NFS_TYRE_MARKS_TRACE=1` logs
each piece.

The game lays marks only for the player's slide. `skidPedals` (from
`collisionEffects`, once for every car each frame) lays the port's own through
`sub_4c6cb0` too (`guest::call`; the two pairs in scratch below the stack, 48
of 255 before `skidLaid`'s shading), on any ground the table below gives a mark
to, 30 cm a piece. For the
first player's car: under the rear wheels while they spin -- full throttle
pulling away in first below 8 m/s, or revved past half the redline below
12 m/s -- under all four once the brake is past 0.5 above 3 m/s, and under the
rear two with the handbrake on, both from a sixteenth of a second of it (a
second of the brake past 0.55 until 2026-11-17: they were hardly ever laid).
And for **every car**, the opponents' and the cops' too: its velocity across it
over 1.2 m/s -- sliding -- marks the road under each wheel on any ground with a
mark of its own, since the game left the AI's slides with no mark at all. The handbrake (bit 0x04 of
`0x79c33b`) makes a piece 1.7 times darker, and a wet road (`0x6fd4cc`) every
piece 1.6 times: the wet road darkens the road, and its reflection is not
added over what lies on the road half seen (alpha under 0.98), so the marks
show in the rain. The game's first block of a race is filled twice over, so a
race's first sixteen pieces are not kept.

**A mark in the ground's own colour** (2026-11-19). `skidLaid` reads what is
under the piece (`groundAt`, `native_particles.cpp`), and `kSurfaceMarks`
gives each kind of ground (0 to 5) whether it marks and the mark's tint, alpha,
width and life against the asphalt's: pavement black and as it was; grass
`#223d18` at 0.3 of the alpha, 0.8 of the width and a third of the life; sand,
dirt and gravel `#6a3d1c` at 0.5, half the life; snow `#b9c6cf` at 0.2 for a
tenth of the life; mud `#2e2114` at 0.7; other ground `#4a3a2a` at 0.4. The
colour must be a colour: the four `skd` pictures are black, and black blended
over the ground only darkens it. `sub_4c6f00` builds each piece's polygon
colour as `alpha << 24 | 0xffffff`, a per-piece RGB that is always white, so
the port patches that one build (0x4c7068 and 0x4c708a) to OR in `markTint` --
the palette's RGB for the piece's ground -- and the renderer leaves the
picture's black RGB out of the colour combine for those four pictures, so the
tint is what shows while the alpha still shapes the mark. The surface rides in
the piece's +0x310 byte, whose low two bits are which picture and whose six
above are the ground's kind: `skidLaid` writes it and the texture lookup
(0x4c704a) masks the low two before indexing the four-picture table.

Which four pictures they are is recognised by their alpha, not by pointer or
name: the game uploads every texture through one shared staging buffer, so
`sub_4c6f00`'s record pointers and `GrTexInfo.data` all describe the same
buffer. A 32x32 picture is hashed (FNV-1a over its alpha, `skidPicture` in
`gliderenderer.h`) and matched against the four `skd` shapes at upload
(`ThrashRenderer::uploadTexture`, `GlideRenderer::setTextureData`), and those
addresses alone have the RGB combine left out (the direct renderer's
`Texture::skid`, the atlas's `m_skidAddresses`). `NFS_SURFACE_TEST` now also
makes `groundUnder` return the ground it names, so every surface's mark, dust
and frost can be tried on any track. `NFS_TYRE_MARKS_TRACE=1` logs each
piece's ground and tint with the rest.

### Steering in the air

`airSteer` (`native_surface.cpp`): while the player's car has been off its
wheels for four ticks or more (`+0x164`), its spin about its own up axis
(`+0xcc`; the world's spin at `+0xe4` and the car's own second component at
`+0x4fc`, as `airRoll` keeps them) eases towards the steering byte
(`0x79c338`, -127 to 127) times 0.9 rad/s, with a third of a second to get
there -- right is positive, as on the ground. `NFS_AIR_STEER` (0.2 -- 1 was far
too much; up to 3, `0`/`off` none).

`airPitch`, the same way: the pedals tip the car about its own right axis
(`+0xc0`; the world's spin at `+0xe4`, the car's own first component at
`+0x4f8`) -- the brake (`0x79c33a`, the down arrow) the nose up and the tail
down, the throttle (`0x79c339`, up) the nose down -- easing towards their
difference times 0.9 rad/s, with the same third of a second. `NFS_AIR_PITCH`
(0.2; up to 3, `0`/`off` none).

### Crashes you feel

How hard a car was hit is the game's own number: `sub_42ac40`, each car's
collision effects, finds it at `[car+0x168]` (m/s) and what it hit at
`[car+0x170]` (0x4xxxx a wall, its material in the low byte; 0x6xxxx a track
object; the rest another car). `collisionEffects` hands every car's to
`crashHit` (`native_crash.cpp`).

**The shake.** For the first player's car a hit over 3 m/s knocks the camera
aside and up -- swinging back once or twice and dying in a few tenths of a
second -- with a fast, small vibration under it, both as strong as the hit
(full at 20 m/s): a wall's whole, another car's 0.7 of it, a sign's or a
tree's 0.45. Scraping along a wall keeps a light buzz going by the speed; a
landing after 8 ticks in the air, falling at 4 m/s or more, jolts it down.
`cameraToView` moves the camera by it in any of the player's own views
(chase, bumper, cockpit; not the replay's television cameras), after the
throttle's pull. By the race's clock, so a pause holds it still.
`NFS_CRASH_SHAKE` (2; up to 3; `0`/`off`).

**The sound.** The game's sounds are started by `sub_4182b0`: the bank's
handle in eax, the patch in edx, the volume (0-127, times the effects
volume) in ecx, the direction in ebx and one more byte as its argument; it
fills SND's play block (`sub_4e9b20`'s defaults: +5 the note, 60; +8 the
volume) and plays it. A crash is bank 2's patch 46 and a wall scrape its
patch 41, and the game starts its crash at a volume of 0, the 3D sound's to
set -- quiet for all but the biggest hits. So each hit of the player's car
over 3 m/s also plays that crash itself, in front, as loud as the hit (35 to
127, by its share as above, at most one every 8 ticks); and every crash, the
game's and the port's, is pitched by the latest hit -- up to 4 semitones lower
for a big one, 3 higher for a tap -- and a scrape up to 5 higher the faster it
scrapes (`crashNote`, in `sub_4182b0` at 0x4182dd, `tools/apply_crash_feel.py`).
`NFS_CRASH_SOUND` (on; `0`/`off` leaves the game's). The port's crash was
heard in a recording of the mix (`SDL_AUDIO_DRIVER=disk`); that the note is
the pitch is SND's default of 60 and not yet heard. `NFS_CRASH_TRACE=1` logs
it all. A pad already rumbles on every hit with the game's own jolts.

### Cars roll less in the air

Off a ramp the game keeps whatever spin a car took off with until it lands:
the word at `+0x164` counts its ticks in the air, and the spin (`+0xe4`, in
the world's axes, and `+0x4f8` in the car's own) is left alone. A car that
left a ramp a little crooked rolled on through the jump and often landed on
its roof. While a car is in the air its roll -- the spin about its forward
axis (`+0xd8`), and the third of `+0x4f8` -- now dies away (`airRoll`,
`native_surface.cpp`): three quarters of it in a quarter of a second. The
jump's height, the pitch and the turning stay the game's; every car in the
race. Tested with a throw (`NFS_AIR_TEST=1`: 5 s on the clock the player's
car goes up 3 m, rolling): the game's own rolled it past its roof and round;
damped it tips about 40 degrees and lands on its wheels.
`NFS_AIR_ROLL_DAMP` (1; up to 3 for more, `0`/`off` for the game's).

### Bodies on their springs

Each car's body moves a little as it is drawn (`NFS_BODY_MOTION`,
`native_body.cpp`) -- discreet and indicative only: it leans a little out of a
corner, tilts a little back going uphill and forward going downhill, and
settles a few centimetres onto its springs when it lands. Nothing from the
throttle or the brakes, and nothing the game works out sees it: the car's
record is never written.

A car's model is drawn part by part by `sub_49de20` (from `sub_49d5c0`'s loop
over the parts, and from the lights' stand-ins), handed the car's matrix
(`[esp+4]`, its axes `+0xc0` right, `+0xcc` up, `+0xd8` forward) and position
(`[esp+8]`). `bodyMotionPart`, at that function's top
(`tools/apply_body_motion.py`), finds the race car whose axes the matrix is
and draws the part through the original with a leaned matrix and a moved
position put in for that call alone, then puts back what was there. (The first
try wrote the lean into the cars' records for the frame; the game's physics
runs in between and read it, and the cars drove on two wheels.)

The lean is worked out once a frame as its drawing begins (`bodyMotionFrame`,
the main view's sky), three springs with dampers in 1/120 s steps: roll,
0.018 rad a g of sideways acceleration (the velocity's change along the car's
right, eased over a quarter of a second), at most 1.5 degrees, to the outside
of the corner; pitch, 0.06 of the road's slope, at most 1 degree, nose up
going uphill; sink, on landing 6 mm for each m/s it came down at, at most
4 cm. The angles' springs barely overshoot (1.4 Hz, damping 0.7). In the air
it settles. The body turns about the ground under its centre (the lowest wheel
contact). All of it times `NFS_BODY_MOTION` (1; up to 3; `0`/`off` none); the
tunables are `body::` in `native_body.cpp`.

### Cars keep their speed in the air

The game drags a car down off its wheels as well: a jump costs speed the ramp
never took, and a long one lands well short of what it left at. Each tick, for
every car whose word at `+0x164` says it is in the air, the port holds the speed
it took off with (`airSpeed`, `native_surface.cpp`): its sideways and forward
velocity scaled back up to that speed, the fall left to gravity. A sudden loss
is left alone and becomes the speed held from then on -- a wall in the air is a
hit, and a hit is meant -- and a car back on its wheels holds nothing.
`NFS_AIR_SPEED` (`0`/`off` for the game's).

### What the effects cost, and keeping frames even

Measured on the weakest GPU at hand -- Intel UHD (Comet Lake GT2), Mesa, 1080p,
a race in the rain -- with `NFS_PERF_TRACE=2`, which leaves each costly effect
out in turn for two seconds and logs the frame's GPU time without it
(2026-11-18). Roughly, each effect's share of a frame:

| Effect | ms | Switch |
|---|---|---|
| Bicubic (Catmull-Rom) magnification of the game's textures | 3 to 4 (5 to 6 before the 5-tap form) | `NFS_TEXTURE_SHARP` |
| The lamps' light trails | ~3 | `NFS_LIGHT_TRAILS` |
| Grain on the road, the ground and the world | 1.5 to 2 | `NFS_ROAD_DETAIL`, `NFS_OBJECT_DETAIL` |
| Rain on the lens | 1.5 to 2 | `NFS_LENS_RAIN` |
| Haze | 1 to 2 | `NFS_HAZE` |
| The wet road's reflection | ~1 | `NFS_WET_ROADS` |
| The port's particles, with the soft ones at half size | ~2 (5 to 18 full size, near the lens in the rain) | `NFS_PARTICLES_HALF` |

What was done about the slow frames:

- **The particle pool** moved a third of a megabyte for each particle added to
  a full one (see A full pool, above); now one is replaced where it stands.
- **The soft particles at half size** (above): the spray's mist and the smoke,
  a quarter of the pixels.
- **Bicubic in 5 taps.** The Catmull-Rom magnification (`sharpTexture`) took
  9 bilinear taps a pixel, the 16 texels' weights folded into the filter's
  own; it takes 5 now -- the middle and the four sides, the corners' weights,
  the smallest, left out -- for a picture the eye cannot tell apart; and
  the GPU's own filtered read (the dearest, at 16x anisotropic) is made only
  where it is blended in, not under every magnified pixel: about 1.5 ms less.
- **Adaptive vsync.** What looked like 40 to 50 ms spikes were frames a little
  over 33 ms: with plain vsync a frame that misses a refresh waits for the
  next, and a 35 ms frame is shown at 50. The swap interval is -1 now
  (`Renderer`, `SDL_GL_SetSwapInterval(-1)`): a late frame goes out at once,
  torn for that frame, rather than held; where the driver has no adaptive
  vsync it falls back to 1. `NFS_VSYNC=strict` keeps plain vsync.
- **What the game uploads** is timed too: `NFS_PERF_TRACE=1` logs any frame
  over 35 ms on the game thread with the textures uploaded in it, their size
  and the time they took (`uploadTexture`, split out of `setTextureData` for
  it), to tell a GPU spike from a loading one.

### A race loads in a moment

Most of a race's loading time was waiting: the game flips its loading picture
again and again as it reads the track and the cars, and each flip waited for
its turn at the frame rate (`Renderer::paceFrame`) and for the screen's
refresh. Measured here on Atlantica: 13.5 s, of which the game's own work was
about a second. Now, from the loading picture to the race
(`loadingScreenFit`, `glide2x::unpacedFrames`), the flips are not paced and
at most one frame in 50 ms is drawn and shown (`ThrashRenderer::swap`), the
rest let go: the same race loads in about 0.3 s. One catch came with it: the
race's clock (the timer `sub_42dcb0` sets going; `[0x5efdf8]` a tick every
two of its calls) must have ticked once by the time the race begins, or
`sub_4c4e40` takes the race for dead and quits the game -- and with no flips
waiting, the timer's thread hardly ever got the guest. So the end of the load
waits for that first tick, the guest let go meanwhile (at most a second).
`NFS_FAST_LOAD` (on; `0`/`off` for the paced loading). The log says
`[LOAD] the race loaded in ... s`.

## Building

### Android

Requires Android SDK with **NDK 28.2.13676358**, build-tools 36.0.0, compileSdk
36, and a JDK (Android Studio's bundled JBR works). The app is arm64-v8a only,
minSdk 26, so it installs on Android 8.0 and later. The floor is `java.nio.file`,
which the launcher reads and writes the game's files with; every platform call
above 26 is behind a version check with a fallback.

SDL3 is built from source on Android and is not vendored here — clone it first:

```bash
git clone --depth 1 --branch release-3.4.2 https://github.com/libsdl-org/SDL third_party/SDL
```

Then build:

```bash
cd android
./gradlew assembleDebug
```

The APK lands in `android/app/build/outputs/apk/debug/`. `assembleRelease`
builds unsigned, so align and sign it yourself with `zipalign` and `apksigner`.

### Desktop (Linux)

CMake ≥ 3.15, a C++17 compiler, and SDL3 3.4 development files. On Debian and
Ubuntu that is `libsdl3-dev` (universe); everything else — OpenGL — is normally
already installed:

```bash
sudo apt install build-essential cmake ninja-build libsdl3-dev libgl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The binary is `build/nfs3hp`. It wants a display: there is no headless mode.
If SDL3 is installed somewhere CMake does not search, point it there with
`-DCMAKE_PREFIX_PATH=/path/to/sdl3`.

A game folder is `fedata/`, `gamedata/`, `install.win` and the four executables
from `nfs3hp/` (`nfs3.exe`, `eacsnd.dll`, `softtria.dll`, `voodoo2a.dll`). The
executables and `install.win` are in the repository — `install.win` is the copy
under `android/app/src/main/assets/gamefiles/` — so only the game's own data has
to come from your disc:

```bash
mkdir -p build/game
cp nfs3hp/* build/game/
cp android/app/src/main/assets/gamefiles/install.win build/game/
cp -r /path/to/disc/FEDATA /path/to/disc/GAMEDATA build/game/   # case-insensitive
python3 tools/extract_viv.py build/game                          # unpack the cars
./build/nfs3hp build/game
```

The disc keeps some of its data packed, and the game only asks for it unpacked:
every car is a `BIGF` archive, `gamedata/carmodel/<car>/car.viv`, holding that
car's model (`car.fce`), texture, text and sounds, and `fedata/art` packs its
showcase pictures the same way. The retail installer unpacks them; a folder
copied off the disc has only the `.viv`. Without that step the game has no car
models — `tools/extract_viv.py` does what the installer did, in place, leaving
the archives alone and never overwriting a file that is already there (`--force`
overwrites).

`unable to open file .../car.fec` lines are not a sign of that: `car.fec` is the
20-byte record the game keeps of one car's own colour and tuning, written beside
`car.viv` when a car is chosen, so a data set has one only for the cars it has
been played with. A missing one is expected — the row is created the first time
that car is picked.

With two arguments the second is the CD path, for data split between an install
directory and the disc.

A game starts on the whole screen: borderless, no title bar or frame, and the
picture keeps its own shape with the rest of the screen padded in black rather
than stretched -- the renderer picks the largest rectangle of the game's aspect
and blits into it (`present` in `src/lib/renderer.cpp`), so a 16:10 or 21:9
monitor gets bars. `NFS_FULLSCREEN=0` opens a window instead, at 1280x720,
centred and resizable (a display too small for that gets a smaller one of the
same shape, `NFS_WINDOW=1600x900` picks another). **Alt+Enter** toggles between
the two.

Losing focus — another window in front — stops the game's clock, and a race
being driven comes back paused, as a phone's background does: the game's own
pause menu, opened by its own key. A network race is not paused; no one player
may stop it.

**Escape, Return or Space** ends a movie. The intro's films come before the
first menu and a desktop had no way past them — the skip was the Android UI
thread's to ask for. `NFS_SKIP_MOVIES=1` ends every movie as it starts.

If the folder is not a game folder, the log says which part is missing before
the game starts; the game itself only enumerates an empty directory and exits.

A settings file written by another build of the port can bind player one's
steering and pedals to the first pad's **axes**. On a desktop with no pad
attached that is a stick which is always centred and a pedal that is never
pressed, so the car does not move however many keys are pressed. When no pad is
attached and those entries name an axis, the port puts the arrows back in both
tables player one uses, before the front end reads them, and says so in the log
(`[SETTINGS] no pad attached: the arrows steer and work the pedals`). A file
that already binds keys, and a machine with a pad plugged in, are left alone.

A new game — no `fedata/config/config.dat` of its own, which is what data
straight off the disc looks like — gets the port's screen settings instead of
the ones the game picks for itself: Screen Size **Dynamic Fullscreen**, which
is the window's own size, so a race fills it at whatever size it was opened or
resized to, and View Distance **Full**. The HUD is left as the game arranged
it: the phone's corner layout is laid out for a touch screen, and a monitor
shows the game's own 640x480 arrangement, scaled. The game saves the file at
the first front end, and from then on it is the player's — delete it to get
these back.

Keys are the game's own 1998 defaults: the arrows steer and work the pedals,
Escape is back, Enter confirms; the mouse is the game's pointer, races included.
While a box asks for a name the window takes typed text (`SDL_StartTextInput`,
in `textEntryTick`): SDL 3 only sends it while that is on, so until it was,
Backspace deleted and the letters did nothing.
The game's cursor is put where the system pointer is on the picture, as a
phone's mouse is (`desktopMouseWatch`, `src/nfs3hp/nfs3hp_main.cpp`), not moved
by how far the mouse went: that distance counts in pixels of the window, up to
three times the 640 a menu spans, so the two parted at once and the system
pointer stopped at the screen's edge with the game's cursor short of it. The
wheel still reaches the game as DirectInput's.
The window's own pointer is hidden while the game's mouse is open — the game
draws the pointer itself, and SDL's arrow stayed on screen beside it, two
pointers a few pixels apart (`ShowCursor`, `src/lib/winapi/user32.cpp`).
A pad is `NFS Gamepad 1` / `2` with X steering and Y as both pedals, its buttons
sending keys chosen with `NFS_GAMEPAD<n>_<BUTTON>` (see Environment variables).

### Adding a car

`tools/import_car.py` does all of what follows for a downloaded car — a zip, a
`.viv` or the folder it was unpacked to, however the add-on packed it:

    python3 tools/import_car.py build/game ~/Downloads/some_car.zip [more.zip ...]

Each car in it becomes a new folder; a folder name, an ID or a four-letter
code another car already has is changed for the new one (in its archive, every
language), its showcase art and slide go in under its new name and ID where
nothing is there yet, and files every car shares — the add-ons' own copy of
`compare.fsh` — are never written. `--dry-run` says what it would do. Fake
downloads (a `.viv` that is not an archive) and archives that are not cars (an
NFS4 traffic body) are named and skipped.

**Taking a car out, and finding one that is there twice.** The same tool lists
what is installed and takes cars out:

    python3 tools/import_car.py build/game --list
    python3 tools/import_car.py build/game --remove clk2 clk3 --backup /tmp
    python3 tools/import_car.py build/game --dedupe --backup /tmp --dry-run

`--list` prints every folder with its four-letter code, its ID and its make and
model, and marks the folders that are another's duplicate — two imports of the
same car, which the menus show as two identical entries.
`--remove` zips each folder whole into `--backup` (the system's temporary
folder by default) before deleting it, and takes its line out of
`gamedata/carorder.txt` with it, so nothing is lost and the order of the rest is
kept.

`--dedupe` does that for every duplicate at once: the folders whose texts name
the same make and model are one car — the importer gives each copy a code and an
ID of its own, and those are all that tell them apart — and **the copy installed
first is kept, the lowest car ID**, which is the game's own car where one of the
pair is one (`ff50` the F50, `merc` the CLK-GTR) and otherwise the earlier
download. It prints the pair before acting, and takes the rest out exactly as
`--remove` does, so `--backup` and `--dry-run` mean the same. It was used on
`cc5r`/`chev` (the Corvette C5R), `clio`/`rena` (the Clio V6), `clkg`/`merc`
(the CLK-GTR), `ff50`/`nfs3` (the F50), `suba`/`impw` (the Impreza WRC) and
`peug`/`p206` (the 206 WRC): 54 folders to 48 cars, none a duplicate.
The cars `--remove` was used on by hand: `clk2` and `clk3` (two more CLK-GTRs),
`chev` (the Camaro, `cama` kept), `d24z` (the Datsun, `dats` kept) and `lstm`
(the Lister Storm, `lst2` kept).

**Where a new car goes in the list.** The game numbers its cars by the order
it finds their folders, and that number is what the settings keep: the
player's car, the opponents'. Windows lists folders by name, so a new one slid
in among the others; Linux and Android list them in the directory's own order,
so it could land anywhere — and every car after it moved up a place, which
looked like the new car taking an old one's place. The port keeps the order in
`gamedata/carorder.txt` (`carFolderOrder`, `src/nfs3hp/nfs3hp_main.cpp`), one
folder a line: the cars named there keep their places, and a folder not named
yet is added after them, so nothing already chosen changes. The first run
writes it from the order already in use. Editing it reorders the list.

One car is always last: Knockout (`knoc`, ID 33), the stand-in the knockout
races use. The Player Car list is built in place order and stops at it
(`0x43a435`), so whatever came after it was loaded but could not be chosen —
on a Linux disk that was usually a few retail cars and any car just added.
The retail install had it last only because its installer wrote the folders
onto FAT in that order.

By hand, three things make a folder a car.

- **`car.viv`**, the car's archive, and it is the one file that matters: the
  model (`car.fce`), the texture (`fedata.fsh`), the text (`fedata.eng` and the
  other languages), the sounds and the dashboard are all read from inside it.
  The loose copies `tools/extract_viv.py` unpacks are for the game's later
  lookups, not for loading. A folder without a `car.viv` is not loaded, and the
  log says so before the game starts.
- **The ID in `fedata.eng`**, one byte at offset `0x18` of the archive's
  `fedata.eng` — the retail cars use 2 to 49, all distinct. The game compares
  this byte across every car as it starts and refuses to run on a repeat:

      Duplicate cars found in carmodel folder!

  A copied car repeats it. Change this byte, and the copy becomes a car of its
  own. `carp.txt`'s `Serial Number` is a different field and is not the one
  checked. The value is compared as a byte and must be distinct — 256 cars is
  the whole of the range — but it may be anywhere in `0` to `255`; the retail
  cars only happen to use 2 to 49.
- **A name.** The display name is in the same `fedata.eng`; two cars may share
  one, as the game's own (Bonus) cars do, but the car list is sorted by it.

Then the car is in the list, selectable and drivable — verified by adding a
copy of the Corvette under a new folder with its ID changed to 20 and its name
to `ZZZ TESTER`, and finding it in the Player Car screen. Showcase art
(`fedata/art/showcase/h<car>.qfs`) and the compare picture
(`fedata/art/compare.fsh`) are cosmetic: a car without them still appears, with
whatever the game draws in their place.

A folder that is not a car — a half-made one, or a stray copy — is skipped
rather than crashing the game: `tools/apply_car_archive_guard.py` stops the
lookup that would read through the null the missing archive left behind, and
the port's C runtime answers a null `FILE*` with an error instead of a
segfault (`src/nfs3hp/native_watcom.cpp`).

An archive that is there but incomplete is the same kind of thing. With an
entry taken out of `car.viv` the game hands the model builder a pointer that is
not a model — FM50 without its `fedata.fsh` arrives with a header a byte along
(0x45000 triangles instead of its 1104), the heap is asked for 37 MB, the heap
answers nothing, and the records are written through the null. `modelWalkable`
in `src/nfs3hp/native_loading.cpp` checks the header against the block it came
from and the part ranges against its own counts before anything is read, and a
model that fails is a car with no body instead of a crash:

    [FCE] a car's model is not an FCE at 04aa3a90: no body for it (is its car.viv complete?)

An archive with no `car.fce` in it at all is named before the game starts
(`checkCarArchives`, `src/nfs3hp/nfs3hp_main.cpp`), the way a folder with no
archive is:

    [DATA] 1 car archive without a model (car.fce), and so drawn with no body:
    [DATA]   gamedata/carmodel/<car>/car.viv

**Up to 256 cars, and it used to be fifty.** Three separate limits stood in
the way, all of them "fifty because the retail game has 23":

- **The table `sub_439a50` memoises cars into** — 50 pointers at `0x5f3c00`,
  with the car count in the dword straight after it, so it could not be made
  longer where it lies. It is now the port's: 512 entries in host memory
  (`src/nfs3hp/native_car_slots.cpp`, `tools/apply_car_slots.py`), which is
  what `sub_439a50` reads and writes instead of the table. The table is a
  cache: `sub_4399f0` empties it each time the scan rebuilds or re-sorts the
  list of loaded cars (`sub_439d90`, `sub_438eb0`), so that every place is
  looked up again. It still cleared the 200 bytes at `0x5f3c00`, so the port's
  table kept each place's record from the order before: an opponent picked
  from a class (found by walking the list, `sub_439990`) was raced as whatever
  car had stood at its place -- police cars among them. `sub_4399f0` now
  empties the port's table too (`clearCarSlotRecords`). Its bound was also
  still `cmp eax, 0x31` — the first attempt at this only changed the comment
  beside that compare, which is why raising the table did nothing on its own.
- **Six per-car arrays** the scan fills, at `0x554fa8`, `0x55500a`, `0x555138`
  and `0x5551a2` (16-bit) and `0x55506c` and `0x555214` (32-bit): 49 entries
  and a terminator each, packed against the next, so the fifty-first car wrote
  into the array after its own. They are now in a section of memory the port
  adds to the machine's map, `kCarArrayA`-`kCarArrayF` in
  `src/nfs3hp/car_arrays.h` (`tools/apply_car_arrays.py`), with room for every
  car the list can hold. Every access is `base + index * element` with the
  index already scaled, so only the base had to change.
- **The four lists the car menus sort** (`sub_43a580`), at `0x5552d8`,
  `0x5553a0`, `0x555468` and `0x555530`: fifty records each, 200 bytes apart,
  with a group's address worked out as `group * 200`. Moved and enlarged the
  same way (`kCarGroupTable`, `tools/apply_car_groups.py`), and the two places
  that multiply by 200 now multiply by `kCarGroupStride`. This was the one
  that crashed: past the fourth group ran the counters at `0x5555f8` and then
  live globals, so the sort compared a pointer that was never a car.
- **The five car lists' records**, twenty bytes each from `0x5564ac`: the
  choice the list drives, its name, and the three per-car tables it shows
  (`cars`, `car2`, `carShowcase`, `carCompare1`, `carCompare2`). Those three
  are pointers held in the game's *data*, so moving the arrays left every car
  menu reading the addresses the tables used to have: the Player Car row showed
  line 0 of the language's text — the game's own `BAD TEXT INDEX 0` — and the
  car lists scrolled through nothing. `patchCarListDescriptors`
  (`src/nfs3hp/nfs3hp_main.cpp`) repoints them at `kCarArrayA`, `B` and `E` as
  the game starts, since no code patch can reach a pointer in data.

A car's ID is one byte, so 256 distinct cars is the ceiling, and that has been
run: 256 folders load, the car screens list and drive them, and a race starts
and runs. Two things are worth knowing about how it got there.

The tables are sized **512**, twice the id range, because the game asks
`sub_439a50` with two different numbers: a car's place in the list, and — in
the traffic code — a traffic body's id, which runs to `0x1f3`. Sized for the
first only — 260 entries — a 256-car race crashed mid-race; 512 holds both.

**What is left**: the traffic bodies. Their ids are 50 to 66, their names are
in the game's own table at `0x55cfb0` ("0008", "0009", … the folders under
`carmodel/traffic`), and they are not loaded cars, so the game names them by
falling back to that table when `sub_439a50` has nothing at that index. Below
fifty cars there is never anything there; past it there is, so the race asks
for `carmodel/traffic/<a player car>/car.viv`, which does not exist. The open
fails, the log says so, and the race carries on without that body — it is not
a crash -- except where the game opens the body itself, sub_42c960's
`openhandlea`, which stops it with "OPEN FAILED ON
gamedata\carmodel\traffic\pmf1\car.viv" (Hot Pursuit with the Blitz R348,
52 cars). That one call (0x42ca40) now names an id from 50 to 0x1f3 straight
from `0x55cfb0` (`nfs3hp.7.cpp`, marked PORT). A player car past the fiftieth
place still goes down the traffic folder's path. The whole fix means letting `sub_439a50` answer by id — the record's own
id byte at `+0x31c` — instead of by place, so a place and an id stop meaning
the same number, and that is the next piece of work here.

### Porting a track from High Stakes

Need For Speed: High Stakes (NFS4) is this game's engine a version on, and its
`data/TRACKS` folders hold the same tracks. `tools/port_track.py` turns one
into a track folder of `gamedata/tracks`:

    python3 tools/port_track.py --list
    python3 tools/port_track.py HOMETOWN --slot 0 --out /tmp/try --dry-run
    python3 tools/port_track.py AQUATICA --slot 6 --force

and undone with, no copying of folders either way:

    python3 tools/port_track.py --status          # each slot against the snapshot
    python3 tools/port_track.py --reset Aquatica   # or --reset 6, trk006, or all
    python3 tools/port_track.py --reset --dry-run

The first write into `gamedata/tracks` takes `gamedata/tracks-default.tar.gz`,
the tracks as they shipped, and `--reset` puts every file of every slot back
from it byte for byte and takes away any track that is not the game's (slot 9
onward). `--snapshot` takes it by hand, `--force` replacing it.

Nine of its nineteen tracks are this game's own: the slot a track belongs in is
the art name at `0x559620` — home, redr, atla, rock, coun, lost, aqua, summ,
empi — so `trk000` is Hometown, `trk005` Lost Canyons and `trk006` Aquatica,
and each of the nine has exactly the same number of road vectors in the two
games (`simhts.dat` holds one four-byte entry each, which is how the tool pairs
them). `--slot` with `--force` puts High Stakes' version in that slot and the
menus find it as before. The other ten — Coastal, Snowy, Hills, Park, France,
Germany, UK, GT1, GT2, GT3 — go in a slot past the ninth, which the game never
asks for: its location menu lists nine, and its per-track art, place names and
place counts are the nine-entry tables at `0x559620`, `0x5596d4` and
`0x5597f4` (the three are `dword[track * 4]` lookups, and slots 9 to 13 are
empty in them — room, but nothing lists a track there yet).

What is the same format and is copied: the road's objects (`TR00.CAN` …
`TR05.CAN` → `tr0000.can` …, byte for byte), the heights (`HEIGHTS.SIM` →
`simhts.dat`), the road's textures (`TR0.QFS` → `tr000.qfs` and its three
siblings, one FSH container for this game's four), the sky (`SKY.QFS` →
`sky.fsh`). `TR.INI`, `TRN.INI`, `TRW.INI` and `TRNW.INI` are this game's four
`.hrz` files under other names — the same horizon, clouds, fog regions, weather
and ambient light, key by key — and are translated, in the order this game's
own `3tr00.hrz` writes them. Hometown's comes out with the same horizon values
as the file this game ships (its fog regions are High Stakes' own, and more of
them: nine to this game's two).

The file's first two words matter: every track this game ships opens `0x0135`
then `0xbeef`, every High Stakes one `0x0058` then `0xbeef`, and with High
Stakes' word kept the game loads nothing at all — the race comes up with no
blocks (`[VIEW] 1 blocks`), empty at the default fog, and drops back to the
menu. A ported file therefore carries `0x0135` whatever its High Stakes source
said (2026-11-18).

**The two worlds are a scale apart.** High Stakes' version of a shared track is
this game's divided by 1.3 — the same road vectors in the same order, the same
block centres, every coordinate and every ground height 1/1.3 of this game's
(High Stakes' `HEIGHTS.SIM` × 1.3 is this game's `simhts.dat` digit for digit;
its block centres × 1.3 are the retail ones to within the integer rounding of
High Stakes' own). Everything converted is scaled back up by that, without
which a ported track sits 23% small under this game's cars.

`TR.FRD` is the same data laid out differently and is converted. High Stakes
writes a list of road vectors, then every block's header, then every block's
data; this game interleaves each block's header with its data
(`native_loading.cpp`, `sub_419c20`). The header is the same fields in another
order plus a few (the eleven chunks' polygon counts, the block's min/max
corners); a polygon is 13 bytes there to 14 here; and a polygon carries its own
road vector instead of an index into its block's, so the tool collects a
block's vectors, deduplicated, and writes the indices. High Stakes' four
polygon chunks past the seven a block holds here — its extra scenery ones — and
its special objects (the ones a car drives into) have no counterpart and are
dropped. Each conversion is read back with this game's own layout and checked
for polygon vertices and road-vector indices in range, and for the whole file
fitting the arena the game reads a track into (`0x310000` bytes: the retail
tracks take 34 to 65% of it), before it is written.

**Three things had to be true of a ported file, each found by a crash
(2026-11-18).** The sky: High Stakes' `SKY.QFS` stops the whole track's setup
before the road is read — this game's horizon loader (`sub_478340`, on the way
to `sub_41d960`) sets the loading screens' flag `[0x55e88c]`, the road is
skipped, and the race starts on an empty track and faults on the way back to
the menu. So `sky.fsh` stays the game's own unless `--sky` asks otherwise, and
a port puts it back if an earlier one left High Stakes' there. The arena: it is
3 211 264 bytes and the loader never gives any of it back, so past its end an
allocation returns 0 and the read that follows copies into address 0 — the
track loads a stub and the race dies later. High Stakes carries about ten times
this game's road polygons (Park's *medium* set is 800 a block against Rocky
Pass's 107), so `fit_arena` sheds detail until the file fits 85% of it: the
levels this game's own tracks never use (1, 3, 5 and 7 to 10), then the high
road set (4), then the coarsest (0), then the lanes (6), then the scenery, and
it refuses a track that still does not fit. Park lands at 85%. The objects: the
loader reads each one as three words, twelve more, then the *length* of a block
of extra bytes and that block (an animated object's keyframes), then its
vertices, colours and polygons — so a keyframe block written inline makes one
object read as 196608 vertices. `loader_reads()` walks a ported file in exactly
that order and refuses a file the game would not read whole; it reads all nine
retail files to their last byte, which is what makes it trustworthy.

    python3 tools/port_track.py --self-test

runs both halves of that in memory — this game's nine tracks written back as
they came, every High Stakes track converted and read back — and names what
failed, writing nothing.

**What is missing is collision.** High Stakes' tracks ship no `.col`; this game
loads one (and a `.ccm`) beside the road. `--borrow N` takes both (and
`speedsf.bin`/`speedsr.bin`, whose High Stakes layout differs) from the
snapshot — the game's own files, not whatever the slot holds now — which for
the nine shared tracks is right by construction: at the 1.3
scale their road vectors agree to within the quantisation of the `.col`'s own
fixed-point coordinates (100% of them within 1%), so the retail collision fits
the ported road. The borrow is the default for those.

For the ten tracks this game never had it can be **generated**: `trNN.col` is
`COLL` version 11 — a texture table (8 bytes a texture id), a struct3D of
collidable meshes, one or two object tables, and then a road-vector table of
36-byte records, one per slice — 959 of them for Hometown, the same count and
order as High Stakes' own road vectors, at 1.3× their coordinates. So the road
half of it is a re-encoding of data the converter already reads:

- `refPt` is 16.16 fixed (High Stakes' floats × 1.3, × 65536),
- `normal`, `forward` and `right` are four signed bytes each, three of them the
  vector × 128, the fourth unused (this game's own files have junk in it),
- `leftWall`/`rightWall` are 16.16 too, but not High Stakes' walls × a factor:
  measured over the nine shared tracks this game's is High Stakes' plus about
  3.4 (± 0.7), so a generated track's walls want calibrating against the road's
  own width rather than a constant,
- the texture table names a texture id (the same numbers the road's polygons
  carry) and the surface behind it, so it needs the same High Stakes-to-this
  game's texture numbering the polygons do,
- the struct3D meshes are the collidable scenery, which High Stakes' objects
  could be built from — they are in its FRD, already read here.

`OBJECT_KINDS` differ between the two games too: Hometown's first block asks
for object kind 7 in High Stakes and 9 here, though the `.can` catalogues are
byte for byte the same file, so a ported track's roadside objects may come out
as the wrong ones.

**What has been seen.** Aquatica was driven in this game's engine by hand
(2026-11-18, at the 0x0135 header), so a shared track ported this way is a
track one can race on: its road, its heights, its textures, its horizon. A race
can also be driven without hands, from the port's own touch script — which is
how the three crashes above were found and cleared:

    SDL_VIDEODRIVER=offscreen NFS_SKIP_MOVIES=1 \
    NFS_TOUCH_SCRIPT="8000@key:Return;11000@key:Return;15000@350,448" \
    ./build/nfs3hp ./build/game

(the intro film skipped, One Player, then a tap on the setup screen's RACE
button — `NFS_TOUCH_SCRIPT` takes `<ms>@key:Name` and `<ms>@x,y` in the
640x480 screen's own coordinates, and a `[TRACK] track N: …` line per step of
the load says where it is).

Park gets further than any other High Stakes-only track: its own 118 blocks
load and the setup completes at 85% of the arena, and the race then faults in
the collision code, because the `.col` in its slot is Rocky Pass's. Until a
`.col` can be generated (below), only the nine shared tracks are drivable.

### Seeing and tuning a car's performance

`carp.txt` is the whole physics of a car: 80 labelled fields, plain text, and
the labels are what the game's parser looks for (they are in `nfs3.exe` at
`0x53e0xx`-`0x53f0xx`), so editing a value is the whole job — nothing is
compiled or re-derived.  It lives **inside the car's `car.viv`**, beside
`car.fce` and `fedata.fsh`: that is the copy the game parses, and the loose
`carp.txt` in the folder is only what `tools/extract_viv.py` unpacked (delete
it and the car still drives).  The tool therefore reads the archive and writes
both, and a file that grows past its old size has the archive rebuilt around
it.

`tools/car_perf.py` reads them, draws them, and writes them back
(`report` is `tools/car_report.py`'s, below):

    python3 tools/car_perf.py list                     # every car, side by side
    python3 tools/car_perf.py list --fields            # the fields you can set
    python3 tools/car_perf.py show hgts                # a window
    python3 tools/car_perf.py show hgts --out hgts.png # a picture instead
    python3 tools/car_perf.py compare hgts diab ffal
    python3 tools/car_perf.py set hgts mass=1500 "final=4.10"
    python3 tools/car_perf.py clone hgts mycar --id 60 --name "ZZZ 001" mass=1500
    python3 tools/car_perf.py report [diab f355 ...] [--out car_reports]

`show` puts four panels on one page: the headline figures as bars against the
rest of the field (power, power per tonne, top speed, 0-100, grip, braking,
turning circle), the engine's torque curve with the power worked out from it,
the gear chart (engine rpm against road speed, one line per gear, with the
redline and the drops at a shift), and the acceleration the game gives the car.
`set` and `clone` take `field=value` in the same words the labels use, or a
`--preset` JSON object, and `--dry-run` says what would change without writing;
`clone` copies the whole folder and gives the copy its own id and display name,
in `fedata.eng` and inside `car.viv`, so it is a car the game will load (the id
must be unused, and the name cannot be wider than the one it replaces — the
archive's offsets are fixed).

`report` (`tools/car_report.py`, which also runs on its own) writes a sheet
per car -- `car_reports/<folder>.png` -- of six panels: torque and power
against rpm with the redline; the gearbox, engine rpm against road speed in
each gear with the drop at each shift; the push at the wheels in each gear
against road speed (torque times the gear, the final drive and the gear's
efficiency, over the wheel's radius, worked out of the game's own rpm per m/s)
with the best gear drawn over all, so the speeds where each gear hands over to
the next show; the game's acceleration table by speed with 0-100, 0-160 and
0-200 km/h worked out of it; and the specs (mass, power to weight, ratios,
final drive, top speed, drive, brakes, grip, tyres). With several cars it
adds `compare.png` (top speed, 0-100, 0-200 and power to weight, every car
ranked) and `summary.csv`.

`edit` (`python3 tools/car_perf.py edit [car]`, or `tools/car_report.py
--edit`) opens a window: every car in a list on the left, a page at a time;
its charts in the middle; and on the right its specs as boxes -- mass, the
torque curve scaled, redline, idle, the forward gear ratios, the final drive,
top speed and cap, front drive, brakes and their bias, grip, front grip and
downforce. Change one and press Enter (or click elsewhere) and the charts and
the worked-out figures follow at once; a car with changes is marked `*` in the
list. Pointing at a box brings up a note on what that value does to the car,
over the charts beside it and semi-transparent, so the curves stay readable
under it; a value wider than its box (the gear ratios) is cut at the box and
scrolls with the arrow keys or the wheel. Save writes the car through
`car_perf.py` (car.viv and the loose carp.txt, the gearbox's rpm per m/s kept
in step with the ratios and the final drive, the automatic gearbox's the same
where they were the same); Cancel puts it back as it is on disk. It needs a
window for matplotlib (Tk or Qt).

**The torque curve is dragged, not typed.** Its 41 points (one every 256 rpm)
are marks on the torque chart: the pointer within a few pixels of one picks it
up, moving up and down sets that point -- never below zero -- and letting go
hands the curve on (`CurveDrag`). The "Torque x" box still scales the whole
curve at once, and a factor typed there is folded into the points when the next
drag starts, so the two never bite each other.

**The acceleration chart shows both curves**: the game's own table, and -- for a
car with unsaved edits on it -- dashed over it what the engine as tuned now
makes of the car: the same curve Save writes when it rebuilds the tables,
carried through the same road (the pending changes applied to a copy of the
car, the model's estimate for it, scaled to the car's own level), so the two
meet exactly after a rebuild. A car nobody has touched shows only the game's
table: the model's curve and EA's differ a little even there, and drawing that
as "this tune" made every car in the list look edited -- the dashes came up on
all of them, and stayed up after Cancel. The "Worked out"
column has the times out of each. Both move *while* a point is dragged: a chart
of its own costs tens of milliseconds to draw, so instead of drawing it again
the drag freezes the two charts as they are -- their curves hidden, their pixels
kept -- and every step puts those pixels back and draws the dragged curve and
the new estimate over them (`drag_freeze`, `drag_show`): under five milliseconds
a step, where drawing the charts again was over a hundred. Everything that
follows the curve -- power, the push in each gear, the figures -- is drawn again
once, when the drag ends.

**The game's own acceleration table** -- the curve the AI drives to, carp.txt's
eight AI ACC sections -- is rebuilt on Save when the box below Save says so
("Rebuild AI tables on Save: yes"; `derive_tables`, the same as `car_perf.py
derive`). Nothing else recomputes it, so a car whose engine, gearing or mass
was tuned otherwise keeps the acceleration its donor had. Its *level* stays the
car's own: the untuned car is worked out the same way and its figures are the
anchor, so only the tune's own improvement is kept. Aimless, the model would
have its way -- it has no clutch, tyre or traction loss in it, and on an
add-on's optimistic figures it can be 3.4 times out (`clkg`: 3.4 s of car
against a 1.4 s estimate), which would hand the AI a table it cannot drive to.

An ACC section is written as many numbers as the section holds, not 14 each: an
add-on's file can differ (`clkg`'s first section is a whole 98-sample curve),
and the table is as long as the car's own.

All of these find the cars on their own: `gamedata/carmodel` where they are run,
or else the repository's (the folder above `tools/`), so they run from
anywhere; and the list holds every car's folder under it -- the racers, and
the traffic and police cars further down (`traffic/0001`,
`traffic/pursuit/0005`; `car_perf.car_folders`).

### Marking a car's exhausts

`python3 tools/car_model.py exhausts [car]` (or `tools/car_exhausts.py`, or the
editor's "Mark exhausts..." button) opens a window: the cars in a list on the
left, the car's body seen from behind (its high level of detail, shaded), and
small on the right from the side and from above with the marks on them. A left
click on the body puts an exhaust there -- on its surface, the nearest point from
whichever view was clicked -- and holding the button grows it; **any of the three
views marks**, since some cars have their exhausts down the side, where the view
from behind cannot reach a point (the depth each view picks along is its own:
from behind the nearest z, from the side the nearest x, from above the nearest y,
`pick`); a left click on a mark
grows it, a right click shrinks it while held; a middle click or `d` takes it
away; `m` mirrors (on by default: a mark made, grown or shrunk on one side is
on the other too); The strip under the views is the colour the flames burn: hue across it,
brightness up, full saturation -- the game's own reader adds the near-white core
itself, so only the outer colour needs picking -- a click on it picks one, the
swatch beside it shows it and its hex, and a click there puts back the game's
own `#f2380a` (the file then naming none, as most do). What the file has is
shown when the car opens and written back untouched unless it is picked again,
so a hand-written `"flame"` -- in any of the forms the game reads -- survives a
visit to the tool. `c` colours the body in the game's own picture of
it -- `car00.tga` out of its `car.viv`, the paint and the details its triangles
are mapped into (`textured_view`, `car_exhausts.py`) -- instead of the grey
shading, and off by default (the grey is quicker to browse cars with); `s` or
Save writes them, `r` or Revert goes back.

Colouring is a small texture-mapper of the tool's own: each triangle's three
corners are its (u, v) into the 256×256 picture, so the face is filled with the
part of the picture they enclose, sampled a pixel at a time, its whole area lit
as one face the way the game lights a body (`lit`, the light turned to suit each
view). Faces turned away are left out, their normals first turned to point out
of the car -- a body's parts are not all wound alike -- and farther faces are
drawn first, so the near ones cover them. The rear view, the big one, is drawn
at 340 pixels a metre, the two small ones beside it at 190; the heaviest car's
4,800 triangles take about a second and a half, the rest a good deal less. A
car with no `car00.tga` (or no PIL to read the TGA) falls back to its paint, out
of `car.fce`'s palette by the page each triangle names (`car_model.page_colours`,
the rule the OBJ export paints its materials by), its wheels black. They are
kept in the car's folder as `exhausts.json`:

    {"flame": "#f2380a",
     "exhausts": [{"x": 0.45, "y": -0.32, "z": -1.98, "radius": 0.056}, ...]}

in the model's own space (car.fce), metres: x to the car's right, y up, z
forward. A car with no file has no exhausts, and so no flames (Exhaust flames
when gearing up). `flame` is the colour they burn, left out of most files: the
game's own `#f2380a` then. The tool does not edit it and writes it back as it
found it, so a car's colour is set by hand.

Three things are worth knowing before tuning:

- **Acceleration comes from two different places.** The eight `AI ACC`
  sections are one curve — acceleration in m/s² against speed in 1 m/s steps,
  running out exactly at the car's maximum velocity — and that is what the AI
  drives to; it is what the tool integrates for its 0-100 and 400 m, and for
  the retail cars it reproduces the showroom figures to within about 0.2 s.
  The player's car gets its go from the torque curve, the gearbox and the
  mass, which the tool draws as the dashed estimate. So editing mass, gearing
  or the torque curve moves the dashed curve and not the table: a tuned car
  needs its `AI ACC` sections edited too if the AI is to keep up.
- **Gearing is stored twice.** `gear ratios` and `final gear` are the gearbox,
  but the speed at a given rpm comes from `velocity to rpm ratio`, and the game
  reads that one — `velocity to rpm ratio = ratio × final × 60 ÷ (wheel
  circumference)`, which the retail files satisfy. `set` and `clone` recompute
  it when the gearing changes (`--keep-v2r` leaves it alone), which is why a
  change to the final drive moves the gear chart.
- **The showroom numbers are blurb, not data.** The `220 kW @ 4800 rpm` in
  `fedata.eng` is what the brochure says; the torque curve in `carp.txt` is
  what the car is simulated from, and for some cars the two disagree (the HSV
  curve peaks at 600 where the brochure claims 475 Nm). The tool shows both
  side by side rather than picking one.

#### Building the performance for a car of your own

Nothing has to be authored from a blank page: take the retail car nearest to
what you want, copy it, then move the numbers.

    # 1. a car of its own: new folder, new id, new name, and a starting point
    python3 tools/car_perf.py clone hgts mycar --id 60 --name "My Car"

    # 2. the numbers, as field=value in the labels' own words, or a JSON preset
    python3 tools/car_perf.py set mycar mass=1250 "final=3.75" grip=3.10 \
            "torque=300,320,350,...,200" --preset tweaks.json

    # 3. the acceleration tables, from the engine, gearing and mass you just set
    python3 tools/car_perf.py derive mycar          # --match 5.2 to aim at a 0-100

`derive` is the piece that makes a tuned car coherent. The eight `AI ACC`
sections are a curve of acceleration against speed that nothing else
recomputes, so without it your car would be quicker for you and stock for the
AI. It rebuilds them by running the car's own torque curve through its own
gearbox and mass, then scaling that estimate until it gives the 0-100 the car
already has -- so the shape is the model's, the level the car's, and the model's
own cheerfulness (it has no clutch, tyre or traction loss in it) is taken out
car by car. The scale is per car rather than one factor for the field, which
matters on an add-on whose engine figures are optimistic (`clkg`'s make it 3.4
times quicker than it is): aimed greedily, it would hand the AI a 1.4 s table to
drive to. `--scale 1` writes the raw estimate, `--match <seconds>` aims the
table at a 0-100 you choose, and `--dry-run` shows what would change. The
editor's "Rebuild AI tables on Save" box does the same on Save, except that it
keeps the tune's own improvement -- anchored on the car's shipped 0-100 -- so
the AI gets quicker when you made the car quicker.

The torque curve is 41 numbers, one every 256 rpm from 0 to 10240 — in the
editor each is a mark on the chart to drag, no typing — and the other arrays
are fixed sizes too — `list --fields` gives every field and its size, and a
wrong count is refused rather than written. An `AI ACC` section is written as
many numbers as that section already holds: the retail cars keep 14 each, an
add-on's file can differ (`clkg`'s first is a whole 98-sample curve), and the
table is as long as the car's own. What is worth setting
first, in the order it changes the car: `mass`, `torque`, the gear `ratios` and
`final` (whose `velocity to rpm ratio` follows automatically), `vmax` and
`speed cap`, then the handling set — `grip`, `brake decel`, `front grip`,
`turn in`/`turn out`, `turning circle` — and `acc` sections only through
`derive`.

### Giving a car a body

The body is a car's `car.fce`: the mesh, its parts and its paint. The format is
EA's NFS3 FCE and it is documented — TheXDS/VivLib keeps the specification
(`docs/FileFormats/FCE-specs.md`) — with a fixed `0x1f04`-byte header full of
table offsets: vertices (12 bytes each, part-local, in metres), normals, then
56-byte triangles of a texture page, three vertex indices, material flags and
three (u, v) pairs; a part table with each part's origin, name, vertex range
and triangle range; and two palettes of 16 colours in hue/saturation/
brightness. The parts are the game's levels of detail — "hi body" with its four
wheels, then "medium", "small" and "tiny" — plus odd extras such as
"high headlights".

`tools/car_model.py` reads them:

    python3 tools/car_model.py info hgts             # parts, size, paint, pages
    python3 tools/car_model.py preview hgts          # draw it, no textures
    python3 tools/car_model.py export hgts --out /tmp --lod high
    python3 tools/car_model.py import hgts mycar.obj # a body of your own

`preview` draws the mesh shaded from two angles; `export` writes an `.obj` with
the parts as groups and a `.mtl` per texture page, Z-up for Blender. Vertices
are stored *per part*, so both move each one by its part's origin — skip that
and the wheels pile up inside the body.

A car can be given a body nobody has modelled for it yet:

    python3 tools/car_model.py swap mycar --from diab --textures

That puts the donor's `car.fce` — and with `--textures` its `fedata.fsh` paint
palette, with `--dashboard` its `dash.qfs`, with `--showcase` its `car00.tga` —
into the car's archive and beside it. Verified by putting the Diablo's body and
paint on the Ferrari 550's slot and starting the game: the car screen shows a
Diablo. The physics stay yours, and so do the names in `fedata.eng` — only the
shell is the donor's. Two things to expect: the wheels sit where the *donor*
had them (their origins are in its mesh, not in `carp.txt`), and the dashboard
and the showcase picture keep their own shapes unless you swap those too.

A mesh of your own is an `.obj` away. `import` builds the `car.fce` and puts
it in the car's archive:

    python3 tools/car_model.py import mycar mycar.obj --base hgts

The format held nothing back: the tables above are everything the game draws
from, and the three reserved areas at header `+0x1c`/`+0x20`/`+0x24` — 32 bytes
a vertex, then 12 and 12 — are scratch. Reserved #1 is the record the renderer
transforms a vertex in: `sub_4bf4c0` writes x, y, z and 1/z over it every frame,
`sub_49cfa0` zeroes its `+0x14`, and a retail file therefore holds the *screen*
coordinates of whatever frame last drew the car (FM50's start 393.3, 237.1 — not
a vertex of its mesh). Zeroing all three in a retail `car.fce`, in the loose file
and in `car.viv`, still renders the car in the showroom and races it unchanged.
`import` leaves them zero; `info --reserved` prints what a retail file puts
there for reference.

What the mesh has to bring is what the game cannot work out:

- **Parts**, one per `o`/`g` group, the first being the body. A group that names
  its wheel and side (`wheel_FL`, `front left wheel`) becomes the name the game
  knows — `high left front wheel` — because those are the parts it steers and
  rolls; any other name is kept. Each part's origin is its bounding box's centre,
  which is the pivot a wheel turns about, and its vertices are stored relative to
  it.
- **Four levels of detail**: the game picks one by distance, and a car carrying
  only the near one is drawn at one distance. `--lods dup` (the default) gives
  the missing levels the near one's geometry again — triangles spent, never the
  wrong thing shown; `--lods single` leaves them out. A mesh that brings its own
  levels (`export` writes all four) keeps them.
- **(u, v) per corner**, into the car's one texture page. That page is the car's
  own `fedata.fsh`, so a new body either paints itself with the retail car's
  layout or ships a new `fedata.fsh`. Corners with no `vt` take `--uv u,v`.

Everything else comes from the model it replaces (`--base` for another car): the
two 16-colour paint palettes, the headlight and brake light positions at header
`+0x34`, the unknown corners of the header, and half the model's size is
computed from the mesh. Axes: the game is Y up with `+Z` forward, which is what
Blender's `.obj` export gives (`--up y`, the default, flips its forward `-Z`);
`--up z` for what `export` itself writes.

Verified both ways round: `export hgts --lod all` then `import` of that file
gives back 821 vertices, 1104 triangles and the same world geometry, triangle
for triangle, and the game renders it identically; a hand-written box body with
four box wheels imports, shows in the Player Car screen and races.

### Desktop (Windows)

Uses the prebuilt SDL3 in `sdl3/`. CMake ≥ 3.15 and a C++17 compiler:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run it with the path to your game folder:

```bash
./build/nfs3hp /path/to/game
```

With two arguments the second is the CD path, for data split between an install
directory and the disc.

### CMake options

| Option | Default | Description |
|---|---|---|
| `WITH_MMX` | `ON` | MMX bit in the emulated CPUID. Leave it on: reported absent, the game picks different copy routines and the movie streamer breaks. |
| `WITH_PEDANTIC_FPU` | `OFF` | Every x87 value in 80 bits and the x87's own arithmetic to the bit (`src/lib/x87soft.cpp`). Any GCC or Clang build, Android included (`-Px87Exact`); not MSVC. |
| `NFS_TRACE_MSG` | `OFF` | Message-pump and Glide state tracing. Very verbose. |
| `NFS2_ASSERT_TRAP` | `OFF` | Unsupported-path asserts break into the debugger instead of logging. |
| `NFS_BUILD_FF_TESTS` | `OFF` | Build `force_feedback_checks`, the force-feedback (`lib/forcefeedback.h`) regression test, against an SDL virtual joystick. |
| `NFS_BUILD_MEMORY_TESTS` | `OFF` | Build the guest memory allocator checks and `glidetmu_checks`, the texture atlas regression test. |

## Environment variables

Read once at startup. On Android the launcher sets `NFS_ORIENTATION`,
`NFS_FPS_CAP`, `NFS_GAMMA`, `NFS_BRIGHTNESS`, `NFS_CONTRAST`, the `NFS_TOUCH_*`
keys and one variable per pad button from its own screens, and `NFS_AUDIO_RATE`
from the phone; the rest matter for a desktop run.

### The switches as a file: `nfs3hp.ini`

Every switch below can also live in `nfs3hp.ini`, beside the game data --
`build/game/` for a desktop run, the app's external files directory on Android.
It is read once, at the very start of `main`, before anything asks the
environment for a switch (`settingsLoad`, `native_settings.cpp`), and each line
it has *uncommented* is put into SDL's environment; a switch that is already
there wins over the file, so a launch script and `adb shell am start --ez ...`
keep the last word.

```ini
# NFS_PARTICLES = 1
NFS_PARTICLES = 2
NFS_FLAME_LIGHT = 1.5
```

The file is written out on the first run with every switch the port reads, in
groups, **commented out at the value the port gives it** -- so the defaults have
one place to look at, and a line is changed by removing its `#`. `[Groups]` and
`#` remarks are carried over untouched. The port's own menus change a switch
with `settingsSet` (which also uncomments its line where it stands) and write
the file back with `settingsSave`, so a setting made in a menu is still there
the next time the game starts.

| Variable | Default | Description |
|---|---|---|
| `NFS_FPS_CAP` | `30` | Frame rate the renderer paces presents to. The game's logic is tuned for 30 Hz. |
| `NFS_VSYNC` | adaptive | Where the driver has it, a frame that misses the screen's refresh goes out at once, torn, instead of waiting a whole refresh more (at 30 frames a second on a 60 Hz screen a 34 ms frame was shown at 50); on time, a frame waits for the refresh as before. `strict`: plain vsync. Android's EGL refuses it and keeps plain vsync. |
| `NFS_WINDOW` | unset | Desktop only: the window's size as `WIDTHxHEIGHT`, e.g. `1600x900`. Unset, 1280x720 fitted to the display. |
| `NFS_FULLSCREEN` | on | Desktop only: `0` opens a window instead of the whole screen; `1` or unset starts borderless fullscreen, as Alt+Enter's is. |
| `NFS_GAMMA`, `NFS_BRIGHTNESS`, `NFS_CONTRAST` | `1.0` | Applied to the finished frame in the final blit, so menus and movies are covered as well as a race. |
| `NFS_TEXTURE_FILTER` | on | `0`: textures sampled as the Voodoo did -- the game's one level, no levels made, no anisotropy (Texture filtering and the road's grain). |
| `NFS_ANISOTROPY` | `8` | Anisotropic filtering of the textures, at most what the GPU offers; `1` for none. |
| `NFS_ROAD_DETAIL` | `0.35` | How much the road's and the ground's grain lightens and darkens them, 0 to 1; `0` for none. |
| `NFS_OBJECT_DETAIL` | `0.3` | The same for the rest of the world -- walls, buildings, trees, fences, grass, rocks -- finer, on opaque texels only; `0` for none. |
| `NFS_HAZE` | on (`0.8`) | `0`/`off`: the fog worked out at the corners and mixed into its flat colour, as the game has it. A number: how far the fog fades towards the sky behind (Haze, glow, colour grade and anti-aliasing). |
| `NFS_BLOOM` | on (`0.6`) | `0`/`off`: no glow around lights brighter than the sky. A number: how strong. |
| `NFS_GRADE` | on | `0`/`off`: no colour grade per track and light. |
| `NFS_AA` | on | `0`/`off`: no FXAA over the race's picture. |
| `NFS_EXPOSURE` | on (`0.7`) | `0`/`off`: no auto-exposure. A number up to 1: how much of the difference from the track's usual brightness is made up (Haze, glow, colour grade and anti-aliasing). `NFS_EXPOSURE_TRACE=1` prints it once a second. |
| `NFS_CAR_SHINE` | `1` | How much the cars' reflection is shaped by how the body faces the view -- sharper highlights, stronger towards the edges -- 0 to 1; `0` the game's own (The cars' paint). |
| `NFS_TEXTURE_SHARP` | on | `0`: textures seen larger than they are drawn bilinear, as the GPU does, instead of Catmull-Rom. |
| `NFS_HEADLIGHT_REACH`, `NFS_HEADLIGHT_WIDTH` | `2`, `1.4` | How far the headlights' patch on the road reaches and how wide it is, against the original's (0.25 to 4; `1` and `1` are the original). |
| `NFS_SOFT_EFFECTS` | on | `0`: the headlights' pool, the wet road's light streaks and the cars' shadows as the game draws them, hard-edged. |
| `NFS_SHADOW_STRETCH` | `0.7` | How far a car's shadow is stretched away from the sun, in its own half sizes (0 to 3; `0` the original). |
| `NFS_SHADOW_RAIN`, `NFS_SHADOW_NIGHT` | `0.8`, `0.4` | How strong a car's shadow is in the rain and at night, against daylight (0 to 1; `1` unchanged). |
| `NFS_WET_ROADS` | `1` | How wet the road is in the rain -- darker, puddled, reflecting (0 to 2; `0` never). |
| `NFS_ORIENTATION` | unset | `auto` allows both landscape directions; anything else pins one. Must be set before `SDL_Init`. |
| `NFS_CAR_DETAIL_FULL` | on | With Car Detail at High, every car keeps its detailed model and the player's texture size out to the draw distance, in every view, and the limit on how many are drawn at once is lifted. `0` restores the original. |
| `NFS_MIRROR_FULL` | on | The rear-view mirror is drawn as the main view is: its distances, the track's detailed models, cars at full detail, the sky with clouds and lightning, car lights and the headlights' light. `0` restores the original short, coarse mirror. |
| `NFS_WIDESCREEN` | on | `0` takes the 1280x720, 1600x900 and 1920x1080 modes back out of the Screen Size list. |
| `NFS_TEXTURES32` | on | `0` has the Voodoo2 driver refuse 32-bit textures again, so the game shrinks them to 16 bits as it used to. |
| `NFS_TEXCOORD_REBASE` | on | `0` leaves far-out texture coordinates as the game sends them (see Projected headlights), to compare. |
| `NFS_DEPTH16` | auto | On where the depth buffer has more than 16 bits and the GPU is not an Adreno 5xx. `0` keeps the GPU's own depth precision, `1` forces the Voodoo's 16-bit steps (see Projected headlights). `ab` switches between the two every 5 s, so both can be compared in one race; the `[GPU]` lines count the frames of each. |
| `NFS_GPU_FILTER` | on | `0` filters every texel in the shader. By default the GPU's bilinear filter takes a pixel whose 2x2 texels lie inside their atlas tile and have no chroma key. |
| `NFS_TEXCOORD_CUT` | off | `1` cuts triangles with far-out clamped texture coordinates along the texture's edges. An experiment, not needed for the headlights. |
| `NFS_NATIVES` | on | `0` runs the generated code instead of every native stand-in for the hot vertex and polygon loops; `vertices` keeps only the three vertex loops; `no-clip` all but the clipping of off-screen polygons (`sub_4c2cd0` and its callers `sub_4c11b0`, `sub_4c12e0`); `no-render` all but the ones in `src/nfs3hp/native_render.cpp`. For comparing speeds. |
| `NFS_RENDER_OFF` | unset | Addresses of functions in `src/nfs3hp/native_render.cpp` to leave to their generated code, e.g. `4bf790,47a880`. |
| `NFS_NATIVE_LOAD` | on | `0` leaves the loading of the game's data to the generated code: RefPack, the FSH textures' alpha scans, FCE models and FRD tracks (`src/nfs3hp/native_loading.cpp`). `NFS_NATIVE_CHECK=load` checks only these, every call. |
| `NFS_MAD` | on | `0` plays the movies with the generated MAD player, decoder and display instead of `src/nfs3hp/native_movie.cpp`; `NFS_MAD_PLAYER=0` and `NFS_MAD_GL=0` leave just the player or the full-colour output generated. `NFS_MAD_CHECK=1` (debug) runs the generated player and checks the native decoder against the generated one ([MAD]). |
| `NFS_THRASH_GL` | on | `0` draws through the old renderer, the Voodoo2 emulated over a texture atlas with its filtering done in the shader, instead of `ThrashRenderer`, which gives each of the game's textures an OpenGL texture of its own and lets the GPU filter, wrap and clamp it. |
| *(release)* | | A release build writes nothing to the log and takes none of these flags: the native code is built with `NFS_RELEASE` (log priority critical, SDL's direct Android logging compiled out through `android/app/jni/sdl_quiet.h`), the Java side logs through `AppLog`/`SDLLog` only when `BuildConfig.DEBUG`. Use the debug build (`-Pandroid10`) for diagnostics. |
| `NFS_FPU_EXTENDED` | on | Races at the x87's extended precision, as the Modern Patch runs them; `0` races in single precision, as the original (see The x87's precision). Every player of a network race needs the same. A debug build takes it from a launch: `adb shell am start -n dev.nfs3hp.port/.SplashActivity --ez fpu_single_races true`. |
| `NFS_NATIVE_AI` | on | `0` runs the generated sub_4064f0 and sub_4070c0 instead of their decompiled C++ (`src/nfs3hp/native_ai.h`). A debug build takes it from a launch: `--ez native_ai_off true`. |
| `NFS_AI_TUNE` | on | `0`: the opponents drive as the game has them -- no getting unstuck, no looking ahead for splits, no flashing, no characters, no extra pace (The opponents' driving, tuned). |
| `NFS_AI_PACE` | `1.06` | How much harder the opponents pull (their aim for speed times half the difference), 0.5 to 2; `1` the game's difficulty. |
| `NFS_GRID_TRAFFIC` | on | `0`/`off`: a full grid has no traffic, as the game has it. On, Traffic can be chosen with a full grid, and it is a third of the usual: two cars for one player (The grid). |
| `NFS_PURSUIT_GRID` | on | `0`/`off`: Hot Pursuit always one opponent, as the game has it. On, its Number can be chosen; a full grid is six opponents, as many as the cops' radio has banks for (The grid). |
| `NFS_AI_TRACE` | off | `1` logs which car each opponent is at the start of a race, then each opponent once a second (and where the cops and the traffic are), every step it takes to get unstuck, every split it steers round and every time it flashes the player (`[AI]`). |
| `NFS_NET_PORT` | `9803` | TCP port of network races, as the Modern Patch has it; the launcher's Network screen offers the original's `1030`. |
| `NFS_NET_TRACE` | off | `1` (extra `net_trace`) writes a network race into the log: EA's comm library events and packet sends (`[COMM]`), up to 400 datagrams per socket instead of 16 (`[NET]`), and each new indirect call target once per thread after the IPX transport opens (`[CALL]`). For finding where two machines stop on the way into a race. |
| `NFS_FE_NATIVE` | on | `0` leaves every hooked decompiled menu function, the screens' and the engine's, to the generated code (see Decompiled menus); the main menu, what it calls of the menu loop and the dialogs, and the controls' screens are native whole and stay native. |
| `NFS_FE_NATIVE_OFF` | unset | A list of generated functions, `sub_437e00,sub_438260`, left to the generated code while the other decompiled ones run. |
| `NFS_NATIVE_CHECK` | off | `1` runs each native stand-in against the generated code and logs any difference (`[NATIVE]`). The polygon and car loops are compared over all of guest memory once a second. Slow, and draws some triangles twice. `clip` checks only the stand-ins of the clipping (both ways to the screen, the car's quad mesh included), the track, the effects, the car lights and detail, the objects and the vector math, every call; the small helpers called from hundreds of places are checked on every 64th call. |
| `NFS_AUDIO_RATE` | unset | The output's own sample rate. The game mixes at 22050 Hz and SDL converts to this; a device opened at any other rate is resampled by Android, off its low-latency path. Unset, the device opens at SDL's default. |
| `NFS_VOLUME` | `1.8` | How far the game's sound is lifted on its way to the device (`include/lib/loudness.h`, applied in `src/nfs3hp/native_sound.cpp`), 0.1 to 8. Above -2.5 dBFS the lifted sound's peaks are rounded off (a soft saturator), and a limiter that looks 3 ms ahead holds what is left under -1 dBFS; `1` leaves the mix as the game made it. Higher is louder but rounds off more of the sound. The Audio screen's own sliders start at 50%, and raising them is the other way up. |
| `NFS_NOW_PLAYING` | on | `0`/`off`: no panel naming the song as a race starts. `1`/`on`: it stays five seconds; another number up to 30: that many (The song now playing). |
| `NFS_NEXT_SONG_KEY` | `0x5d001b04` (`]`) | The key that starts the next song in a race, as the game writes a binding: its character << 24, its scan code << 8, 4 for the keyboard. Set from Controls (Next song); `0` for none (The song now playing). |
| `NFS_HEADLIGHT_OBJECTS` | on (`1.6`) | `0`/`off`: the player's headlights light only the road, as the game has them. A number: how bright they are on the walls, fences, trees and buildings (Headlights on what stands by the road). |
| `NFS_MOTION_BLUR` | on (`2`) | `0`/`off`: no motion blur. A number up to 2: how strong the streaks outside the sharp ellipse are (Speed you can feel). |
| `NFS_LIGHT_GLOW` | on (`1`) | `0`/`off`: the lamps' glow as the game has it, a star of spikes. A number up to 3: how strong the halo round the soft glow (The lamps' glow, and their trails). |
| `NFS_LIGHT_TRAILS` | on (`1`) | `0`/`off`: no streak. A number up to 3: how long the short streak under each lamp at speed, on the screen. |
| `NFS_TAILLIGHT_TRAILS` | on (`1`) | `0`/`off`: no trails behind the cars' tail lights. A number up to 3: how long they grow with speed, from 80 km/h (The lamps' glow, and their trails). |
| `NFS_PARTICLES` | on (`1`) | `0`/`off`: the game's own dust and sparks. A number up to 3: how much dirt is thrown (Dust, grit and sparks of the port's own). |
| `NFS_EXHAUST_FLAMES` | on (`1`) | `0`/`off`: no flames out of the cars' exhausts when they gear up (Exhaust flames when gearing up). |
| `NFS_EXHAUST_DEFAULTS` | off (`0`) | `1`/`on`: a car with no exhausts marked gets two default pipes at its rear corners, and so flames too (Exhaust flames when gearing up). A marked car keeps its own. |
| `NFS_FLAME_LIGHT` | on (`1`) | `0`/`off`: no pool of light on the road under each exhaust flame. A number up to 3: how strong (Exhaust flames when gearing up). |
| `NFS_WEATHER_MAX` | `2.5` | How heavy the rain or snow comes at its heaviest, against the mean, which is as much as the game throws; it comes and goes over the race (The weather's flakes and drops, the port's own). 1 to 10; `1` steady. `NFS_WEATHER_TRACE=1` prints the game's rate and the weather's once a second. |
| `NFS_LENS_RAIN` | `1` | How many raindrops on the view in the rain, magnifying what is behind them, streaking off at speed, none under a roof; up to `2`, `0`/`off` none (The weather's flakes and drops, the port's own). |
| `NFS_PARTICLES_HALF` | on | `0`/`off`: the spray's mist and the smoke drawn full size rather than in a half-size layer (The weather's flakes and drops, the port's own). |
| `NFS_PERF_TRACE` | unset | `1`: once a second, what the port's particles cost the game thread (and the mirror's), how many there are and how many are added a frame, and where the GL thread's time goes in a race frame -- the world, the particles, the rest of the whole-picture pass -- each ended by a `glFinish`, so slower while it is on (`[PERF]`); and at once, any frame over 35 ms on the game thread, with the textures the game uploaded in it, their size and the time they took (`[PERF] slow frame`). `2`: each costly effect left out in turn, two seconds each -- haze, bicubic textures, grain, the wet road, headlights on objects, object shadows, motion blur, FXAA, glow, the lamps' halo, light trails, lens rain, auto-exposure -- and a line for each of the times without it, to find what each costs on this GPU; the picture changes as it goes. |
| `NFS_WEATHER_PARTICLES` | on | `0`/`off`: the game's own flakes and drops, drawn as squares cut out of nothing, instead of the port's (The weather's flakes and drops, the port's own). |
| `NFS_SPRITE_TRACE` | unset | `1`: one line for each kind of draw in the frame -- the texture the game gave it, its format and levels, whether and by what colour it is cut out, what it is blended by, its depth and clip. For finding a sprite that is cut out by nothing. |
| `NFS_CHROMA_TRACE` | unset | `1`: print each colour the game cuts its pictures out by, once each (`[CHROMA] keyed out: 0x0000f800 (0,248,0)`), for telling one picture's key from another's. |
| `NFS_TYRE_SMOKE` | on (`1`) | `0`/`off`: the game's own pictures for the smoke off a wheel spinning or locked. A number up to 3: how much more of the port's, and thicker (Tyre smoke). |
| `NFS_EXHAUSTS_TRACE` | unset | `1`: print each car's exhausts, every tip's place and the flame colour, as the loader read them, before the game starts, and during a race a line for each car the first time it shifts up, pops on a downshift, crackles on the overrun, revs at a standstill, or is given the defaults -- and the revs a car at rest climbs to, every 1000 rpm. |
| `NFS_WHEEL_SPRAY` | on | `0`/`off`: the game's own spray off a wet road instead of the port's (Grass, and spray off a wet road). |
| `NFS_SNOW_SPRAY` | on | `0`/`off`: no frost off a tyre on snow (Frost off snow). |
| `NFS_GAME_SPRITES` | off | `1`/`on`: the game's own particle sprites drawn again -- the pool's turned squares and the drops that land on the camera, none of them cut out by anything. Off, they are drawn only where the port leaves a kind to the game: its particles or one of their switches off, or split screen (The game's own sprites are not drawn). |
| `NFS_SUN_GLARE` | on (`1`) | `0`/`off`: no glare from the sun. A number up to 3: how strong (Sun glare); by day with no weather only. |
| `NFS_GRIP_ON_OPPONENTS` | on (`1`) | `0`/`off`: grip by terrain for the player's car only, not every car (Grip by terrain). |
| `NFS_SLIPSTREAM` | on (`1`) | `0`/`off`: no slipstream. A number up to 3: how strong the push behind another car (Slipstreaming). |
| `NFS_SLIPSTREAM_TRAILS` | on (`1`) | `0`/`off`: no air lines round the player's car in a slipstream (Slipstreaming). |
| `NFS_KNOCK_TREES` | on (`1`) | `0`/`off`: trees and lamp posts stand as the game has them, solid (Trees and lamp posts knocked down). |
| `NFS_TREE_MASS` | `1` | Times 150 kg: how much a tree slows the car that knocks it down; `0` for none. |
| `NFS_LAMP_MASS` | `1` | Times 70 kg: the same for a lamp post. |
| `NFS_KNOCK_RESPAWN` | `10` | Seconds before a knocked-down tree or lamp post stands again. |
| `NFS_CRASH_SHAKE` | `2` | How hard the camera shakes and vibrates on a hit, a scrape or a hard landing; up to `3`, `0`/`off` for none (Crashes you feel). |
| `NFS_TYRE_MARKS` | on | `0`/`off`: the game's tyre marks, faint and kept until 64 blocks of them are used (Tyre marks). |
| `NFS_TYRE_MARKS_LIFE` | `60` | Seconds a tyre mark lasts, fading over its last six. |
| `NFS_BODY_MOTION` | `1` | How much the cars' bodies lean in corners, tilt on slopes and settle on landing, as drawn only (the driving is untouched); up to `3`, `0`/`off` none (Bodies on their springs). |
| `NFS_AIR_STEER` | `0.2` | How much the steering turns the player's car in the air; up to `3`, `0`/`off` none (Steering in the air). |
| `NFS_AIR_PITCH` | `0.2` | How much the pedals tip the player's car in the air: the brake the nose up, the throttle down; up to `3`, `0`/`off` none (Steering in the air). |
| `NFS_CRASH_SOUND` | on | `0`/`off`: the game's crash and scrape sounds as they were, none of the port's crash, no pitch by the hit. |
| `NFS_AIR_ROLL_DAMP` | on (`1`) | `0`/`off`: cars keep their roll in the air, as the game has it. A number up to 3: how fast it dies away (Cars roll less in the air). |
| `NFS_AIR_SPEED` | on | `0`/`off`: a car off its wheels is dragged down by the game, as it has it, so a jump costs speed the ramp never took. On, the speed it took off with is put back each tick, its fall left to gravity; a hit in the air still loses what a hit loses (Cars keep their speed in the air). |
| `NFS_FAST_LOAD` | on (`1`) | `0`/`off`: the loading picture's frames paced and all shown, as before (A race loads in a moment). |
| `NFS_SURFACE_GRIP` | on (`1`) | `0`/`off`: every surface as the game has it. A number up to 3: how much the player's car feels grass, dirt, snow and mud (Grip by terrain). |
| `NFS_TRAFFIC_WEIGHT` | `0.5` | How heavy the traffic is against the game's, 0.1 to 2; `0`/`off` for the game's (Traffic lighter). |
| `NFS_COP_WEIGHT` | `1.25` | How heavy the cops are against the game's, 0.1 to 3; `0`/`off` for the game's (Traffic lighter, cops heavier and faster). |
| `NFS_COP_PACE` | on (`1`) | `0`/`off`: the cops' cars as the game has them. A number up to 3: how much of the tuning (Traffic lighter, cops heavier and faster). |
| `NFS_SPIKE_STRIPS` | off | `1`: cops lay spike strips (in the making; Spike strips). |
| `NFS_CAMERA_PULL` | `1.2` | How far back, in metres, the chase camera eases under full throttle; `0`/`off` for none. |
| `NFS_SPARKS` | `2` | How many times the sparks of a burst, and whether walls throw them on hits and scrapes; `0`/`off`: the game's own (Sparks off walls). |
| `NFS_CAR_SHADOW` | on | `0`/`off`: no shadow under the cars (Car shadows). |
| `NFS_OBJECT_SHADOWS` | on (`0.45`) | `0`/`off`: no shadows from what stands by the road. A number: how dark (Shadows of what stands by the road); fainter in the rain and at night by `NFS_SHADOW_RAIN` and `NFS_SHADOW_NIGHT`. |
| `NFS_SND_MIX` | on | `0` leaves the game's sound mixer (`sub_500864` and everything under it) to its generated code instead of `src/nfs3hp/native_mixer.cpp`. `NFS_SND_CHECK=1` (debug) mixes each block natively and with the generated code and logs any byte that differs (`[SNDCHK]`); a block that calls the game, to refill a stream or end a sound, is left to the generated code. |
| `NFS_TOUCH_STEER_LEFT` etc. | unset | Keys the touch overlay sends for steering and the pedals, so that endpoint can report them as axes. |
| `NFS_GAMEPAD1_SOUTH` etc. | built-in defaults | What a pad button sends, as `NFS_GAMEPAD<n>_<BUTTON>`: an SDL key name (`Space`, `Return`), `axis:steer_left`, `axis:steer_right`, `axis:accelerate`, `axis:brake`, or empty for nothing. `_UI` after it is the same button in the menus. `LEFT_TRIGGER` and `RIGHT_TRIGGER` are the triggers read as buttons, empty by default; the launcher gives them the pedals' keys for a pad set to Digital. |

## Regenerating the recompilation

The repository ships the generated C++, so this is only needed if you change the
disassembler in `disasm/`:

```bash
pip3 install capstone
python3 disassemble_nfs3hp.py
```

It reads `nfs3hp/nfs3.exe` and the three DLLs and rewrites
`src/nfs3hp/disassembly`.

Between two places where control can arrive or leave — a label, a jump, a
call, a return — the generator keeps the x87 stack in C++ locals rather than in
the emulated FPU's memory, and writes it back where the run ends
(`disasm/codegen/fpu_stack.py`). The arithmetic is the same calls in the same
order, so the results are the same to the bit; on a phone it is about a quarter
more frames in the heaviest split-screen races. `NFS_FPU_LOCALS=0` in the
generator's environment writes every instruction against the emulated FPU as
before, and `NFS_FPU_LOCALS_CHECK=1` plays every run through both ways,
symbolically, and stops on the first one that differs.

## Credits

- [motor-dev/nfs-recompiled](https://github.com/motor-dev/nfs-recompiled) — the
  recompilation this is built on.
- [libsdl-org/SDL](https://github.com/libsdl-org/SDL) — SDL3, zlib licence.
- [americusmaximus/AZHP](https://github.com/americusmaximus/AZHP) — AutoZone Hot
  Pursuit, MIT licence.

Need for Speed III: Hot Pursuit is the property of Electronic Arts. This
repository contains no game content — no tracks, cars, audio or video. Those you
bring from your own disc.
