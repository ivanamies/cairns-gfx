// Watercolor over the viking room: gaussian wash + color/depth edge
// darkening + noise wobble + paper granulation (Luft & Deussen 2006).
cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
cairns.dispatch("cairns.scene.spawnFitted",
                { glbs: ["viking_room.glb"], instances: 1, animated: false });
const e = cairns.dispatch("cairns.entity.new", { name: "watercolor" });
cairns.dispatch("cairns.entity.addComponent", {
    entity: e.result.entity, type: "PostEffect",
    props: { type: 2, order: 0,
             p0: [2.0, 6, 6, 0.6], p1: [4, 8, 0.4, 0] }
});
