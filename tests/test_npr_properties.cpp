// tests/test_npr_properties.cpp
//
// Reference-free correctness properties for the NPR work (M1 lighting, M2
// shadows, M3-M5 post-effect chain). The golden refs lock in DETERMINISM of
// whatever was first baked -- they cannot catch a wrong-but-stable render
// (the M2 "green but ambient-only" bug). Each SCENARIO here asserts a
// physical or mathematical property the correct implementation must satisfy,
// computed from the rendered pixels themselves, with no baked reference.
// Assertions avoid screen-Y entirely (readback row order is a backend
// detail); left/right (world +X maps to screen right under the +Z look-at
// camera) and image-wide statistics carry every check.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "engine.hpp"
#include "rhi/init_config.hpp"

#include "golden_js.hpp"
#include "test_seams.hpp"

namespace {

struct Image {
    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
};

Image Grab(cairns::Engine& e, uint32_t advance) {
    namespace seam = cairns::test_seams;
    Image img;
    REQUIRE(seam::AdvanceFrames(e, advance));
    REQUIRE(seam::ReadFinalTargetRgba(e, img.rgba, img.w, img.h));
    REQUIRE(img.w > 0);
    REQUIRE(img.h > 0);
    return img;
}

float Lum(const uint8_t* px) {
    return 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
}

// Mean luminance over the horizontal band [x0_frac, x1_frac) of the image.
float MeanLumBand(const Image& img, float x0_frac, float x1_frac) {
    const uint32_t x0 = static_cast<uint32_t>(x0_frac * img.w);
    const uint32_t x1 = static_cast<uint32_t>(x1_frac * img.w);
    double sum = 0.0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < img.h; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            sum += Lum(&img.rgba[(y * img.w + x) * 4]);
            ++n;
        }
    }
    return n ? static_cast<float>(sum / n) : 0.0f;
}

float MeanLum(const Image& img) { return MeanLumBand(img, 0.0f, 1.0f); }

struct DiffStats {
    int max_darken = 0;      // max per-pixel luminance drop a -> b
    int max_brighten = 0;    // max per-pixel luminance gain a -> b
    uint64_t darkened = 0;   // pixels darker in b by > thresh
    double darken_cx = 0.0;  // centroid x (pixels) of the darkened set
};

DiffStats Diff(const Image& a, const Image& b, int thresh) {
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    DiffStats st;
    double cx_sum = 0.0;
    for (uint32_t y = 0; y < a.h; ++y) {
        for (uint32_t x = 0; x < a.w; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
            const int d = static_cast<int>(Lum(&a.rgba[i])) -
                          static_cast<int>(Lum(&b.rgba[i]));
            if (d > st.max_darken) {
                st.max_darken = d;
            }
            if (-d > st.max_brighten) {
                st.max_brighten = -d;
            }
            if (d > thresh) {
                ++st.darkened;
                cx_sum += x;
            }
        }
    }
    st.darken_cx = st.darkened ? cx_sum / static_cast<double>(st.darkened)
                               : 0.0;
    return st;
}

bool BytesEqual(const Image& a, const Image& b) {
    return a.w == b.w && a.h == b.h && a.rgba == b.rgba;
}

// Distinct values of one channel across the whole image.
size_t DistinctChannelValues(const Image& img, int channel) {
    std::set<uint8_t> vals;
    const size_t n = static_cast<size_t>(img.w) * img.h;
    for (size_t i = 0; i < n; ++i) {
        vals.insert(img.rgba[i * 4 + channel]);
    }
    return vals.size();
}

// Boots a 256^2 headless engine (small: the suite boots ~20 of these).
struct PropEngine {
    cairns::Engine e;
    PropEngine() {
        cairns::test_seams::EnsureImguiContext();
        cairns::rhi::InitConfig icfg{};
        icfg.surfaceless = true;
        icfg.width = 256;
        icfg.height = 256;
        cairns::EngineConfig ecfg{};
        ecfg.use_fixed_clock = true;
        REQUIRE(e.GreaterInit(icfg, ecfg));
    }
};

