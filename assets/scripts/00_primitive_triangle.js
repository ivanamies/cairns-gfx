// Procedural triangle primitive -- a real vbo mesh through the static-mesh
// path, NOT a fullscreen vertex-less special-case (Adreno mis-rasterizes
// those at MSAA). Scales to pyramid/cylinder/ellipse/ellipsoid.
cairns.dispatch("cairns.primitive.create", { type: "triangle" });
