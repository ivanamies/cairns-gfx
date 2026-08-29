// Compute particle sim (VK-tutorial style), WebGPU port of particle.metal's
// particle_compute. group0: binding0 = dt (dynamic UBO), binding1 = src
// particles (storage), binding2 = dst particles (storage). Ping-pong parity
// sets supply src/dst each step; the recorder carries dt as the dynamic offset.
// Particle layout matches the C++/metal struct: pos@0, vel@8, color@16 (32 B).

struct Particle {
    pos: vec2<f32>,
    vel: vec2<f32>,
    color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> dt: f32;
@group(0) @binding(1) var<storage, read_write> p_in: array<Particle>;
@group(0) @binding(2) var<storage, read_write> p_out: array<Particle>;

@compute @workgroup_size(256)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let tid = gid.x;
    var p = p_in[tid];
    p.pos = p.pos + p.vel * dt;
    if (p.pos.x < -1.0 || p.pos.x > 1.0) {
        p.vel.x = -p.vel.x;
    }
    if (p.pos.y < -1.0 || p.pos.y > 1.0) {
        p.vel.y = -p.vel.y;
    }
    p_out[tid] = p;
}
