// Anisotropic Kuwahara over the viking room: exercises the post-effect
// chain (tensor -> tfm -> filter fullscreen passes between forward and swap).
cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
cairns.dispatch("cairns.scene.spawnFitted",
                { glbs: ["viking_room.glb"], instances: 1, animated: false });
const e = cairns.dispatch("cairns.entity.new", { name: "kuwahara" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: e.result.entity, type: "PostEffect",
    props: { type: 0, order: 0, p0: [6, 8, 1, 0] }
});