// One white ellipsoid, lit shader, camera on +Z looking at the origin.
const char* kLitSphereJs = R"JS(
    cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 4 });
    cairns.dispatch("cairns.primitive.create",
                    { type: "ellipsoid", color: [1, 1, 1, 1] });
    cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
)JS";

std::string LightJs(float dx, float dy, float dz, float intensity,
                    bool cast_shadows) {
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "const _l = cairns.dispatch('cairns.entity.new', { name: 'sun' });"
        "cairns.dispatch('cairns.entity.addComponent', {"
        "  entity: _l.result.entity, type: 'DirectionalLight',"
        "  props: { dirX: %f, dirY: %f, dirZ: %f,"
        "           colorR: 1.0, colorG: 1.0, colorB: 1.0, intensity: %f,"
        "           ambientR: 0.08, ambientG: 0.08, ambientB: 0.08,"
        "           castShadows: %s } });",
        static_cast<double>(dx), static_cast<double>(dy),
        static_cast<double>(dz), static_cast<double>(intensity),
        cast_shadows ? "true" : "false");
    return buf;
}

std::string PostFxJs(int type, const char* p0, const char* p1) {
    std::string js =
        "const _fx = cairns.dispatch('cairns.entity.new', { name: 'fx' });"
        "cairns.dispatch('cairns.entity.addComponent', {"
        "  entity: _fx.result.entity, type: 'PostEffect',"
        "  props: { type: ";
    js += std::to_string(type);
    js += ", order: 0, p0: ";
    js += p0;
    js += ", p1: ";
    js += p1;
    js += " } });";
    return js;
}

const char* kVikingJs = R"JS(
    cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
    cairns.dispatch("cairns.scene.spawnFitted",
                    { glbs: ["viking_room.glb"], instances: 1,
                      animated: false });
)JS";

Image RenderViking(const std::string& fx_js) {
    PropEngine pe;
    cairns::golden::DriveJs(pe.e, std::string(kVikingJs) + fx_js);
    return Grab(pe.e, 9);
}

bool VikingPresent() {
    return cairns::test_seams::AssetsPresent({"viking_room.glb"});
}

// Large ground sphere seen from a bird-view camera, optionally with a small
// occluder sphere at `occluder_y` over it. setTransform overrides the fitted
// placement.
Image RenderShadowSceneAt(float light_dx, bool cast, bool with_occluder,
                          float occluder_y) {
    PropEngine pe;
    std::string js = R"JS(
        // Bird view: eye (0,6,8) pitched down at the origin, so the ground
        // patch around the occluder's shadow faces the camera.
        cairns.dispatch("cairns.viewport.setCamera",
                        { viewport: 0, x: 0, y: 6, z: 8,
                          yaw: 0, pitch: -0.6435 });
        const _b0 = cairns.dispatch("cairns.scene.listEntities", {})
                        .result.entities;
        cairns.dispatch("cairns.primitive.create",
                        { type: "ellipsoid", color: [0.9, 0.9, 0.9, 1] });
        const _b1 = cairns.dispatch("cairns.scene.listEntities", {})
                        .result.entities;
        const ground = _b1.filter(e => !_b0.includes(e))[0];
        cairns.dispatch("cairns.scene.setTransform",
                        { entity: ground, x: 0, y: -9.5, z: 0, scale: 9 });
    )JS";
    if (with_occluder) {
        char buf[512];
        std::snprintf(
            buf, sizeof(buf),
            "const _b2 = cairns.dispatch('cairns.scene.listEntities', {})"
            "                .result.entities;"
            "cairns.dispatch('cairns.primitive.create',"
            "                { type: 'ellipsoid', color: [0.9, 0.2, 0.2, 1] });"
            "const _b3 = cairns.dispatch('cairns.scene.listEntities', {})"
            "                .result.entities;"
            "const occluder = _b3.filter(e => !_b2.includes(e))[0];"
            "cairns.dispatch('cairns.scene.setTransform',"
            "                { entity: occluder, x: 0, y: %f, z: 0,"
            "                  scale: 0.5 });",
            static_cast<double>(occluder_y));
        js += buf;
    }
    js += "cairns.dispatch('cairns.scene.setMaterialShaderAll',"
          "                { shader: 'lit' });";
    // dz tilts the rays toward the camera so the shadow lands on the
    // camera-facing slope of the ground sphere (a straight-down shadow sits
    // on the sphere's crown, edge-on to the +Z camera and barely visible).
    js += LightJs(light_dx, -1.0f, 0.3f, 1.2f, cast);
    cairns::golden::DriveJs(pe.e, js);
    return Grab(pe.e, 9);
}

