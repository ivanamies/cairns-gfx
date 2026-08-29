// Shadow primitives: the five procedural primitives over a flat ground die,
// lit by a cast_shadows directional light. Exercises the shadow_vp0 depth
// pass + PCF sampling in the lit fragment.
cairns.dispatch("cairns.primitive.createAll", {});
const e = cairns.dispatch("cairns.entity.new", { name: "sun" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: e.result.entity, type: "DirectionalLight",
    props: { dirX: -0.4, dirY: -1.0, dirZ: -0.3,
             colorR: 1.0, colorG: 0.96, colorB: 0.88,
             intensity: 1.1,
             ambientR: 0.10, ambientG: 0.10, ambientB: 0.13,
             castShadows: true }
});
cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
