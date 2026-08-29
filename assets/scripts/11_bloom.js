// Bloom over lit primitives: a hot directional light pushes facing surfaces
// past the threshold; exercises the bright->down->up->combine ladder.
cairns.dispatch("cairns.primitive.createAll", {});
const sun = cairns.dispatch("cairns.entity.new", { name: "sun" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: sun.result.entity, type: "DirectionalLight",
    props: { dirX: -0.4, dirY: -1.0, dirZ: -0.3,
             colorR: 1.0, colorG: 0.98, colorB: 0.9,
             intensity: 1.6,
             ambientR: 0.10, ambientG: 0.10, ambientB: 0.13 }
});
cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
const fx = cairns.dispatch("cairns.entity.new", { name: "bloom" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: fx.result.entity, type: "PostEffect",
    props: { type: 1, order: 0, p0: [0.7, 0.5, 0.8, 0] }
});