Image RenderShadowScene(float light_dx, bool cast) {
    return RenderShadowSceneAt(light_dx, cast, true, 1.2f);
}

}  // namespace

// ---- M1: directional lighting ----------------------------------------------

SCENARIO("npr property: directional light illuminates the facing side",
         "[npr_property][golden]") {
    // Light arriving FROM +X (travel direction -X) must brighten the sphere's
    // right (screen +X) half; the mirrored light flips it. Catches sign errors
    // in the N.L convention and a silently-ignored light (the M2 bug class).
    float asym_from_px = 0.0f;
    {
        PropEngine pe;
        cairns::golden::DriveJs(pe.e, std::string(kLitSphereJs) +
                                          LightJs(-1.0f, -0.15f, -0.3f, 1.2f,
                                                  false));
        const Image img = Grab(pe.e, 9);
        const float left = MeanLumBand(img, 0.05f, 0.45f);
        const float right = MeanLumBand(img, 0.55f, 0.95f);
        INFO("light from +X: left=" << left << " right=" << right);
        REQUIRE(right > left + 2.0f);
        asym_from_px = right - left;
    }
    {
        PropEngine pe;
        cairns::golden::DriveJs(pe.e, std::string(kLitSphereJs) +
                                          LightJs(1.0f, -0.15f, -0.3f, 1.2f,
                                                  false));
        const Image img = Grab(pe.e, 9);
        const float left = MeanLumBand(img, 0.05f, 0.45f);
        const float right = MeanLumBand(img, 0.55f, 0.95f);
        INFO("light from -X: left=" << left << " right=" << right);
        REQUIRE(left > right + 2.0f);
        // Mirrored light, mirrored asymmetry (loose tolerance: the sphere
        // tessellation is not perfectly symmetric).
        REQUIRE(std::fabs((left - right) - asym_from_px) < 4.0f);
    }
}

SCENARIO("npr property: light intensity scales brightness; no light = ambient",
         "[npr_property][golden]") {
    auto mean_with = [&](const std::string& light_js) {
        PropEngine pe;
        cairns::golden::DriveJs(pe.e, std::string(kLitSphereJs) + light_js);
        return MeanLum(Grab(pe.e, 9));
    };
    const float none = mean_with("");
    const float dim = mean_with(LightJs(-0.4f, -1.0f, -0.3f, 0.4f, false));
    const float hot = mean_with(LightJs(-0.4f, -1.0f, -0.3f, 1.6f, false));
    INFO("mean lum: none=" << none << " dim=" << dim << " hot=" << hot);
    // Existence-based lighting: a lit material with no DirectionalLight
    // renders ambient-only, strictly darker than any lit variant.
    REQUIRE(dim > none + 1.0f);
    // Monotone in intensity.
    REQUIRE(hot > dim + 1.0f);
}

// ---- M2: directional shadows -------------------------------------------------

SCENARIO("npr property: shadows only darken, and toggle with castShadows",
         "[npr_property][golden]") {
#if CAIRNS_WEBGPU
    SKIP("webgpu stubs the depth-only shadow PSO -- shadows honestly off");
#endif
    const Image off = RenderShadowScene(0.0f, false);
    const Image on = RenderShadowScene(0.0f, true);
    const DiffStats st = Diff(off, on, 10);
    INFO("darkened=" << st.darkened << " max_darken=" << st.max_darken
                     << " max_brighten=" << st.max_brighten);
    // A shadow exists: a real patch of pixels lost >10 luminance.
    REQUIRE(st.darkened > 100);
    REQUIRE(st.max_darken > 25);
    // Shadow sampling must never ADD light (small epsilon for PCF edges).
    REQUIRE(st.max_brighten <= 6);
}

