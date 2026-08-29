// Watercolor 3/4: Sobel on blurred color AND linearized depth -> edge
// intensity. Mirrors wc_edge.frag. Binding 1 is a depth texture
// (texture_depth_2d + non-filtering sampler).

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    let p = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));
    var out: VsOut;
    out.uv = p;
    out.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

struct PostFxParams {
    screen: vec4<f32>,  // x = 1/w, y = 1/h
    p0: vec4<f32>,
    p1: vec4<f32>,      // x = color-edge gain, y = depth-edge gain
    p2: vec4<f32>,      // z = near, w = far
};

@group(0) @binding(0) var u_color: texture_2d<f32>;
@group(0) @binding(1) var u_depth: texture_depth_2d;
@group(0) @binding(2) var u_smp: sampler;
@group(0) @binding(3) var<uniform> params: PostFxParams;

fn lum(uv: vec2<f32>) -> f32 {
    let c = textureSampleLevel(u_color, u_smp, uv, 0.0).rgb;
    return dot(c, vec3<f32>(0.299, 0.587, 0.114));
}

fn lin_depth(uv: vec2<f32>) -> f32 {
    let d = textureSampleLevel(u_depth, u_smp, uv, 0u);
    let near = params.p2.z;
    let far = params.p2.w;
    let lin = near * far / max(far - d * (far - near), 1e-6);
    return clamp((lin - near) / max(far - near, 1e-6), 0.0, 1.0);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let s = vec2<f32>(in.uv.x, 1.0 - in.uv.y);
    let d = params.screen.xy;
    let l00 = lum(s + vec2<f32>(-d.x, -d.y));
    let l10 = lum(s + vec2<f32>(0.0, -d.y));
    let l20 = lum(s + vec2<f32>(d.x, -d.y));
    let l01 = lum(s + vec2<f32>(-d.x, 0.0));
    let l21 = lum(s + vec2<f32>(d.x, 0.0));
    let l02 = lum(s + vec2<f32>(-d.x, d.y));
    let l12 = lum(s + vec2<f32>(0.0, d.y));
    let l22 = lum(s + vec2<f32>(d.x, d.y));
    let lgx = l20 + 2.0 * l21 + l22 - l00 - 2.0 * l01 - l02;
    let lgy = l02 + 2.0 * l12 + l22 - l00 - 2.0 * l10 - l20;
    let ce = length(vec2<f32>(lgx, lgy));
    let z00 = lin_depth(s + vec2<f32>(-d.x, -d.y));
    let z10 = lin_depth(s + vec2<f32>(0.0, -d.y));
    let z20 = lin_depth(s + vec2<f32>(d.x, -d.y));
    let z01 = lin_depth(s + vec2<f32>(-d.x, 0.0));
    let z21 = lin_depth(s + vec2<f32>(d.x, 0.0));
    let z02 = lin_depth(s + vec2<f32>(-d.x, d.y));
    let z12 = lin_depth(s + vec2<f32>(0.0, d.y));
    let z22 = lin_depth(s + vec2<f32>(d.x, d.y));
    let zgx = z20 + 2.0 * z21 + z22 - z00 - 2.0 * z01 - z02;
    let zgy = z02 + 2.0 * z12 + z22 - z00 - 2.0 * z10 - z20;
    let de = length(vec2<f32>(zgx, zgy));
    let e = clamp(ce * params.p1.x + de * params.p1.y, 0.0, 1.0);
    return vec4<f32>(e, e, e, 1.0);
}
