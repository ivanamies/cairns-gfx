// Subject: one die -- single static textured mesh.
cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 4 });
cairns.dispatch("cairns.scene.spawnFitted",
                { glbs: ["die.glb"], instances: 1, animated: false });