SCENARIO("npr property: shadow displaces along the light's tilt",
         "[npr_property][golden]") {
#if CAIRNS_WEBGPU
    SKIP("webgpu stubs the depth-only shadow PSO -- shadows honestly off");
#endif
    // Light rays travel along dir; tilting dir.x positive throws the
    // occluder's shadow toward +X (screen right), negative toward -X. This is
    // the headless form of the "is the shadow on the correct side / is the
    // map flipped" eyeball the TODO flags for interactive verification.
    const Image off_r = RenderShadowScene(0.45f, false);
    const Image on_r = RenderShadowScene(0.45f, true);
    const Image off_l = RenderShadowScene(-0.45f, false);
    const Image on_l = RenderShadowScene(-0.45f, true);
    const DiffStats right = Diff(off_r, on_r, 10);
    const DiffStats left = Diff(off_l, on_l, 10);
    INFO("tilt +x: darkened=" << right.darkened << " cx=" << right.darken_cx);
    INFO("tilt -x: darkened=" << left.darkened << " cx=" << left.darken_cx);
    REQUIRE(right.darkened > 100);
    REQUIRE(left.darkened > 100);
    const double mid = static_cast<double>(off_r.w) / 2.0;
    REQUIRE(right.darken_cx > mid + 3.0);
    REQUIRE(left.darken_cx < mid - 3.0);
}

SCENARIO("npr property: no shadow acne on unoccluded geometry",
         "[npr_property][golden]") {
#if CAIRNS_WEBGPU
    SKIP("webgpu stubs the depth-only shadow PSO -- shadows honestly off");
#endif
    // The ground sphere alone: convex, so every camera-visible pixel is
    // light-facing and nothing legitimately occludes it. castShadows on must
    // render IDENTICAL to off -- any diff is self-shadow acne, i.e. the
    // slope-scaled bias in shadow_factor is too small for the shadow map's
    // depth precision over this ortho range.
    const Image off = RenderShadowSceneAt(0.45f, false, false, 0.0f);
    const Image on = RenderShadowSceneAt(0.45f, true, false, 0.0f);
    const DiffStats st = Diff(off, on, 1);
    INFO("acne: max_darken=" << st.max_darken
                             << " max_brighten=" << st.max_brighten);
    REQUIRE(st.max_darken <= 1);
    REQUIRE(st.max_brighten <= 1);
}

SCENARIO("npr property: near-contact shadow survives the bias (no peter-pan)",
         "[npr_property][golden]") {
#if CAIRNS_WEBGPU
    SKIP("webgpu stubs the depth-only shadow PSO -- shadows honestly off");
#endif
    // Occluder 0.3 world units above the ground: a gap larger than the
    // acne bias's world-space cost (~0.13 at this ortho range), so a correct
    // bias keeps a STRONG shadow here; an overtuned one detaches it. The
    // hard 1.2 X-tilt slides the umbra out from under the occluder's own
    // silhouette (a gentler tilt leaves it hidden from the bird camera --
    // physically correct but unmeasurable).
    const Image off = RenderShadowSceneAt(1.2f, false, true, 0.3f);
    const Image on = RenderShadowSceneAt(1.2f, true, true, 0.3f);
    const DiffStats st = Diff(off, on, 10);
    INFO("near-contact: darkened=" << st.darkened
                                   << " max_darken=" << st.max_darken);
    REQUIRE(st.darkened > 100);
    REQUIRE(st.max_darken > 40);
}

// ---- M3: kuwahara + the params plumbing ---------------------------------------

