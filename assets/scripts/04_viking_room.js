// Subject: viking room -- UVs + depth.
cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
cairns.dispatch("cairns.scene.spawnFitted",
                { glbs: ["viking_room.glb"], instances: 1, animated: false });
