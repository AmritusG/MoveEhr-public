#pragma once
#include <memory>
#include <unordered_map>
// Draws a GpuModel into an MSAA FBO and composites it into the host FBO (premultiplied alpha).
#include "GpuModel.h"
#include "Perf.h"
#include "MathUtil.h"
#include "SdfBuilder.h"
#include "Video.h"
#include "GlCount.h" // MoveEhr W6: <OpenGL/gl3.h> + the GL call counters
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mehr
{
struct PartControls
{
	bool visible   = true;
	float hue      = 0;
	float glow     = 0;
	bool inputSkin = false;
	float surface  = 1;
};

// Stage 3 surface effects
struct SurfaceState
{
	bool on = false;
	float time = 0, beat = 0, audioMid = 0; // time: seconds, wrapped (random-step flicker / glitch only)
	float film = 0, filmThick = 1, filmShift = 1, filmSwirl = 1;
	float filmT = 0, filmT2 = 0, filmX = 0; // swirl drift, the same drift one loop back, cross-fade weight to it
	float glitter = 0, glitterDensity = 0.35f, glitterSize = 1, glitterMetal = 0.8f, glitterPhase = 0; // phase: twinkles
	float chrome = 0, chromeRipple = 0.6f, chromeOff[ 2 ] = { 0, 0 }; // ripple drift (y, z) in the 64-periodic noise
	float glass = 0, glassIOR = 1.45f, glassDisp = 0.4f, glassTint[ 3 ] = { 1, 1, 1 };
	float holo = 0, holoScan = 1, holoFlicker = 0.5f, holoSplit = 1, glitch = 0, holoColor[ 3 ] = { 0.2f, 0.9f, 1 }, holoPhase = 0;
	float heat = 0, heatTemp = 1900, heatSpread = 0.7f;
	int pattern = 0;
	float patAmt = 2, patScale = 5, patBeat = 0,
	      patT = 0, patT2 = 0, patX = 0, patAudio = 0, patColor[ 3 ] = { 0.2f, 0.9f, 1 };
};

// An extra spot / point light (volume lights A/B/C-left/C-right first, then the sweeping spots). World space.
struct XLight
{
	int type      = 0; // 0 spot, 1 point
	float pos[ 3 ]   = { 0, 0, 0 };
	float dir[ 3 ]   = { 0, 0, 1 }; // spot aim (unit)
	float color[ 3 ] = { 0, 0, 0 }; // colour x intensity
	float coneDeg    = 40;          // full cone angle
	float coneSoft   = 0.5f;
	bool shadow      = false;       // volume lights: render a shadow map
	float front      = -1.0f;       // >= 0: the medium only counts beyond this distance along dir (eye lights, Front Only)
};
// LoadEhr W1: four model slots (A..D). The renderer draws one pair at a time (the hop's outgoing and incoming model); every
// per-model cache (shards, dust, letters, liquid SDF, sim obstacle SDF) has one entry per SLOT, so a slot's data is built once,
// ahead of use, and a hop between any two slots finds it ready.
static constexpr int kSlots = 4;
// MoveEhr W5 (S5): an HDRI environment read from a Radiance .hdr (stb_image, linear floats) on a worker; error set = nothing usable
struct EnvImage
{
	std::string path, error;
	int w = 0, h = 0, srcW = 0, srcH = 0; // srcW / srcH: the file's size (w / h after halving to <= 4096 wide)
	std::vector< float > rgb;            // w x h x 3, row 0 = the top of the image
};
std::shared_ptr< const EnvImage > LoadEnvImage( const std::string& path ); // EnvFile.cpp; any thread
struct ProgSource // MoveEhr W1: a linked program's sources (interned process-wide), so its ANIM twin can be linked on demand
{
	std::shared_ptr< const std::string > vs, fs;
	bool capture = false;
};
enum SlotUse : unsigned { kUseShards = 1, kUseDust = 2, kUseLiquid = 4, kUseSim = 8, kUsePoints = 16, kUseCloud = 32, kUseSparkle = 64 };
// W6 Stage 2: Transition Style (0..4 as before; the pack 5..22 appended, so saved clips keep their style). W7 (Amrit: "Fold can go and
// also Shrink Wrap"): their values 6 and 22 are TOMBSTONES (not in the list, never renumbered: a clip holding one dissolves)
enum TransStyle : int
{
	kStyleDissolve = 0, kStyleShatter, kStyleDust, kStyleSlice, kStyleLiquid,
	kStylePoints, kStyleRetired6, kStyleWire, kStyleVoxel, kStyleLowPoly, kStylePieces, kStyleImplode, kStyleTwist, kStyleMelt, kStyleScan,
	kStyleGlitch, kStyleRipple, kStyleSwarm, kStyleSmoke, kStyleBurn, kStyleLava, kStyleSpinSwap, kStyleRetired22,
	kStyleValues // option values 0 .. kStyleValues - 1
};
static constexpr int kSKShadowUnit = 21; // W23: the key shadow map in the volume march (Smoke Key Light)
static constexpr int kMaxVolLights = 4; // A, B, C left, C right (only A/B can be point lights: cube maps 0/1)
static constexpr int kMaxXLights   = 8;

// Volume light (Stage 1): the medium and how it is rendered.
struct VolumeState
{
	bool active     = false; // any volume light on, or Smoke Ambient / Smoke Key Light with smoke
	bool projOnly   = false; // W26: on only for the projector's beam (the main march is skipped: it would add nothing)
	int method      = 0;     // 0 raymarch, 1 screen rays
	int lights      = 0;     // how many of FrameState::xl (from 0) are volume lights
	int fogLights   = 0;     // how many light the medium (volume lights + spots with beams)
	float spotBeams = 0;     // spots' brightness in the medium
	float haze = 0, smoke = 0, smokeFreq = 1, turb = 0, glowCloud = 0;
	float smokeAmbient = 0, smokeKey = 0; // W23: smoke lit without a volume light (ambient: key colour; key: key light + shadow)
	float flash[ 3 ] = {}; // W26 lightning: the strike's flash, added to the smoke's ambient light (colour x intensity)
	float drift[ 3 ]  = { 0, 0, 0 };
	float center[ 3 ] = { 0, 0, 0 };
	float radius = 3, g = 0.6f, shaftLen = 5, motes = 0, time = 0;
	int steps = 32, resDiv = 2, blend = 0;
};

// W20: the 3D smoke simulation (Smoke Engine Sim / Sim + Detail). World-aligned grid centred on the mask.
struct SimState
{
	int engine = 0;          // 0 noise (no sim), 1 sim, 2 sim + detail
	int res    = 64;
	float boxCenter[ 3 ] = { 0, 0, 0 };
	float boxSize        = 5;
	float unit           = 1; // world length of one fitted mask unit (= the mask's radius)
	float model[ 16 ];       // raw model units -> world, this frame
	float prevModel[ 16 ];   // the same, last frame (obstacle velocity)
	float frameDt = 1.0f / 60.0f;
	int iterations = 20;
	float buoyancy = 2, weight = 0.3f, heatDecay = 0.5f, vorticity = 2, fade = 0.3f, turb = 0.5f;
	float wind[ 3 ] = { 0, 0, 0 };
	float emitEyes = 0, eyesHeat = 1, eyePos[ 6 ] = {}, eyeDir[ 3 ] = { 0, 0, 1 }, eyeRad = 0.1f, eyeSpeed = 1;
	float emitBehind = 0, behindHeat = 1, behindPos[ 3 ] = {}, behindRad = 0.5f;
	float emitRim = 0, rimHeat = 1, emitInside = 0, insideHeat = 1;
	float emitOffset = 0; // world: Rim and Eyes start this far out of the surface
	float emitMul = 1;
	float confine = 0, confineCenter[ 3 ] = {}, confineR = 2;
	float tint[ 3 ] = { 1, 1, 1 };
	float project = 0;
	bool reset = false;
	// W24 Trip (sim side). warp: simulated seconds per second (0 = frozen: no substeps, the grid stays put)
	float warp = 1;
	float stepScale = 1; // W28 Fire Speed < 1: shorter substeps (one per frame) instead of skipped frames
	float maskCenter[ 3 ] = {}, maskUp[ 3 ] = { 0, 1, 0 };
	int ringN = 0; // W25: rings alive (newest first): drive, smoke-on, centre, axis, radius (world), hold (1 keep .. -2 dissolve)
	float ringK[ 2 ] = {}, ringS[ 2 ] = {}, ringPos[ 6 ] = {}, ringDir[ 6 ] = { 0, 0, 1, 0, 0, 1 }, ringR[ 2 ] = { 0.3f, 0.3f }, ringH[ 2 ] = {};
	float ringSpeed = 1, ringSpin = 0, ringSmoke = 0;
	int ghostN = 0;      // smoke copies of the mask being painted now (up to 3)
	float ghostInv[ 3 ][ 16 ] = {}, ghostK[ 3 ] = {}, ghostToWorld[ 3 ] = {}, ghostSmoke = 0;
	float whirl = 0, hyper = 0, hyperAxis[ 3 ] = { 0, 0, -1 }; // hyperAxis: camera -> mask (unit)
	bool trip = false;   // run the TRIP variants of the correct / force / project passes (any Trip look on)
	// W26 Stage 3 fire: the FIRE variants (TRIP + FIRE) run instead; smoke b = fuel
	bool fire = false;
	float fireFuel = 0, fireSmoke = 1, burn = 3, heatRelease = 6, soot = 0.5f, fireCool = 1.5f, lift = 4, fuelFade = 1;
	float flameTemp = 1900, flameGlow = 2, flameTint[ 3 ] = { 1, 1, 1 }, flameTintMix = 0, flameSoot = 0.3f;
};

// W24 Trip: looks on top of the smoke. medium = the TRIP march variant runs (Raymarch), post = prism / hue echo / contour
// lines (W25) on the fog.
struct TripState
{
	bool medium = false, post = false;
	int colourBy = 0, palette = 0;
	float palMix = 0, palShift = 0, palSpread = 1, palA[ 3 ] = { 1, 0, 1 }, palB[ 3 ] = { 0, 1, 1 };
	float film = 0, filmThick = 1;
	float prism = 0, prismAngle = 0; // prism: amount; angle in radians
	int kaleido = 1;
	float kTwist = 0, kSpin = 0;     // radians per world unit out from the axis; radians
	float fractal = 0, fracScale = 1, fracRot[ 9 ] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	int fracIter = 3;
	float veins = 0, veinWidth = 0.4f, veinColor[ 3 ] = { 0, 1, 1 };
	float contours = 0, bands = 8, bandSharp = 0.6f, bandScroll = 0;
	float lines = 0, lineWidth = 1.5f, lineGlow = 1; // W25 Contour Style Lines (a post pass; contours is then 0)
	float black = 0, blackColor[ 3 ] = { 0.5f, 0.1f, 1 };
	int blackBy = 0;
	float echo = 0, echoHue = 0; // echo: per-frame persistence (dt-corrected); echoHue: turns this frame
	float center[ 3 ] = {}, axis[ 3 ] = { 0, 0, 1 }, right[ 3 ] = { 1, 0, 0 }, up[ 3 ] = { 0, 1, 0 }, unit = 1;
};

// Stage 2 post: floor / wall (shadow catcher, mirror), silhouette, bloom + flares.
struct PostState
{
	float shadowAmt = 0, shadowSoft = 0.4f;
	float floorLight = 0, floorLightColor[ 3 ] = { 1, 1, 1 }; // key-lit pool on the catcher plane (display space)
	float catchPlane[ 4 ] = { 0, 1, 0, 1.15f }; // n.xyz, d
	float floorY          = -1.15f;
	float mirror = 0, mirrorBlur = 0.3f, mirrorFade = 0.4f;
	float maskCenter[ 3 ] = { 0, 0, 0 };
	float maskRadius      = 1.2f;
	float sil = 0, silFill = 0, silGlow = 2, silWidth = 6;
	float silColor[ 3 ] = { 0.3f, 0.9f, 1.0f };
	float bloom = 0, bloomThreshold = 0.65f, bloomRadius = 0.6f, ghosts = 0, streak = 0;
};

// Stage 4 shape (fitted object space; see kSoupVS)
struct ShapeState
{
	bool on = false; // any soup-stage deform active
	float slicePhase = 0, curlY = 0, dt = 0; // Slice Wave (radians); dust wind drift (64-periodic noise)
	int shardCount = 64;
	int shatterMode = 1; // W2 (text / logos): 0 Letters (whole letters fly), 1 Pieces (solid chunks), 2 Pieces + Letters (a letter's pieces share its kick)
	float shatterT = 0, shatterForce = 2, shatterGravity = 4, shardSpin = 2;
	float melt = 0, drip = 1;
	int sliceAxis = 1, sliceCount = 12;
	float sliceGap = 0, sliceWave = 0;
	float voxel = 0, voxelSize = 0.05f, lowPoly = 0, lowPolySize = 0.12f;
	float lowPolyGrid[ 9 ] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // mask -> grid rotation (column-major)
	int tumble             = 0;                             // 0 off, 1 together, 2 separate
	float tumbleAng[ 3 ]   = { 0, 0, 0 };                   // degrees: the steady turn, integrated, wrapped at 360
	float tumblePhase[ 3 ] = { 0, 0, 0 };                   // degrees: the Sway rocking phase, wrapped at 360
	float tumbleRandom = 0.5f, tumbleSway = 0, tumbleW = 1, facetGap = 0;
	bool flipOn    = false;
	float flipBase = 0, flipT = 0, flipDur = 0.3f, flipDeg = 90;
	int deformAxis = 1;
	float twist = 0, bend = 0, ripple = 0, rippleFreq = 12, ripplePhase = 0;
	// W2 Deform group (values already x Deform Amount): deform = any of them on (the DEFORM program twins draw)
	bool deform = false;
	float taper = 0, squash = 0, shear = 0, bulge = 0, spherize = 0;
	int shapeTarget = 0; // W9: Spherize's target: 0 sphere, 1 cube (W11: torus / crystal retired)
	float warp = 0, warpScale = 2, warpPhase = 0; // warpPhase: lattice cells, wrapped at 256
	float glitchShift = 0, glitchBands = 12, glitchStep = 0; // glitchStep: the step index, wrapped at 1000
	float foldAxes[ 3 ] = { 0, 0, 0 }, foldK = 0;
	int foldN = 1; // fold copies per draw (2 per folded axis)
	float jellyLin[ 3 ] = { 0, 0, 0 }, jellyAng[ 3 ] = { 0, 0, 0 }, jellyScale = 0;
	float dustCut = 0, dustWind = 1, dustSize = 1.5f, reformTime = 1.5f;
	bool dustInvert = false; // transition: the incoming model shows where the outgoing one has dissolved
	float slide = 0;         // transition Slice: slabs slide out sideways (+) / in (-)
	// W16 Shatter / Dust transition: every piece (shard / dust point) has an order key k in 0..1 and flies over
	// u = clamp( ( transE - stagger * k ) / ( 1 - stagger ) ). Side 1 = the outgoing model (away = u),
	// side 2 = the incoming one (away = 1 - u). shatterT = the flight time at away 1; dust uses transSpread.
	int transSide  = 0;
	bool dustTrans = false; // the Dust transition: points drawn at their place on the slider (no simulation)
	float transE = 0, transStagger = 0, transEase = 0, transSpread = 1;
	float transKT = 0; // Dust: the key the mesh is cut at (outgoing: shows k >= kT; incoming: shows k <= kT)
	int dustCount = 100000;
	bool dustReturning = false;
	bool dustRestart   = false; // this frame: the points snap home before the step (Blow To Dust while out)
	// W26 Stage 9: Shatter Physics Bounce = shards fly a recorded CPU flight (gravity, the floor / wall, restitution, friction)
	bool bounce = false;
	float bounciness = 0.5f, friction = 0.3f;
	uint32_t shatterGen = 0; // a new shatter = a new flight
	float contour = 0, contourSpacing = 0.06f, contourWidth = 0.12f, contourScroll = 0;
	int contourMode = 1;
	float contourColor[ 3 ] = { 1, 0.6f, 0.2f };
};

struct ShardJob
{
	uint64_t key = 0;
	std::atomic< bool > done{ false }, cancel{ false };
	std::vector< uint16_t > triShard;
	std::vector< float > shardData;
	std::vector< float > radius; // W26 Bounce: per shard, fitted units
	double ms = 0;
};
struct DustJob
{
	uint64_t key = 0;
	std::atomic< bool > done{ false }, cancel{ false };
	std::vector< float > points; // raw xyz + uv
	std::vector< float > refs;   // MoveEhr W1: per point its triangle (uint bits) + barycentric b, c (the posed origin follows the pose)
	std::vector< std::array< uint32_t, 2 > > partRanges;
	float keyQ[ 17 ] = {}; // W16: the dust noise at quantiles 0, 1/16 .. 1 over these points
	double ms = 0;
};

// A clone or ghost of the model: full model matrix (column-major), hue offset (turns), alpha, LOD level.
// keep < 1 dissolves it (tunnel ring fade), fogKeep < 1 hides it in Depth Fog (the tunnel's far end with fog on),
// age = a ghost's place along the trail (0 = at the mask, 1 = its end; drives Trail Dust).
struct Instance
{
	float m[ 16 ];
	float hue = 0, alpha = 1;
	int lod   = 0;
	float keep = 1, fogKeep = 1, age = 0;
	float next[ 16 ] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // ghosts: the next newer pose (Trail Smear)
};

// Stage 5 trails (W15): Trail Echoes = a screen-space feedback buffer of the rendered mask (stamped every Delay
// frames, or every frame with sub-steps as Smear rises); every pixel carries its age. Ghosts share the look knobs.
struct TrailState
{
	int echoes = 0, delay = 4;
	float decay = 0.7f, offset[ 2 ] = { 0, 0 }; // offset: uv per copy (Delay frames)
	float smear = 0, blur = 0, dust = 0, grainPx = 2.5f; // grain in pixels at this height
	float drift[ 2 ] = { 0, 0 };      // dust drift this frame, uv
	float grainPhase[ 2 ] = { 0, 0 }; // drifting grain pattern, cells (wrapped at 256: the grain hash repeats there)
	int curve = 0;                    // 0 linear, 1 soft, 2 long tail
	float glow = 0, colorMix = 0, color[ 3 ] = { 1, 1, 1 };
	// ghosts: dust grain size in fitted object units, and how far a fully dusty ghost has drifted (world)
	float ghostGrain = 0.02f, ghostDrift[ 3 ] = { 0, 0, 0 }, ghostSmear = 0;
};

// W26 Stage 2: the projector. A spot whose cookie is a picture (the FX input, or an image file decoded off the render thread
// with its mips): its texture holds the picture (rgb) + the shadow distance (a) in the projector's own frustum.
struct ProjImage
{
	int w = 0, h = 0;
	std::vector< std::vector< uint8_t > > mips; // RGBA, rows bottom-up (GL), level 0 first
	uint64_t serial = 0;
};
struct ProjState
{
	bool on = false;
	float pos[ 3 ] = {}, dir[ 3 ] = { 0, 0, -1 };
	float fovX = 30;           // throw, degrees across the picture
	float color[ 3 ] = {};     // colour x intensity
	float beam = 1, surface = 1, focus = 0;
	int source = 0;            // 0 the FX input, 1 the image / video file
	std::shared_ptr< const ProjImage > image;
	std::shared_ptr< const VideoFrame > video; // W2: the video file's frame for this frame (instead of image)
	bool videoStraight = false;                // W3 Video Alpha: straight alpha = premultiply the frame in the copy pass
	bool videoKeep     = false;                // W8: the video file was refused: the last picture stays
};

// W6 Stage 2: the transition pack's per-side state. The MORPH program variants (`#define MORPH`, Morph.cpp) carry its vertex side
// (vs: 1 shrinkwrap, 2 pieces, 3 implode, 4 glitch, 5 ripple, 6 spin swap) and its fragment side (mask: 1 scan, 2 glitch, 3 ripple,
// 4 burn); the point cloud (styles 5 Points / 17 Swarm) is its own draw. side 1 = the hop's outgoing model, 2 = the incoming one.
struct MorphState
{
	int vs = 0, mask = 0, side = 0;
	float e = 0;       // the hop's progress, eased by Trans Ease
	float k = 0;       // the style's own progress / amount on this side (see each style)
	float stagger = 0, spread = 0.6f;
	float scale = 1;   // spin swap: this side's size
	float step = 0;    // glitch: the stepped clock (a whole step per 1/30 of the hop: slider-driven)
	float freq = 12;   // ripple frequency / glitch bands
	float band = 0.06f; // scan: the wire band's half width (0..1 of the axis)
	float color[ 3 ] = { 0, 1, 1 }, glow = 0; // scan / ripple edge: Wire Color, Wire Glow
	float temp = 2000, flame = 2;             // burn: Flame Temp, Flame Glow
	float lavaGlow = 3;                       // lava: Lava Glow (the style glows whatever Lava Cracks says)
	float seed[ 3 ] = { 0, 0.4f, 0.9f };       // burn: where the fire starts (fitted)
	float lead = 0;                           // W7 twist: which end winds first (> 0 the top) and how far ahead
	float sparkle = 0, sparklePhase = 0;      // W7 Trans Sparkle (0..10) and its twinkle clock (twinkles, wraps at 1000)
	// the point cloud (styles 5 / 17): drawn while cloud > 0 (its fade), flight progress cloudK
	float cloud = 0, cloudK = 0;
	bool swarm = false;
	float cloudColor[ 3 ] = { 0, 1, 1 }, cloudColorMix = 0, cloudPx = 2, cloudGlow = 0, cloudTwinkle = 0, cloudPhase = 0;
	bool On() const { return vs > 0 || mask > 0; }
};

// W26 Stage 4: the Liquid transition style (4). ready: both models' SDFs are on the GPU (otherwise it dissolves like style 0)
struct LiquidState
{
	bool ready = false;
	float goo = 0.6f, wobble = 0.4f, phase = 0, tint[ 3 ] = { 1, 1, 1 }, tintMix = 0;
	int look = 0; // 0 chrome, 1 paint
};

// W26 Stage 5: lightning. segs: 8 floats per segment (world a xyz + brightness, world b xyz + width factor)
struct BoltState
{
	std::vector< float > segs;
	float color[ 3 ] = { 0.6f, 0.7f, 1 }, width = 1.5f, glow = 3, tol = 0.05f;
};

// W26 Stage 6: the firefly swarm (count 0 = off)
struct SwarmState
{
	int count = 0;
	bool reset = false;
	float attract = 3, flock = 1, wander = 1, offset = 0, kick = 0, drift = 0, size = 2, twinkle = 0.5f, time = 0, unit = 1, dt = 0;
	float color[ 3 ] = { 1, 0.8f, 0.3f };
	bool maskVisible = true;
};

// W26 Stage 7: lava cracks (fitted object space). on = the glow pass draws; split > 0 = the CRACKS soup variants move the cells
struct LavaState
{
	bool on = false;
	float scale = 2.5f, seed[ 3 ] = { 0, 0, 1 }, front = 0, split = 0;
	float glow = 3, temp = 1600, width = 0.3f, tint[ 3 ] = { 1, 0.4f, 0.1f }, tintMix = 0, flow = 0, haze = 0, hazePhase = 0;
};

// W26 Stage 8: glass caustics. on: the caustic light (along the key direction) runs the photon passes
struct CausticState
{
	bool on = false, wholeMask = false;
	float pos[ 3 ] = {}, dir[ 3 ] = { 0, -1, 0 }, fov = 40, color[ 3 ] = { 1, 1, 1 };
	float ior = 1.5f, rainbow = 0.5f, sharp = 0.6f, ripple = 0.3f, phase = 0;
	float floor = 1, smoke = 1, surface = 1, gain = 1; // gain: the cookie passes' 1 / (1 + d^2) falloff undone
};

// W9 Stage A: the line looks (Lines group). Points are captured through the soup stage every frame (CAPTURE twins), then drawn as
// pairs (plexus), ribbons through their past positions (light paint) or edge tubes (neon); light in the main pass (rgb added, alpha
// kept) and in the W5 HDR wire buffer for glow / bloom.
struct LinesState
{
	int look = 0;            // 0 off, 1 plexus, 2 light paint, 3 neon tubes
	int points = 0;          // plexus points / light paint heads
	int source = 3;          // plexus: the Point Source
	float reach = 0.15f;     // plexus: neighbour distance (fitted units); paint: trail seconds; neon: the light's reach 0..1
	float amount = 0.35f;    // Line Amount (neon: the tubes' strength)
	float speed = 0.5f, audio = 0;
	float flow = 0;          // pulses along the lines, cycles (wrapped at 1000)
	float swirl = 0;         // light paint heads' circle, turns (wrapped at 1000)
	float time = 0;          // show seconds wrapped at 3600 (the light paint ring's stamps)
	float flicker = 0;       // neon flicker steps (wrapped at 1000)
	float swirlR = 0;        // light paint: the heads' circle radius, fitted units
	float bands[ 16 ] = {};  // FFT, bass first (smoothed, x Audio Gain)
	int axis = 1;            // the rest height axis for the bands (Deform Axis)
	float width = 1;         // Wire Thickness, px
	float color[ 3 ] = { 0, 1, 1 }, colorMix = 0, glow = 0, bloom = 0;
};

// W9 Stage B (Build group) + Stage C (Cut group): the looks drawn by the DEFORM twins (vertex + fragment side), the cut's cap, the lens
struct XState
{
	int look = 0;            // Build Look: 1 slit-scan, 2 print on, 3 exploded, 4 black hole
	int axis = 1;            // Build Axis
	float amt = 0;           // the Build look's progress / strength
	float swirl = 1.5f;      // black hole turns at the full fall
	float slitPhase = -1;    // slit-scan waves (Build Speed), cycles; < 0 = none
	std::vector< float > hist; // slit-scan: newest first, one entry per 1/60 s (8 texels: pose columns, phases, jelly x 2, -)
	float poseInv[ 16 ] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // slit-scan: the pose now, inverted (world)
	int cut = 0;             // 1 = a plane cut (print on; Stage C: cross-section)
	float cutPos = 0.5f;     // where the plane sits along the axis over the model's extent (0..1); kept below it
	float color[ 3 ] = { 0, 1, 1 }, glow = 1.5f; // Wire Color, the band's light
	bool layers = false;     // print on: printed layer ridges
	bool cap = true;         // the cut face is capped (stencil)
	int capLook = 0;         // 0 print on (hot fresh top), 1 cross-section (scan)
	float capPhase = 0;      // the cap's scan lines, cycles (wrapped at 1)
	bool lens = false;       // black hole: the screen-space lens + event horizon
	float lensAmt = 0;
	// W9 Stage C (Cut group, every bundle): region 0 the plane (Print On / Cross-Section: + slab, 0..1 of the extent under it, 0 = none),
	// 1 the reveal box; window = the openings show another world
	int region = 0;
	float slab = 0;
	float box[ 4 ] = { 0, 0, 0, 0.5f }, round = 0; // reveal box: centre (fitted), half size, corner share
	bool removedWire = false; // the removed part as wire even with Wire Mode Off (the reveal box's outside)
	// MoveEhr W5 (S6) Reveal Sphere: the reveal box at corner share 1 (a sphere), placed per model by the renderer: centre on the model's
	// height at spherePos (0 its bottom .. 1 its top, fitted box), radius sphereR x the fitted box's diagonal
	bool sphere = false;
	float spherePos = 0.5f, sphereR = 0.0f;
	bool window = false;
	float eyes[ 2 ][ 4 ] = {}; // window: the eye openings, world centre + radius (radius 0 = none)
	float front[ 3 ] = { 0, 0, 1 }; // the mask's front (world)
	GpuModel* winModel = nullptr;   // the other world: a model slot (drawn with winM) ...
	float winM[ 16 ] = {}, winScale = 1;
};

// W9 Stage D (Surface Look group, classic): spikes and copies on captured surface spots (the dominant model), camera looks (DEFORM twins)
struct SurfLookState
{
	int look = 0;          // 1 audio spikes, 2 skin of copies, 3 x-ray, 4 thermal, 5 night vision, 6 led matrix
	float amount = 0.5f, scale = 0.35f, speed = 0.5f;
	float phase = 0;       // the look's clock (spikes' dance / copies' wobble: cycles wrapped at 1000; camera looks: their own)
	int spots = 0;         // spikes / copies
	float bands[ 16 ] = {};
	float spikeLen = 0.3f, spikeRad = 0.02f; // fitted units
	float copySize = 0.06f;                   // fitted units of the main model per fitted unit of the copy
	GpuModel* copy = nullptr;                 // skin of copies: the source model (Surf Source)
	float color[ 3 ] = { 0, 1, 1 }, colorMix = 0;
};

struct FrameState
{
	// target
	int width = 1, height = 1;
	GLuint hostFBO = 0;
	// matrices (column-major)
	float model[ 16 ];
	float view[ 16 ];
	float proj[ 16 ];
	float camPos[ 3 ];
	float modelScale = 1; // raw -> world along the model matrix
	float modelB[ 16 ];   // the hop's incoming model: the same pose, fitted by its own bbox
	float modelScaleB = 1;
	float transition  = 0; // the hop's progress: 0 = the outgoing model (a), 1 = the incoming one (b)
	int transitionStyle = 0; // 0 dissolve, 1 shatter, 2 dust, 3 slice
	float transEase = 1, transStagger = 0.85f, transSpread = 0.6f; // W16: Shatter / Dust timing (W6: and the pack's)
	MorphState morph; // W6 Stage 2 (the pack's dials are filled by the plugin; the per-side state by TransitionOut / In)
	// material
	float tint[ 3 ]   = { 1, 1, 1 };
	float tintMix     = 0;
	float hue         = 0;
	float saturation  = 1;
	float brightness  = 1;
	bool metalOverride = false;
	float metalValue  = 0;
	float roughScale  = 1;
	float normalStrength = 1;
	bool normalFlipY  = false;
	float heightAmt   = 0;
	float emissiveGlow = 1;
	float rimGlow     = 0;
	float rimColor[ 3 ] = { 1, 0.85f, 0.6f };
	PartControls parts[ kMaxParts ];
	// lighting
	float keyDir[ 3 ]    = { 0, 0, 1 };
	float keyColor[ 3 ]  = { 3, 3, 3 };
	float fillDir[ 3 ]   = { 0, 0, 1 };
	float fillColor[ 3 ] = { 0, 0, 0 };
	float ambient        = 0.03f;
	float envIntensity   = 1;
	int envSource        = 0; // 0 studio, 1 input, 2 off; MoveEhr W5: 3 HDRI file (only while the renderer holds one: EnvFileReady)
	float envBackdrop    = 0; // MoveEhr W5: the HDRI shown behind the model (0..1), HDRI file only
	float envRotate      = 0;
	float exposure       = 0;
	int tonemap          = 0;
	// fx
	float wireMix = 0, wireThickness = 1, explode = 0, explodeRandom = 0, explodeSeed = 0; // wireMix: the W1 wire (plain programs) only
	// W5 wireframe through the WIRE program twins (on = anything past the W1 look). mode 1 over surface, 2 wire only, 3 + dark fill;
	// W6: 4 points only, 5 points + wire (the POINTS twins)
	struct WireState
	{
		bool on = false;
		int mode = 1, back = 0, edges = 0; // back 0 hidden, 1 x-ray; edges 0 all triangles, 1 feature edges
		float mix = 0, color[ 3 ] = { 0, 1, 1 }, colorMix = 0, glow = 0, bloom = 0, bloomSize = 0.5f, fade = 0;
		float fadeNear = 0, fadeFar = 1; // camera distances the fade runs across (the model's near / far side)
		int pointSource = 3;              // W6: 0 vertices, 1 feature vertices, 2..4 surface 25k / 100k / 400k
		float twinkle = 0, twinklePhase = 0; // W6 Point Twinkle: depth; the clock in twinkles (wraps at 1000)
		// MoveEhr W4: Point Shape / Motion / Size Var (ptfx = any of them off its rest: the PTFX twins draw; else W6's POINTS twins)
		bool ptfx = false;
		int shape = 0, motion = 0;
		float str = 0, dis = 0, phase = 0, burst = 1, sizeVar = 0;
		int shading = 0; // MoveEhr W7 Point Shading: 0 Full (every sprite pixel shaded), 1 Per Point (the PTSHADE twins; W7b: Unlit retired)
		const std::vector< uint8_t >* glyphs = nullptr; // Glyph: the 512 x 512 R8 atlas (8 x 8 cells) while Point Shape = Glyph
		uint64_t glyphKey = 0;
		int glyphN = 0;
	} wire;
	float dissolve = 0, dissolveEdge = 0, toonSteps = 0, outline = 0, fogDensity = 0, fogStart = 0;
	// output
	bool dissolveInvert = false; // transition: the incoming model shows where the outgoing one dissolved
	float background[ 4 ] = { 0, 0, 0, 0 }; // straight sRGB colour + alpha
	int msaa              = 4;
	// input (FX)
	GLuint inputTex = 0;
	int inputW = 0, inputH = 0, inputHwW = 0, inputHwH = 0;
	int inputUse        = 0; // 0 off, 1 background, 2 skin flagged, 3 skin all
	int inputProjection = 0;
	bool faceWant       = false; // FX face tracking: read the input back this frame if none is in flight
	float inputMix      = 1;
	int debugView       = 0;
	// extra lights + volume
	XLight xl[ kMaxXLights ];
	int xlCount = 0;
	VolumeState vol;
	SimState sim;
	TripState trip;
	ProjState projector;
	LiquidState liquid;
	BoltState bolts;
	SwarmState swarm;
	LavaState lava;
	CausticState caustic;
	PostState post;
	SurfaceState surf;
	ShapeState shape;
	// MoveEhr W2 Part B: true voxels (Voxel.cpp; docs/PLAN.md section 7). look 0 = the mesh as before
	struct VoxelState
	{
		int look = 0;          // 0 Off 1 Cubes 2 Smooth 3 Rounded 4 Spheres 5 Frames 6 Bricks
		int res = 48;          // cells across the grid (2.5 fitted units)
		float gap = 0.1f, bevel = 0;
		int motion = 0;
		float amount = 0.5f;   // strength (periodic motions)
		float dis = 0;         // how far apart (transient motions)
		float phase = 0;       // turns, wrapped at 1000
		float beat = 0;        // host beat phase 0..1
		float beatN = 0;       // beats mod 1024
		float reveal = 1;
		int revealStyle = 0;
		int partLvl[ 5 ] = {}; // W3: per body part (Head, Torso, Arms, Hands, Legs) the block size as log2 (0 = x1 .. 3 = x8)
	} vox;
	std::vector< Instance > inst;   // opaque copies (clones, and the original when kept); empty = one plain draw
	std::vector< Instance > instB;  // LoadEhr W1: the same copies for the hop's incoming model (its own fit)
	std::vector< Instance > ghosts; // translucent past poses, drawn before the opaque copies
	TrailState trail;
	// W21 text / logos: per-letter transforms, 12 floats per letter (pivot + scale, quaternion, offset + unused);
	// empty = the letters are at rest (the plain programs draw)
	std::vector< float > letterXf;
	LinesState lines; // W9 Stage A
	XState x;         // W9 Stage B / C
	SurfLookState sl; // W9 Stage D
};

// W6 Stage 2: a slot's point cloud for the Points / Swarm styles, built on a worker (Morph.cpp)
struct CloudSlotJob
{
	std::atomic< bool > done{ false }, cancel{ false };
	std::vector< float > pts; // 9 per point: raw xyz, normal xyz, uv, part (sorted by the order key)
	std::vector< float > refs; // MoveEhr W1: 3 per point (same order): triangle (uint bits), barycentric b, c
	double ms = 0;
};

// W6 Stage 1: a model's points for one Point Source (Points.cpp): refs on unit 26, a run per GPU part
// MoveEhr W2 Part B: a voxel spot set being built (Voxel.cpp)
struct VoxSpotJob
{
	std::atomic< bool > done{ false }, cancel{ false };
	std::vector< float > refs; // 4 per spot: triangle, b1, b2, body part (W3: 0 Head 1 Torso 2 Arms 3 Hands 4 Legs)
	std::vector< std::array< uint32_t, 2 > > ranges;
	double ms = 0;
	std::string parts; // W3: how the parts were told apart + their shares (log)
};
struct PointSet
{
	struct Part
	{
		uint32_t first, count;
		bool fractured;
	};
	struct Job
	{
		std::atomic< bool > done{ false }, cancel{ false };
		std::vector< float > refs; // 4 per point: global triangle, b1, b2, 0
		std::vector< std::array< uint32_t, 2 > > ranges;
		double ms = 0;
	};
	uint64_t serial = 0;
	int source      = -1;
	std::shared_ptr< Job > job;
	GLuint buf = 0, tex = 0;
	std::vector< std::array< uint32_t, 2 > > ranges; // per GPU part: first point, count
	size_t count = 0;
	bool ready   = false;
};

GLuint Link( const std::string& vs, const std::string& fs, std::string& error );
void SampleSurface( const CpuGeometry& g, int count, DustJob& job ); // Shape.cpp (W26 swarm homes)
void BindShadowSamplers( GLuint prog ); // program must be bound
void UnbindShadowUnits();
void SetMediumUniforms( GLuint prog, const FrameState& fs ); // Volume.cpp: the shared medium (march, screen rays)
// texture units of the volume lights' shadow maps (2D for every volume light, cube for lights 0/1)
static constexpr int kShadow2DUnit[ kMaxVolLights ] = { 11, 12, 17, 18 };
static constexpr int kShadowCubeUnit[ 2 ]           = { 13, 14 };

class Renderer
{
public:
	bool Init( std::string& error ); // compiles shaders; logs the compile log on failure (+ W2: the DEFORM twins)
	void Release();
	// a = the hop's outgoing model (slot slotA), b = the incoming one (slot slotB); fs.transition = the hop's progress
	void Render( const FrameState& fs, GpuModel* a, GpuModel* b, int slotA = 0, int slotB = 1 );
	// LoadEhr W1: build what a hop will need for a filled slot that is not drawn this frame (uses: SlotUse bits), so a morph
	// never waits on it. Off the render thread except the uploads.
	void PrepareSlot( int slot, GpuModel* model, const FrameState& fs, unsigned uses );
	// MoveEhr W1: model animation. The plugin hands every animated slot's pose each frame (palette: 24 floats per joint = J and
	// D rows, see Anim.h; morph weights); Render() runs one transform-feedback pose pass per pair side whose slot has a pose and
	// draws that side from its posed buffer (the static buffer otherwise: Anim Play Off = no pass at all).
	void SetSlotPose( int slot, GpuModel* model, const std::vector< float >& palette, const std::vector< float >& weights );
	void ClearSlotPose( int slot );
	void SetSlotTrail( int slot, const std::vector< float >& palette, const std::vector< float >& weights ); // W4 Trail Off: the pose 0.35 s earlier
	void ClearSlotTrail( int slot );
	bool ReadPosed( int slot, std::vector< float >& xyz ); // debug (MOVEEHR_POSE_CHECK): the posed positions, a blocking readback
	// debug (MOVEEHR_POSE_CHECK): the slot's posed dust / cloud points vs the barycentric points of its posed vertices (CPU), max
	// |diff| in raw units (-1 = nothing posed to check)
	void CheckPosedPoints( int slot, const GpuModel* model, double& dust, double& cloud );
	void CheckShardPivots( int slot, const GpuModel* model, double stats[ 6 ] ); // W2 A3 debug: pivot vs CPU posed centroids; bind vs posed
	int ShardCount( int slot ) const;
	double poseMs = 0;                                     // CPU ms of this frame's pose passes (the PERF line)
	int posePasses = 0;
	size_t SlotBytes( int slot ) const; // GPU bytes of that slot's per-slot caches
	// Face tracking (FX): true once per completed readback; the pixels are moved out (RGBA, rows top-first).
	bool TakeFaceFrame( std::vector< uint8_t >& px, int& w, int& h );
	PerfProbe perf; // W29: the PERF line's per-pass timing (Debug Log on only)
	// MoveEhr W5 (S5): the HDRI file environment (EnvFile.cpp). SetEnvImage uploads it now (GL thread; null = drop it); while one is
	// held, Env Source 3 draws through the ENVFILE twins and Env Backdrop shows it behind the model
	void SetEnvImage( std::shared_ptr< const EnvImage > img );
	bool EnvFileReady() const { return envFileTex != 0; }

private:
	bool InitPrograms( std::string& error );
	// W2 Deform: every triangle-soup program has a DEFORM twin (linked by Init), drawn whenever a deform is on
	std::vector< std::pair< GLuint, GLuint > > deformTwins;
	GLuint DeformTwin( GLuint p ) const;
	// W5 wireframe (Wire.cpp): WIRE twins of the main-pass soup programs, per-model edge flags, the HDR wire glow buffer
	std::vector< std::pair< GLuint, GLuint > > wireTwins;
	GLuint WireTwin( GLuint p ) const;
	struct WireEdges
	{
		uint64_t serial = 0;
		std::shared_ptr< std::vector< uint8_t > > job; // filled by the worker (flags per triangle, GPU index order)
		std::shared_ptr< std::atomic< bool > > done;
		GLuint buf = 0, tex = 0;
		bool ready = false;
	};
	WireEdges wireEdges[ 4 ];
	GLuint wireDummyEdge = 0, wireDummyEdgeBuf = 0, wireTex = 0, wireFBO = 0, wireDepth = 0, wireBlur[ 2 ] = {}, wireBlurFBO[ 2 ] = {};
	GLuint progWireAdd = 0;
	int wireW = 0, wireH = 0;
	bool wireGlowOk = false;
	bool InitWire( std::string& error );
	void ReleaseWire();
	GLuint WireEdgeTex( GpuModel* model, bool want ); // the model's edge flags (R8UI buffer) or the all-edges dummy
	void DrawWired( GLuint prog, const FrameState& fs, GpuModel* model, const std::vector< Instance >* inst ); // the main pass, wired
	void SetWireUniforms( GLuint prog, const FrameState& fs, GpuModel* model ); // + binds the edge flags (unit 25)
	int wirePassNow = 3; // the WIRE pass a wired DrawModel draws (DrawWired sets it per draw)
	// W6 points (Points.cpp): the POINTS twins, the point sets, and whether a wired DrawModel draws points (DrawWired sets it)
	std::vector< std::pair< GLuint, GLuint > > pointTwins;
	GLuint PointTwin( GLuint p ) const;
	// MoveEhr W4: the PTFX twin of a POINTS twin (or its DEFORM twin), linked on demand from the recorded sources; itself on failure
	std::unordered_map< GLuint, GLuint > ptfxTwins;
	GLuint PtfxTwin( GLuint p );
	// MoveEhr W7: the PTSHADE twin (Point Shading Per Point) of whatever points program a draw has, linked on demand;
	// itself on failure. Its draws go twice: each point shaded once into its texel of ptShadeTex, then the sprites read it
	std::unordered_map< GLuint, GLuint > shadeTwins;
	GLuint ShadeTwin( GLuint p );
	static constexpr int kPtShadeW = 2048, kPtShadeMaxH = 4096, kPtShadeUnit = 66; // RGBA32F, rows grown to need (8M points x instances)
	GLuint ptShadeTex = 0, ptShadeFBO = 0, ptShadeDummy = 0;
	int ptShadeH = 0;
	bool ptShadeFull = false; // logged once: a draw past the buffer's size fell back to Full
	bool PtShadeFit( GLsizei n );
	void ReleasePtShade();
	// MoveEhr W5: the ENVFILE twin of a main-pass program (Env Source HDRI File), linked on demand like PtfxTwin; itself on failure
	std::unordered_map< GLuint, GLuint > envTwins;
	GLuint EnvTwin( GLuint p );
	std::shared_ptr< const EnvImage > envImg;
	GLuint envFileTex = 0, progBackdrop = 0;
	int envFileLevels = 0;
	void BindEnvFile( const FrameState& fs );
	void DrawBackdrop( const FrameState& fs, int w, int h );
	void ReleaseEnvFile();
	void SetPointLookUniforms( GLuint p, const FrameState& fs, GpuModel* model, bool posed );
	GLuint glyphTex = 0, glyphDummy = 0, trailDummy = 0, trailDummyBuf = 0;
	uint64_t glyphKeyNow = 0;
	static constexpr int kPointSets = 8;
	PointSet pointSets[ kPointSets ];
	unsigned pointNext = 0;
	GLuint pointDummy = 0, pointDummyBuf = 0; // one point (the prewarm draws through it before a set exists)
	bool pointsNow = false, warmingNow = false;
	float pointShareNow = 1.0f;  // W7: the share of each part's points a POINTS draw takes (the sparkles: a few thousand of 25k)
	bool sparkleNow     = false; // W7: this frame's wire buffer holds sparkles
	FrameState sparkFs[ 2 ];     // W7: the pair's sparkle draws (Points Only, glow only), kept for RenderWireGlow
	bool InitPoints( std::string& error );
	// W9 Stage A (Lines.cpp): the CAPTURE twins, the line sets (spots + pairs per model), the capture buffers, the passes
	std::vector< std::pair< GLuint, GLuint > > captureTwins;
	GLuint CaptureTwin( GLuint p ) const;
	bool captureNow = false; // DrawModel: this draw captures the spots of captureRefTex / captureRanges (transform feedback)
	GLuint captureRefTex = 0;
	const std::vector< std::array< uint32_t, 2 > >* captureRanges = nullptr;
	float capTime = 0, capSwirl = 0, capPhase = 0;
	int capAxis = 1;
	bool InitLines( std::string& error );
	void ReleaseLines();
	// MoveEhr W2 Part B: true voxels (Voxel.cpp). Per model slot: the surface spots (worker), their capture, the occupancy atlas, the
	// solid + owners, the HistoPyramid (two atlases), the indirect commands and the voxel list; built per drawn side when stale
	struct VoxSlot
	{
		uint64_t spotKey = 0, wantKey = 0;
		std::shared_ptr< VoxSpotJob > job;
		GLuint refBuf = 0, refTex = 0;
		std::vector< std::array< uint32_t, 2 > > ranges;
		size_t spots = 0;
		GLuint capBuf[ 3 ] = {}, capTex[ 3 ] = {};
		size_t capSize = 0;
		int res = 0, tiles = 0, size = 0, top = 0;
		GLuint ownerTex = 0, depthRb = 0, splatFbo = 0, extTex[ 2 ] = {}, extFbo = 0, countTex = 0, ownTex = 0, baseFbo = 0;
		GLuint fieldTex[ 2 ] = {}, fieldFbo[ 2 ] = {}, hpTex[ 2 ] = {}, hpFbo[ 2 ] = {}, countFbo = 0;
		GLuint listBuf = 0, listTex = 0, cmdBuf = 0;
		size_t listCap = 0;
		// W3 part sizes: per block level (x2, x4, x8) a (res / size)^3 atlas of (solid cells, cells voting this size or more, colour owner),
		// and the merged count / owner atlases the later passes read instead of countTex / ownTex (merged = in use this build)
		GLuint blkTex[ 3 ] = {}, blkFbo[ 3 ] = {}, mCountTex = 0, mOwnTex = 0, mergeFbo = 0;
		int blkN[ 3 ] = {}, blkT[ 3 ] = {};
		bool merged = false;
		uint64_t builtKey = 0;
		bool built = false, smooth = false;
		bool stateLogged = false; // Debug Log: the host's GL state at the first build
		int countLogs     = 0;     // Debug Log: the first voxel counts
	} vox[ kSlots ];
	GLuint progVoxSplat = 0, progVoxExtent = 0, progVoxBase = 0, progVoxBlur = 0, progVoxCount = 0, progVoxReduce = 0, progVoxCmd = 0, progVoxTrav = 0;
	GLuint progVoxDraw = 0, progVoxSmooth = 0, progVoxPlain = 0, progVoxBlock = 0, progVoxMerge = 0, progVoxDrawB = 0, progVoxPlainB = 0, voxVao = 0;
	// W4 A1: the SHADOW twins (a caster's value instead of the shade; BLOCKS reads an x1 list as level 0, so one twin serves both lists)
	GLuint progVoxDrawS = 0, progVoxPlainS = 0, progVoxSmoothS = 0;
	GLuint progVoxBaseC = 0; // W4 A2: the base pass with the concave carve (MOVEEHR_NO_CARVE=1: W3's fill)
	bool voxInitTried = false;
	// W4 A1: what the main pass draws per side this frame (voxels, or the mesh until a side's spots land) + the hop, for the casters / mirror
	bool voxDrawn[ 2 ]  = { false, false };
	float voxHopT       = 0.0f;
	int casterOrtho     = 0;  // the caster pass in flight: the key shadow (depth along casterDir) or a light's (distance)
	float casterDir[ 3 ] = { 0, -1, 0 };
	bool InitVoxels();
	void ReleaseVoxels();
	bool VoxSpots( int slot, GpuModel* model, int res ); // the spot set for (model, res) ready (else building)
	bool VoxTargets( VoxSlot& v, int res );
	// this side's voxel list from the frame's pose / state (cached while nothing it reads changed); false = nothing to draw yet
	bool BuildVoxels( const FrameState& fs, GpuModel* model, GLuint soupProg, bool everyFrame );
	// W4 A1: shadowLightPos = a caster pass (vp = the light's view-projection)
	void DrawVoxels( const FrameState& fs, GpuModel* model, int hopSide, float hopE, const float* vp = nullptr, const float* shadowLightPos = nullptr );
public:
	struct LineSet
	{
		struct Job
		{
			std::atomic< bool > done{ false }, cancel{ false };
			std::vector< float > refs;                       // 4 per spot: triangle, b1, b2, 0
			std::vector< std::array< uint32_t, 2 > > ranges; // per GPU part: first spot, count
			std::vector< uint32_t > pairs;                   // plexus: i, j per line (neon: none, spots 2k / 2k + 1 are an edge)
			double ms = 0;
		};
		uint64_t key = 0, serial = 0;
		int look = 0;
		std::shared_ptr< Job > job;
		GLuint refBuf = 0, refTex = 0, pairBuf = 0, pairTex = 0;
		std::vector< std::array< uint32_t, 2 > > ranges;
		size_t count = 0, lines = 0;
		bool ready = false;
		uint64_t used = 0; // the frame it was last asked for
	};
private:
	static constexpr int kLineSets = 6;
	LineSet lineSets[ kLineSets ];
	const LineSet* LinesFor( GpuModel* model, const LinesState& L );
	struct Capture // per side of the pair: this frame's spots (world position + time, normal + rest height, colour + visible)
	{
		GLuint buf[ 3 ] = {}, tex[ 3 ] = {};
		size_t cap = 0;
		// light paint: the ring of past positions (rows x heads), stamped with their time
		GLuint ringBuf = 0, ringTex = 0;
		size_t ringHeads = 0;
		int ringHead = 0, ringFilled = 0;
		int64_t lastTick = -1;
		uint64_t ringFor = 0;
	} capture[ 3 ]; // W9: [ 2 ] = the Surface Look's spots
	static constexpr int kRingRows = 240; // 4 s at 60 samples / s
	GLuint progLinePair = 0, progLineRibbon = 0, progLineDot = 0, progLineLight = 0, lineBlur[ 2 ] = {}, lineBlurFBO[ 2 ] = {};
	int lineBlurW = 0, lineBlurH = 0;
	struct LineDraw
	{
		const FrameState* fs;
		const LineSet* set;
		int side;
		float weight;
		float worldPerFitted;
	};
	std::vector< LineDraw > lineDraws; // this frame's line draws (the glow pass draws them again)
	bool CaptureSpots( const FrameState& fs, GpuModel* model, GLuint prog, const LineSet& set, int side );
	void RenderLines( const FrameState& fsA, GpuModel* a, GLuint progA, const FrameState* fsB, GpuModel* b, GLuint progB, float t );
	void DrawLines( const LineDraw& d, bool glow ); // the bound target: main (light, alpha kept) or the HDR wire buffer
	void AddLineLight( const FrameState& fs, int w, int h ); // neon: the tubes' wide light on the model
	// W9 Stage B (Build.cpp): the looks in the DEFORM twins, exploded groups, the slit-scan history, the cut + cap, the black hole's lens
	bool InitBuild( std::string& error );
	void ReleaseBuild();
	void SetXUniforms( GLuint prog, const FrameState& fs, GpuModel* model ); // DEFORM twins (binds units 31 / 32)
	GLuint XGroupTex( GpuModel* model, bool want );
	void XPlanes( const FrameState& fs, GpuModel* model, float* world, float* fitted, float& worldPerFitted, int plane = 0 ) const;
	void DrawSide( GLuint prog, const FrameState& fs, GpuModel* model, const std::vector< Instance >* inst ); // the main draw of one model
	void DrawCap( GLuint prog, const FrameState& fs, GpuModel* model, int plane = 0 ); // plane 1: the cross-section slab's lower plane
	void DrawWindow( const FrameState& fs, GpuModel* model ); // the openings (stencil) and the other world behind them
	GLuint progWinMark = 0, progWinQuad = 0;
	bool LensOn( const FrameState& fs ) const { return fs.x.lens && fs.x.lensAmt > 0.0f; }
	void SetLensUniforms( GLuint prog, const FrameState& fs, int w, int h );
	void WarmLooks( const FrameState& fs, GpuModel* model ); // at model swap, through the 1-pixel scissor
	// W9 Stage D (Surface.cpp)
	bool InitSurfLook( std::string& error );
	void ReleaseSurfLook();
	void RenderSurfLook( const FrameState& fs, GpuModel* model, GLuint prog ); // spikes / copies on the model's surface (main pass)
	GLuint progSpike = 0, progMainIT = 0;
	bool skinNow = false; // DrawModel: the copies (progMain -> progMainIT, one instance per captured spot)
	int skinCount = 0, skinLod = 0;
	struct XGroups
	{
		uint64_t serial = 0;
		std::shared_ptr< std::vector< float > > job;
		std::shared_ptr< std::atomic< bool > > done;
		GLuint buf = 0, tex = 0;
		bool ready = false;
	} xGroups[ 4 ];
	GLuint xDummy = 0, xDummyBuf = 0, xHistBuf = 0, xHistTex = 0, progCap = 0, progPostLens = 0;
	size_t xHistCap = 0;
	int xCutNow = 0;               // DrawSide: 1 = this draw keeps the kept side of the cut, 2 = the removed side
	GpuModel* lensModel = nullptr; // the model the hole sits in (the dominant one)
	const FrameState* lensFs = nullptr;
	void ReleasePoints();
	const PointSet* PointsFor( GpuModel* model, int source );
	void RenderWireGlow( const FrameState& fs, int w, int h ); // the wired draws of this frame, again, into the HDR wire buffer
	struct WireDraw
	{
		GLuint prog;
		const FrameState* fs;
		GpuModel* model;
		const std::vector< Instance >* inst;
		int side;
		float share = 1.0f; // W7: the share of the points the glow pass draws
	};
	std::vector< WireDraw > wireDraws; // this frame's wired draws that glow (DrawWired adds them)
	float wireMixNow = -1.0f;          // a wired draw's Wireframe Mix override (-1 = the dial)
	void AddWireLight( const FrameState& fs, int w, int h ); // after the composite: glow + halo, light at alpha 0
	bool EnsureTargets( int w, int h, int samples );
	void ReleaseTargets();
	void EnsureVAO( GpuModel* model );
	void UpdateInputCopy( const FrameState& fs );
	void DrawModel( GLuint prog, const FrameState& fs, GpuModel* model, bool outline, bool soup, uint32_t limit, const float* vpOverride = nullptr,
	                const float* shadowLightPos = nullptr, const std::vector< Instance >* inst = nullptr );
	GLuint LodVAO( GpuModel* model, int lod );
	void UpdateEchoes( const FrameState& fs, int w, int h );
	void UpdateFaceReadback( const FrameState& fs );
	void SetLightUniforms( GLuint prog, const FrameState& fs, bool bindShadows );
	// Volume.cpp
	bool InitVolume( std::string& error );
	void ReleaseVolume();
	void ComputeLightMatrices( const FrameState& fs );
	void RenderShadows( const FrameState& fs, GpuModel* model );
	bool EnsureSceneDepth( int w, int h );
	void CopySceneDepth( int w, int h );
	void RenderVolume( const FrameState& fs, int w, int h );
	void SetSimMediumUniforms( GLuint prog, const FrameState& fs, const Mat4& VP );
	void SetTripUniforms( GLuint prog, const FrameState& fs );
	void RenderTripPost( const FrameState& fs, int w, int h );
	void DrawMotes( const FrameState& fs, int w, int h );
	void WarmVolume( const FrameState& fs, GpuModel* model, int w, int h );
	void EnsureNoise();
	// Sim.cpp (W20)
	bool InitSim( std::string& error );
	void ReleaseSim();
	void UpdateSim( const FrameState& fs, GpuModel* model );
	void SimStep( const FrameState& fs, float dt, bool last );
	void SimPass( GLuint prog, GLuint a, GLuint b = 0 );
	void SimStats( const FrameState& fs );
	bool SimReady() const { return simLive; }
	// Shape.cpp
	bool InitShape( std::string& error );
	void ReleaseShape();
	void UpdateShards( GpuModel* model, int count, int mode );
	static uint64_t ShardKey( const GpuModel* model, const ShapeState& s );
	bool UpdateLetters( GpuModel* model, const std::vector< float >& xf ); // true = the LETTERS programs can draw it
	GLuint LettersProgram( GLuint p ) const;
	void SetShapeUniforms( GLuint prog, const FrameState& fs, GpuModel* model );
	void UpdateDust( const FrameState& fs, GpuModel* model, bool prepOnly = false ); // prepOnly: sample + upload, no step
	void DrawDust( const FrameState& fs, GpuModel* model, int h );
	// Lightning.cpp (W26)
	bool InitBolts( std::string& error );
	void ReleaseBolts();
	void RenderGlow( const FrameState& fs, int w, int h ); // the HDR glow layer: fireflies (added) + bolts (max), then added once
	// Crack.cpp (W26)
	bool InitCracks( std::string& error );
	void ReleaseCracks();
	void DrawCracks( const FrameState& fs, GpuModel* model ); // the glow on the mask (additive, after the main draw)
	GLuint progSoupC = 0, progShadowSoupC = 0, progCrackMain = 0, progCrackSoup = 0, progCrackSoupC = 0;
	bool DrawHazeMask( const FrameState& fs, GpuModel* model, int w, int h ); // the visible cracks' glow at half res
	GLuint progCompositeHaze = 0, hazeTex = 0, hazeFBO = 0, hazeDepth = 0;
	int hazeW = 0, hazeH = 0;
	// Caustic.cpp (W26)
	bool InitCaustics( std::string& error );
	void ReleaseCaustics();
	void RenderCaustics( const FrameState& fs, GpuModel* model ); // before the main draw
	void DrawCookieMarch( const FrameState& fs, bool sim, GLuint tex, const float* vp, const float* pos, const float* color );
	GLuint progPhotonMain = 0, progPhotonSoup = 0, progPhotonSplat = 0, progCausticInit = 0;
	GLuint causticTex = 0, causticFBO = 0, causticDepth = 0, recvTex = 0, recvFBO = 0, photonTex[ 2 ] = {}, photonFBO = 0, photonDepth = 0, photonVAO = 0;
	float causticVP[ 16 ];
	bool causticOk = false;
	int glassFilter = 0; // DrawModel: 0 every part, 1 glass parts only (the photon pass)
	// Swarm.cpp (W26)
	bool InitSwarm( std::string& error );
	void ReleaseSwarm();
	void UpdateSwarm( const FrameState& fs, GpuModel* model );
	void DrawSwarm( const FrameState& fs, int w, int h ); // into the bound glow buffer (additive)
	void WarmSwarm( const FrameState& fs, int w, int h );
	GLuint progSwarmSim = 0, progSwarmSplat = 0, progSwarmDraw = 0, swarmGrid = 0, swarmGridFBO = 0;
	struct SwarmSlot
	{
		GLuint home = 0, state[ 2 ] = {}, simVAO[ 2 ] = {}, drawVAO[ 2 ] = {};
		uint64_t key = 0;
		int count = 0, idx = 0;
		bool live = false;
		std::shared_ptr< DustJob > job;
	} swarmSlot;
	GLuint progBolt = 0, progBoltAdd = 0, boltVAO = 0, boltVBO = 0, boltTex = 0, boltFBO = 0;
	int boltW = 0, boltH = 0;
	// Morph.cpp (W6 Stage 2): the MORPH program variants, their per-pair uniforms, the point cloud of styles 5 / 17
	bool InitMorph( std::string& error );
	void ReleaseMorph();
	void SetMorphUniforms( GLuint prog, const FrameState& fs, GpuModel* model );
	bool MorphPairReady( const FrameState& fs, GpuModel* a, GpuModel* b ); // the style's per-slot data is on the GPU for this pair
	bool UpdateCloud( int slot, GpuModel* model );                          // true once the slot's cloud is uploaded and baked
	void DrawCloud( const FrameState& fsA, const FrameState& fsB, int w, int h, bool glow ); // main pass (sprites) / light (glow)
	void WarmMorph( const FrameState& fs, GpuModel* model );
	GLuint progSoupM = 0, progSoupMI = 0, progShadowSoupM = 0, progShadowSoupMI = 0;
	GLuint progCloud = 0, progCloudBake = 0, cloudBakeFBO = 0, cloudVAO = 0, cloudWarmVAO = 0, cloudWarmVBO = 0;
	struct CloudSlot
	{
		uint64_t serial = 0;
		std::shared_ptr< CloudSlotJob > job;
		GLuint vbo = 0, colorTex = 0;
		GLuint refBuf = 0, posedBuf = 0; // MoveEhr W1: the cloud on the posed surface (an animated side): xyz + face normal
		bool baked = false;
	} cloud[ kSlots ];
	bool cloudGlowNow = false; // this frame's cloud lights the composite (DrawCloud glow after it)
	// Liquid.cpp (W26)
	bool InitLiquid( std::string& error );
	void ReleaseLiquid();
	bool UpdateLiquidSdf( GpuModel* a, GpuModel* b ); // the pair's slots; true once both SDFs are uploaded
	bool UpdateLiquidSlot( int slot, GpuModel* m );
	void RenderLiquidPaint( const FrameState& fsA, const FrameState& fsB, GpuModel* a, GpuModel* b, int w, int h );
	void RenderLiquid( const FrameState& fs, float t, int w, int h );
	GLuint progLiquid = 0, paintTex[ 2 ] = {}, paintFBO = 0, paintDepth = 0;
	int paintW = 0, paintH = 0;
	struct LiquidSmooth
	{
		std::atomic< bool > done{ false };
		SdfField field;
	};
	struct LiquidSdf
	{
		const CpuGeometry* geo = nullptr;
		std::shared_ptr< SdfJob > job;
		std::shared_ptr< LiquidSmooth > smooth;
		GLuint tex = 0;
		float min[ 3 ] = {}, size = 1;
	} liquidSdf[ kSlots ];
	// Projector.cpp (W26)
	bool InitProjector( std::string& error );
	void ReleaseProjector();
	void RenderProjector( const FrameState& fs, GpuModel* model ); // shadow + cookie into projTex (before the main draw)
	void DrawProjSurface( const FrameState& fs, GpuModel* model ); // the light on the mask (additive, after the main draw)
	void DrawProjMarch( const FrameState& fs, bool sim );          // the beam, added onto the fog's march target
	void BindCookieLights( GLuint prog, const FrameState& fs );
	GLuint progProjMain = 0, progProjSoup = 0, progProjSoupL = 0, progProjCookie = 0, progProjMarch = 0, progProjMarchSim = 0;
	GLuint projTex = 0, projFBO = 0, projDepth = 0, projImageTex = 0;
	// W2 video: the frame -> projImageTex (a draw: BGRA IOSurface bound zero-copy as a rectangle texture, or HAP blocks as DXT / BC)
	GLuint progProjVideo = 0, projVideoRect = 0, projVideoDxt = 0, projVideoAlpha = 0, projVideoFBO = 0, dummyRect = 0;
	int projImageW = 0, projImageH = 0, projDxtW = 0, projDxtH = 0;
	GLenum projVideoDxtFmt = 0, projVideoAlphaFmt = 0; // the compressed format each video texture was defined with
	bool projMipsValid = false;
	std::shared_ptr< const VideoFrame > projVideoHeld; // the frame bound to the rectangle texture (its IOSurface stays alive)
	bool UploadVideoFrame( const VideoFrame& f, bool mips, bool straight );
	bool projVideoStraight = false; // the Video Alpha the picture was drawn with
public:
	double videoUploadMs = 0; // W2: the last frame's upload (+ mips) ms on the render thread (PERF / harness)
private:
	uint64_t projImageSerial = 0;
	int projImageLevels = 1;
	float projVP[ 16 ];
	bool projOk = false;
	// Post.cpp
	bool InitPost( std::string& error );
	void ReleasePost();
	void RenderKeyShadow( const FrameState& fs, GpuModel* model );
	void RenderMirror( const FrameState& fs, GpuModel* model, int w, int h );
	void RenderSilhouetteBlur( const FrameState& fs, int w, int h );
	void Blur1D( GLuint src, GLuint dstFBO, int w, int h, float dx, float dy, int taps );
	bool PostActive( const FrameState& fs ) const;
	void EnsurePostTarget( int w, int h );
	void RenderPost( const FrameState& fs, int w, int h );
	void WarmPost( const FrameState& fs, GpuModel* model, int w, int h );
	void WarmWow( const FrameState& fs, GpuModel* model, int w, int h ); // Wow.cpp: the W26 programs

	GLuint progMain = 0, progSoup = 0, progComposite = 0, progCopy = 0, progMainI = 0, progSoupI = 0, progShadowI = 0, progShadowSoupI = 0;
	GLuint progSoupT = 0, progSoupTI = 0; // W16: the Shatter transition (plain / instanced)
	GLuint progSoupL = 0, progSoupLI = 0, progShadowSoupL = 0, progShadowSoupLI = 0; // W21: per-letter transforms
	struct LetterSlot
	{
		GLuint ofBuf = 0, xfBuf = 0, ofTex = 0, xfTex = 0;
		uint64_t key = 0; // model serial of the uploaded letter ids
		bool live    = false; // this frame draws through the LETTERS programs
	} letters[ kSlots ];
	GLuint progShadow = 0, progShadowSoup = 0, progFogMarch = 0, progFogTemporal = 0, progRaySrc = 0, progRayBlur = 0, progMotes = 0;
	GLuint dummyCube = 0, noiseTex = 0;
	// shadow maps for the volume lights (distance to the light in R32F); cube maps for lights 0/1 only
	GLuint shadowTex[ kMaxVolLights ] = {}, shadowCube[ 2 ] = {}, shadowFBO[ kMaxVolLights ] = {}, shadowDepth[ kMaxVolLights ] = {};
	int shadowMode[ kMaxVolLights ] = {}; // what the last RenderShadows produced: 0 none, 1 2D, 2 cube
	float lightVP[ kMaxVolLights ][ 16 ];
	// scene depth (single-sample copy) and the fog targets
	GLuint depthFBO = 0, sceneDepthTex = 0;
	int depthW = 0, depthH = 0;
	GLuint marchFBO = 0, fogTex = 0, fogDepthTex = 0, histFBO[ 2 ] = {}, histTex[ 2 ] = {}, raySrcFBO = 0, raySrcTex = 0;
	int fogW = 0, fogH = 0, histIdx = 0;
	bool histValid = false;
	GLuint fogOut = 0; // what the composite samples this frame
	float prevVP[ 16 ];
	uint32_t frameCounter = 0;
	// post
	static constexpr int kBloomLevels = 6;
	GLuint progBright = 0, progDown = 0, progUp = 0, progGhost = 0, progBlur1D = 0, progPost = 0;
	GLuint postTex = 0, postFBO = 0, downTex[ kBloomLevels ] = {}, downFBO[ kBloomLevels ] = {}, upTex[ kBloomLevels ] = {}, upFBO[ kBloomLevels ] = {};
	GLuint ghostTex = 0, ghostFBO = 0, streakTex[ 2 ] = {}, streakFBO[ 2 ] = {}, silTex[ 2 ] = {}, silFBO[ 2 ] = {};
	GLuint keyShadowTex = 0, keyShadowFBO = 0, keyShadowDepth = 0, mirrorTex = 0, mirrorFBO = 0, mirrorDepth = 0;
	int postW = 0, postH = 0, mirrorW = 0, mirrorH = 0, silW = 0, silH = 0;
	float keyVP[ 16 ], keyPos[ 3 ], keyDir[ 3 ];
	bool keyShadowOk = false, mirrorOk = false, silOk = false;
	// shape: shards + dust, one entry per model slot (A..D)
	GLuint progDustSim = 0, progDustDraw = 0, progDustTrans = 0, dummyShardBuf = 0, dummyShardOf = 0, dummyShardData = 0;
	struct ShardSlot
	{
		GLuint ofBuf = 0, dataBuf = 0, ofTex = 0, dataTex = 0;
		uint64_t key = 0;
		std::shared_ptr< ShardJob > job;
		// W26 Bounce: CPU copies + the recorded flights (per shard, 241 frames x 2 texels: offset, rotation)
		std::vector< float > data, radius;
		GLuint pathBuf = 0, pathTex = 0;
		uint64_t pathKey = 0;
		// W6 Pieces transition: per shard (rank of shard i bottom-up, 0..1; the shard of rank i) as RG32F, unit 29 / 30
		GLuint rankBuf = 0, rankTex = 0;
		int count = 0;
		// MoveEhr W2 A3: posed shard pivots. The triangles sorted by shard (R32UI), chunks of <= 256 of them (RG32UI begin, end),
		// per shard its chunk range (RG32UI); pass 1 sums each chunk's area-weighted posed centroid (RGBA32F), pass 2 each shard's
		// (pivot RGBA32F: fitted centroid + the seed, the shard data layout), unit 39 for the ANIM twins, 28 as a posed partner
		GLuint triBuf = 0, triTex = 0, chunkBuf = 0, chunkTex = 0, rangeBuf = 0, rangeTex = 0, partBuf = 0, partTex = 0, pivotBuf = 0, pivotTex = 0;
		int chunks = 0, pivotCount = 0;
		bool pivotOk = false; // the pivot buffer holds this frame's pose
		uint64_t pivotGen = 0, pivotKey = 0; // the pose generation + shard key the pivot buffer was made from
	} shards[ kSlots ];
	void UpdateBounce( const FrameState& fs, GpuModel* model ); // Shape.cpp (W26)
	GLuint progSoupB = 0, progShadowSoupB = 0;
	struct DustSlot
	{
		GLuint origin = 0, state[ 2 ] = {}, simVAO[ 2 ] = {}, drawVAO[ 2 ] = {};
		GLuint refBuf = 0, posedBuf = 0, simVAOp[ 2 ] = {}, drawVAOp[ 2 ] = {}; // MoveEhr W1: the posed origins (an animated side)
		bool posedNow = false;
		uint64_t key = 0;
		int count = 0, idx = 0;
		std::vector< std::array< uint32_t, 2 > > partRanges;
		std::shared_ptr< DustJob > job;
		bool live = false, warm = false;
		// W16: the dust noise's quantiles on this model (the Dust transition's key k = its CDF); until the points
		// exist, the noise's usual span
		float keyQ[ 17 ] = { 0.2f, 0.2375f, 0.275f, 0.3125f, 0.35f, 0.3875f, 0.425f, 0.4625f, 0.5f, 0.5375f, 0.575f, 0.6125f, 0.65f, 0.6875f, 0.725f, 0.7625f, 0.8f };
	} dust[ kSlots ];
	// face tracking readback (FX): input -> 640 px wide, rows flipped top-first -> PBO -> fence -> CPU
	GLuint progFaceCopy = 0, faceFBO = 0, faceTex = 0, facePBO = 0;
	GLsync faceFence = nullptr;
	int faceW = 0, faceH = 0;
	bool facePending = false;
	std::vector< uint8_t > facePixels;
	bool faceReady = false;
	// LoadEhr W1: which model slot the pair's side draws from; activeSide = the side being prepared / drawn now (the same file in
	// two slots is one GpuModel, so the side, not the pointer, picks the cache entry)
	int sideSlot[ 2 ] = { 0, 1 }, activeSide = 0;
	int SlotOf( GpuModel* ) const { return sideSlot[ activeSide ]; }
	// mid-hop, the passes that draw the model as a caster (light shadow maps, the key shadow, the projector's shadow) or as its
	// floor reflection draw the whole pair, each side with its own half of the transition: they morph with the picture
	bool pairOn = false;
	const FrameState* pairFs[ 2 ] = {};
	GpuModel* pairModel[ 2 ] = {};
	void DrawCasters( const FrameState& fs, GpuModel* model, const float* vp, const float* lightPos, bool withInst );
	GLuint progShadowSoupT = 0, progShadowSoupTI = 0; // the Shatter hop's shards in the shadow passes
	// the sim obstacle per slot (the dominant model's is bound)
	struct SimSdf
	{
		const CpuGeometry* geo = nullptr;
		std::shared_ptr< SdfJob > job;
		GLuint tex = 0;
		float min[ 3 ] = {}, size = 1;
		int solid = 0, cavity = 0;
	} simSdf[ kSlots ];
	bool EnsureSimSdf( int slot, GpuModel* model ); // true once uploaded
	// LOD vertex arrays (level 0 is `vao`)
	GLuint lodVao[ kLodLevels ] = {};
	uint64_t lodVaoFor[ kLodLevels ] = {};
	// echo trails: feedback ping-pong (premultiplied colour + age x coverage), resolved into echoTex for the composite
	GLuint trailCol[ 2 ] = {}, trailAge[ 2 ] = {}, trailFBO[ 2 ] = {}, echoTex = 0, echoFBO = 0, progTrail = 0, progEcho = 0;
	int echoW = 0, echoH = 0, trailIdx = 0, echoFrame = 0;
	float trailShift[ 2 ] = { 0, 0 }; // the sub-pixel remainder of the per-frame shift (whole pixels keep copies crisp)
	bool echoOk = false, trailLive = false;
	// W20 smoke sim: fields are 2D atlases of z-slices (see SimShaders.h)
	GLuint progSimObs = 0, progSimAdvect = 0, progSimCorrect = 0, progSimCurl = 0, progSimForce = 0, progSimDiv = 0, progSimJacobi = 0, progSimProject = 0;
	GLuint progFogMarchSim = 0, progRaySrcSim = 0;
	// W24 Trip: the march variants (#define TRIP) and the fog post (prism + hue echo, ping-pong at fog resolution)
	GLuint progFogMarchT = 0, progFogMarchSimT = 0, progTripPost = 0, tripTex[ 2 ] = {}, tripFBO[ 2 ] = {};
	GLuint progSimCorrectT = 0, progSimForceT = 0, progSimProjectT = 0; // the sim passes with the Trip fields / forces
	GLuint progSimCorrectF = 0, progSimForceF = 0, progFogMarchSimF = 0; // W26 fire (TRIP + FIRE)
	void SetFireUniforms( GLuint prog, const FrameState& fs );
	bool simTripWarm = false; // the sim (re)started: prewarm its TRIP passes and march once, through a 1-pixel scissor
	int tripW = 0, tripH = 0, tripIdx = 0;
	bool tripHist = false;
	GLuint simFBO = 0, simVel[ 2 ] = {}, simSca[ 2 ] = {}, simTmpV = 0, simTmpS = 0, simObs = 0, simDiv = 0, simP[ 2 ] = {};
	GLuint sdfTex = 0; // the bound obstacle: simSdf[ the dominant slot ] (not owned)
	float sdfMin[ 3 ] = { 0, 0, 0 }, sdfSize = 1;
	int simRes = 0, simTilesX = 0, simAtlasW = 0, simAtlasH = 0, simV = 0, simS = 0, simPi = 0;
	float simBoxMin[ 3 ] = { 0, 0, 0 }, simCell = 1, simAcc = 0, simPrewarm = 0;
	bool simLive = false;
	uint32_t simFrames = 0;
	double simStatMs = 0;
	GLuint quadVAO  = 0;
	GLuint whiteTex = 0;
	// targets
	int tw = 0, th = 0, tSamples = 0;
	GLuint msFBO = 0, msColor = 0, msDepth = 0;
	GLuint resolveFBO = 0, resolveTex = 0, resolveDepth = 0;
	// input copy
	GLuint inFBO = 0, inTex = 0;
	int inW = 0, inH = 0, inLevels = 1;
	// per-instance VAO for the current model
	GLuint vao        = 0;
	uint64_t vaoFor   = 0;
	GLuint vaoVbo     = 0; // MoveEhr W1: the vertex buffer the VAO reads (the posed one or the model's own)
	GLuint lodVaoVbo[ kLodLevels ] = {};
	// MoveEhr W1: the pose pass (src/Pose.cpp)
	struct SlotPose
	{
		bool active = false, posed = false;
		uint64_t serial = 0; // the GpuModel the pose belongs to
		std::vector< float > palette, weights, lastPalette, lastWeights;
		GLuint vbo = 0, tbo = 0, inVao = 0;
		uint64_t inFor = 0;
		size_t verts = 0;
		GLuint palBuf = 0, palTex = 0, wBuf = 0, wTex = 0;
		uint64_t gen = 0; // W2: +1 per pose pass run (what the buffer holds changed)
		// MoveEhr W4 Trail Off: the same slot posed 0.35 s earlier (a second pass into its own buffer, only while Trail Off is on)
		bool trailWant = false, trailPosed = false;
		std::vector< float > trailPalette, trailWeights, trailLast, trailLastW;
		GLuint trailVbo = 0, trailTbo = 0, trailPalBuf = 0, trailPalTex = 0, trailWBuf = 0, trailWTex = 0;
	} pose[ kSlots ];
	GLuint progPose = 0, progRefs = 0, refsVao = 0;
	GLuint progDustSimA = 0, progDustDrawA = 0, progDustTransA = 0; // MoveEhr W1: the dust programs on the posed origins (noise from the rest)
	// points given as (triangle, barycentric) refs -> their posed position + face normal (6 floats each) into outBuf; false = not posed
	bool PointsFromRefs( GpuModel* model, GLuint refBuf, GLuint outBuf, int count );
	std::unordered_map< GLuint, ProgSource > progSrc; // programs whose vertex stage writes vo.opos: their sources, for ANIM twins
	std::unordered_map< GLuint, GLuint > animTwins;
	bool InitPose( std::string& error );
	void RunPosePasses( GpuModel* a, GpuModel* b );
	// W2 A3: posed shard pivots of the pair's posed sides (after their shards are current); ShardPivotTex = 0 when a slot has none
	void RunShardPivots( GpuModel* a, GpuModel* b );
	void UploadShardCsr( ShardSlot& sh, const std::vector< uint16_t >& triShard, size_t tris, int count );
	GLuint ShardPivotTex( const GpuModel* model, int slot ) const;
	GLuint progPivotA = 0, progPivotB = 0, pivotVao = 0;
	void ReleasePose();
	const SlotPose* PosedFor( const GpuModel* model ) const;
	GLuint AnimTwin( GLuint p );
	// the models whose program variants were prewarmed (LoadEhr W1: each model once, not each time a hop makes it the pair's
	// outgoing model: re-warming at every hop cost a 25-37 ms frame)
	std::vector< uint64_t > warmedFor;
};
}