SCENARIO("npr property: kuwahara is identity on constant color regions",
         "[npr_property][golden]") {
    // The filter is a weighted mean of sector means: on a constant input
    // every sector mean equals the input, so an empty scene (clear color
    // everywhere) must pass through within 1 LSB.
    PropEngine pe_on;
    cairns::golden::DriveJs(
        pe_on.e, std::string("cairns.dispatch('cairns.viewport.setCamera', "
                             "{ viewport: 0, z: 5 });") +
                     PostFxJs(0, "[6, 8, 1, 0]", "[0, 0, 0, 0]"));
    const Image on = Grab(pe_on.e, 9);
    PropEngine pe_off;
    cairns::golden::DriveJs(pe_off.e,
                            "cairns.dispatch('cairns.viewport.setCamera', "
                            "{ viewport: 0, z: 5 });");
    const Image off = Grab(pe_off.e, 9);
    const DiffStats st = Diff(off, on, 1);
    INFO("max_darken=" << st.max_darken
                       << " max_brighten=" << st.max_brighten);
    REQUIRE(st.max_darken <= 1);
    REQUIRE(st.max_brighten <= 1);
}

SCENARIO("npr property: kuwahara radius param reaches the shader",
         "[npr_property][golden]") {
    // End-to-end proof of the DrawFullscreenParams plumbing (dyn-UBO offset,
    // per-backend layouts): two radii must produce different abstractions,
    // and both must differ from no-effect.
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    const Image r2 = RenderViking(PostFxJs(0, "[2, 8, 1, 0]", "[0,0,0,0]"));
    const Image r10 = RenderViking(PostFxJs(0, "[10, 8, 1, 0]", "[0,0,0,0]"));
    const Image plain = RenderViking("");
    REQUIRE_FALSE(BytesEqual(r2, r10));
    REQUIRE_FALSE(BytesEqual(plain, r2));
    REQUIRE_FALSE(BytesEqual(plain, r10));
}

// ---- M4: bloom -----------------------------------------------------------------

SCENARIO("npr property: bloom below threshold is a pixel-exact identity",
         "[npr_property][golden]") {
    // LDR forward color caps at 1.0; threshold 2.0 extracts nothing, the
    // ladder carries zeros, and combine adds intensity * 0. The whole chain
    // (bright + 3 downs + 3 ups + combine, params, uv flips, barriers) must
    // be an EXACT identity -- a half-texel flip error would break byte
    // equality through the bilinear taps.
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    const Image off = RenderViking("");
    const Image on =
        RenderViking(PostFxJs(1, "[2.0, 0.1, 1.0, 0]", "[0,0,0,0]"));
    REQUIRE(BytesEqual(off, on));
}

SCENARIO("npr property: bloom adds light and never darkens",
         "[npr_property][golden]") {
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    const Image off = RenderViking("");
    const Image on =
        RenderViking(PostFxJs(1, "[0.35, 0.3, 0.9, 0]", "[0,0,0,0]"));
    const DiffStats st = Diff(off, on, 4);
    INFO("max_darken=" << st.max_darken
                       << " max_brighten=" << st.max_brighten);
    // combine = color + intensity * bloom: monotone non-darkening (1 LSB
    // slack for the BGRA round-trip).
    REQUIRE(st.max_darken <= 1);
    // And it visibly bloomed somewhere.
    REQUIRE(st.max_brighten > 10);
    REQUIRE(MeanLum(on) > MeanLum(off));
}

// ---- M5: watercolor --------------------------------------------------------------

SCENARIO("npr property: watercolor quantizes tones to the level count",
         "[npr_property][golden]") {
    // wobble 0 + edge 0 + grain 0 reduces the composite to
    // quantize(blur(color), levels): each channel collapses to at most
    // levels+1 distinct values across the whole image.
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    const Image off = RenderViking("");
    const Image on = RenderViking(PostFxJs(2, "[2.0, 4, 0, 0]", "[0,0,0,0]"));
    const size_t distinct_off = DistinctChannelValues(off, 1);
    const size_t distinct_on = DistinctChannelValues(on, 1);
    INFO("distinct green values: off=" << distinct_off
                                       << " on=" << distinct_on);
    REQUIRE(distinct_off > 16);  // the scene has real tonal range
    REQUIRE(distinct_on <= 5);   // 4 levels -> {0, 1/4, 2/4, 3/4, 1}
}

