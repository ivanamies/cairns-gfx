// Procedural triangle primitive -- a real vbo mesh through the static-mesh
// path (replaces the old tiny_quad/red_triangle fullscreen special-case that
// Adreno mis-rasterized at MSAA). Scales to pyramid/cylinder/ellipse/ellipsoid.
cairns.dispatch("cairns.primitive.create", { type: "triangle" });
