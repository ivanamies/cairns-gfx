#include <metal_stdlib>
using namespace metal;

struct Particle {
    float2 position;
    float2 velocity;
    float4 color;
};

kernel void particle_compute(
    device const Particle* in [[buffer(1)]],
    device Particle* out      [[buffer(2)]],
    constant float& dt        [[buffer(0)]],
    uint tid [[thread_position_in_grid]])
{
    Particle p = in[tid];
    p.position += p.velocity * dt;
    if (p.position.x < -1.0f || p.position.x > 1.0f) {
        p.velocity.x *= -1.0f;
    }
    if (p.position.y < -1.0f || p.position.y > 1.0f) {
        p.velocity.y *= -1.0f;
    }
    out[tid] = p;
}

struct ParticleVertexOut {
    float4 position [[position]];
    float4 color;
    float point_size [[point_size]];
};

vertex ParticleVertexOut particle_vertex(
    uint vid [[vertex_id]],
    device const Particle* particles [[buffer(0)]])
{
    ParticleVertexOut out;
    out.position = float4(particles[vid].position, 0.0f, 1.0f);
    out.color = particles[vid].color;
    out.point_size = 4.0f;
    return out;
}

fragment float4 particle_fragment(ParticleVertexOut in [[stage_in]])
{
    return in.color;
}