SCENARIO("npr property: watercolor granulation textures flat regions",
         "[npr_property][golden]") {
    // Grain multiplies by the paper channel: the flat clear-color background
    // must gain per-pixel variation (proves the packed effect texture loads
    // and samples). High levels + wobble/edge off isolates the grain term.
    PropEngine pe;
    cairns::golden::DriveJs(
        pe.e, std::string("cairns.dispatch('cairns.viewport.setCamera', "
                          "{ viewport: 0, z: 5 });") +
                  PostFxJs(2, "[1.0, 64, 0, 0]", "[0, 0, 0.5, 0]"));
    const Image on = Grab(pe.e, 9);
    const size_t distinct = DistinctChannelValues(on, 1);
    INFO("distinct green values with grain on an empty scene: " << distinct);
    REQUIRE(distinct > 8);
}

// ---- chain infrastructure ---------------------------------------------------------

SCENARIO("npr property: destroying the PostEffect entity disengages the chain",
         "[npr_property][golden]") {
    // Extraction is per-frame from the live registry: after entity.destroy
    // the render must return to the baseline bytes (catches stale PerSlot
    // state or a swap still sampling the chain output). One registry, one JS
    // context: DriveJs would wipe globals between evals.
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    PropEngine pe;
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(pe.e);
    cairns::golden::EvalJs(reg, kVikingJs);
    const Image before = Grab(pe.e, 9);
    cairns::golden::EvalJs(reg,
                           PostFxJs(0, "[6, 8, 1, 0]", "[0,0,0,0]") +
                               "globalThis._fx_ent = _fx.result.entity;");
    // Advance in steps of 2 to hold frame parity constant against any
    // frame-alternating internal state.
    const Image with_fx = Grab(pe.e, 2);
    REQUIRE_FALSE(BytesEqual(before, with_fx));
    cairns::golden::EvalJs(reg,
                           "cairns.dispatch('cairns.entity.destroy', "
                           "{ entity: globalThis._fx_ent });");
    const Image after = Grab(pe.e, 2);
    REQUIRE(BytesEqual(before, after));
}

SCENARIO("npr property: multiple effects compose in order",
         "[npr_property][golden]") {
    if (!VikingPresent()) {
        SKIP("viking_room.glb not in this build");
    }
    // kuwahara(order 0) -> bloom(order 1): the second effect's input is the
    // first's output, so swapping the order slots must change the image.
    const char* k_then_b_js =
        "{ const a = cairns.dispatch('cairns.entity.new', {name:'a'});"
        "cairns.dispatch('cairns.entity.addComponent', { entity: "
        "a.result.entity, type: 'PostEffect', props: { type: 0, order: 0, "
        "p0: [6, 8, 1, 0] } });"
        "const b = cairns.dispatch('cairns.entity.new', {name:'b'});"
        "cairns.dispatch('cairns.entity.addComponent', { entity: "
        "b.result.entity, type: 'PostEffect', props: { type: 1, order: 1, "
        "p0: [0.35, 0.3, 0.9, 0] } }); }";
    const char* b_then_k_js =
        "{ const a = cairns.dispatch('cairns.entity.new', {name:'a'});"
        "cairns.dispatch('cairns.entity.addComponent', { entity: "
        "a.result.entity, type: 'PostEffect', props: { type: 1, order: 0, "
        "p0: [0.35, 0.3, 0.9, 0] } });"
        "const b = cairns.dispatch('cairns.entity.new', {name:'b'});"
        "cairns.dispatch('cairns.entity.addComponent', { entity: "
        "b.result.entity, type: 'PostEffect', props: { type: 0, order: 1, "
        "p0: [6, 8, 1, 0] } }); }";
    const Image k_then_b = RenderViking(k_then_b_js);
    const Image b_then_k = RenderViking(b_then_k_js);
    const Image plain = RenderViking("");
    REQUIRE_FALSE(BytesEqual(plain, k_then_b));
    REQUIRE_FALSE(BytesEqual(plain, b_then_k));
    REQUIRE_FALSE(BytesEqual(k_then_b, b_then_k));
}
