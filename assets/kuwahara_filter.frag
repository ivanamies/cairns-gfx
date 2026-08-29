#version 450

// Kuwahara 3/3: 8-sector anisotropic filter (Kyprianidis GPU Pro 2010
// polynomial-weight formulation). Ellipse axes stretch along the tangent
// flow; sector weight = clamped quadratic * gaussian falloff; final color
// blends sector means by inverse variance^q.

layout(set = 0, binding = 0) uniform sampler2D u_color;
layout(set = 0, binding = 1) uniform sampler2D u_tfm;

layout(set = 1, binding = 0) uniform PostFxParams {
    vec4 screen;  // x = 1/w, y = 1/h, z = w, w = h
    vec4 p0;      // x = radius, y = q, z = alpha
    vec4 p1;
    vec4 p2;
};

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 s = vec2(inUv.x, 1.0 - inUv.y);
    float radius = max(p0.x, 1.0);
    float q = p0.y;
    float alpha = max(p0.z, 0.001);

    vec4 t = texture(u_tfm, s);
    float a = radius * clamp((alpha + t.w) / alpha, 0.1, 2.0);
    float b = radius * clamp(alpha / (alpha + t.w), 0.1, 2.0);
    float cos_phi = cos(t.z);
    float sin_phi = sin(t.z);
    mat2 R = mat2(cos_phi, -sin_phi, sin_phi, cos_phi);
    mat2 S = mat2(0.5 / a, 0.0, 0.0, 0.5 / b);
    mat2 SR = S * R;
    int max_x = int(sqrt(a * a * cos_phi * cos_phi +
                         b * b * sin_phi * sin_phi));
    int max_y = int(sqrt(a * a * sin_phi * sin_phi +
                         b * b * cos_phi * cos_phi));

    vec4 m[8];
    vec3 sq[8];
    for (int k = 0; k < 8; ++k) {
        m[k] = vec4(0.0);
        sq[k] = vec3(0.0);
    }
    for (int j = -max_y; j <= max_y; ++j) {
        for (int i = -max_x; i <= max_x; ++i) {
            vec2 v = SR * vec2(float(i), float(j));
            if (dot(v, v) > 0.25) {
                continue;
            }
            vec3 c = texture(u_color,
                             s + vec2(float(i) * screen.x,
                                      float(j) * screen.y)).rgb;
            float w[8];
            float z;
            float vxx = 0.33 - 3.77 * v.x * v.x;
            float vyy = 0.33 - 3.77 * v.y * v.y;
            z = max(0.0, v.y + vxx);  w[0] = z * z;
            z = max(0.0, -v.x + vyy); w[2] = z * z;
            z = max(0.0, -v.y + vxx); w[4] = z * z;
            z = max(0.0, v.x + vyy);  w[6] = z * z;
            vec2 vr = 0.7071067812 * vec2(v.x - v.y, v.x + v.y);
            vxx = 0.33 - 3.77 * vr.x * vr.x;
            vyy = 0.33 - 3.77 * vr.y * vr.y;
            z = max(0.0, vr.y + vxx);  w[1] = z * z;
            z = max(0.0, -vr.x + vyy); w[3] = z * z;
            z = max(0.0, -vr.y + vxx); w[5] = z * z;
            z = max(0.0, vr.x + vyy);  w[7] = z * z;
            float sum = w[0] + w[1] + w[2] + w[3] +
                        w[4] + w[5] + w[6] + w[7];
            if (sum < 1e-8) {
                continue;
            }
            float g = exp(-3.125 * dot(v, v)) / sum;
            for (int k = 0; k < 8; ++k) {
                float wk = w[k] * g;
                m[k] += vec4(c * wk, wk);
                sq[k] += c * c * wk;
            }
        }
    }
    vec4 o = vec4(0.0);
    for (int k = 0; k < 8; ++k) {
        if (m[k].w < 1e-8) {
            continue;
        }
        vec3 mean = m[k].rgb / m[k].w;
        vec3 var = abs(sq[k] / m[k].w - mean * mean);
        float sigma2 = var.r + var.g + var.b;
        float w = 1.0 / (1.0 + pow(255.0 * sigma2, 0.5 * q));
        o += vec4(mean * w, w);
    }
    outColor = (o.w > 1e-8) ? vec4(o.rgb / o.w, 1.0)
                            : vec4(texture(u_color, s).rgb, 1.0);
}
