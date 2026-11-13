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

Four effects on a race's picture, each on by default and off with its
variable set to `0` (or `off`): `NFS_HAZE`, `NFS_BLOOM`, `NFS_GRADE`,
`NFS_AA`. The menus are never touched.

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

**Motion blur.** In the whole-picture pass (`motionBlur`): moving forward at v,
a still point at depth z drifts across the screen away from its centre by its
distance from it times v dt / z, so the near verges and walls streak and the
horizon stands. Each pixel is averaged with eight more along that line, the
length from the depth buffer and the first player's speed (`[car+0xb8]`, sent
each frame by `colourGradeTick`, 0 while the race's clock stands); a shutter
of a ninetieth of a second times `NFS_MOTION_BLUR` (2, picked by eye), from 10 m/s, whole
from 35, at most 48 pixels. The cars (0 in the picture's alpha) are neither
blurred nor taken into the blur around them.

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
10 m/s and whole from 35 (the speed the motion blur uses); the centre is the
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
car whatever the light. Its alpha now falls off over the outer third, round at
the corners, and the quad is stretched away from the sun: the sun the game
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
`NFS_SHADOW_RAIN` (0.55), and at night (`0x6fd4c8`) by `NFS_SHADOW_NIGHT`
(0.4) -- both together when both hold; 1 keeps the daylight shadow.

### Wet roads

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
a crash. Fixing it means letting `sub_439a50` answer by id — the record's own
id byte at `+0x31c` — instead of by place, so a place and an id stop meaning
the same number, and that is the next piece of work here.

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

`tools/car_perf.py` reads them, draws them, and writes them back:

    python3 tools/car_perf.py list                     # every car, side by side
    python3 tools/car_perf.py list --fields            # the fields you can set
    python3 tools/car_perf.py show hgts                # a window
    python3 tools/car_perf.py show hgts --out hgts.png # a picture instead
    python3 tools/car_perf.py compare hgts diab ffal
    python3 tools/car_perf.py set hgts mass=1500 "final=4.10"
    python3 tools/car_perf.py clone hgts mycar --id 60 --name "ZZZ 001" mass=1500

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
gearbox and mass, then applying the factor the model is optimistic by —
measured across the whole retail field (about 0.96) — so the tuning shows
through and the model's own cheerfulness does not. `--scale 1` writes the raw
estimate, `--match <seconds>` aims the table at a 0-100 you choose, and
`--dry-run` shows what would change.

The torque curve is 41 numbers, one every 256 rpm from 0 to 10240, and the
other arrays are fixed sizes too — `list --fields` gives every field and its
size, and a wrong count is refused rather than written. What is worth setting
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

| Variable | Default | Description |
|---|---|---|
| `NFS_FPS_CAP` | `30` | Frame rate the renderer paces presents to. The game's logic is tuned for 30 Hz. |
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
| `NFS_CAR_SHINE` | `1` | How much the cars' reflection is shaped by how the body faces the view -- sharper highlights, stronger towards the edges -- 0 to 1; `0` the game's own (The cars' paint). |
| `NFS_TEXTURE_SHARP` | on | `0`: textures seen larger than they are drawn bilinear, as the GPU does, instead of Catmull-Rom. |
| `NFS_HEADLIGHT_REACH`, `NFS_HEADLIGHT_WIDTH` | `2`, `1.4` | How far the headlights' patch on the road reaches and how wide it is, against the original's (0.25 to 4; `1` and `1` are the original). |
| `NFS_SOFT_EFFECTS` | on | `0`: the headlights' pool, the wet road's light streaks and the cars' shadows as the game draws them, hard-edged. |
| `NFS_SHADOW_STRETCH` | `0.7` | How far a car's shadow is stretched away from the sun, in its own half sizes (0 to 3; `0` the original). |
| `NFS_SHADOW_RAIN`, `NFS_SHADOW_NIGHT` | `0.55`, `0.4` | How strong a car's shadow is in the rain and at night, against daylight (0 to 1; `1` unchanged). |
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
| `NFS_HEADLIGHT_OBJECTS` | on (`1.6`) | `0`/`off`: the player's headlights light only the road, as the game has them. A number: how bright they are on the walls, fences, trees and buildings (Headlights on what stands by the road). |
| `NFS_MOTION_BLUR` | on (`2`) | `0`/`off`: no motion blur. A number: how long the shutter, times a ninetieth of a second (Speed you can feel). |
| `NFS_LIGHT_GLOW` | on (`1`) | `0`/`off`: the lamps' glow as the game has it, a star of spikes. A number up to 3: how strong the halo round the soft glow (The lamps' glow, and their trails). |
| `NFS_LIGHT_TRAILS` | on (`1`) | `0`/`off`: no streak. A number up to 3: how long the short streak under each lamp at speed, on the screen. |
| `NFS_TAILLIGHT_TRAILS` | on (`1`) | `0`/`off`: no trails behind the cars' tail lights. A number up to 3: how long they grow with speed, from 80 km/h (The lamps' glow, and their trails). |
| `NFS_CAMERA_PULL` | `1.2` | How far back, in metres, the chase camera eases under full throttle; `0`/`off` for none. |
| `NFS_SPARKS` | `2` | How many times the sparks of a burst, and whether walls throw them on hits and scrapes; `0`/`off`: the game's own (Sparks off walls). |
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
