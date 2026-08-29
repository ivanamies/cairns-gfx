// Lit primitives: the five procedural primitives under a directional light,
// every material flipped to the lit (half-lambert) family. Analytic normals
// + flat-color textures make shading errors read instantly.
cairns.dispatch("cairns.primitive.createAll", {});
const e = cairns.dispatch("cairns.entity.new", { name: "sun" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: e.result.entity, type: "DirectionalLight",
    props: { dirX: -0.5, dirY: -1.0, dirZ: -0.3,
             colorR: 1.0, colorG: 0.95, colorB: 0.85,
             intensity: 1.0,
             ambientR: 0.12, ambientG: 0.12, ambientB: 0.15 }
});
cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
