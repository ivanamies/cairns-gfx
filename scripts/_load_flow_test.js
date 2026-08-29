// #224 L9 verification: the actual user flow.
//
//   1. start engine; assert nothing loaded (count == 0)
//   2. load 3 paths;  assert count == 3
//   3. snapshot prefab handles
//   4. load 3 more paths;  assert count == 6
//   5. assert the first 3 prefabs' Mesh::Hot handles are byte-identical
//      after step 4 (the L6 APPEND-only contract)
//
// Returns JSON {ok, fail, errors[]}; verify_load_flow.sh asserts fail===0.

const BATCH_A = [
    "aatrox.glb",
    "aatrox_blood_moon.glb",
    "aatrox_drx.glb",
];
const BATCH_B = [
    "ahri.glb",
    "ahri_academy.glb",
    "ahri_arcade.glb",
];

const results = { ok: 0, fail: 0, errors: [] };
function run(name, fn) {
    try { fn(); results.ok++; }
    catch (e) {
        results.fail++;
        results.errors.push(name + ": " + (e.message || e));
    }
}
function _call(op, args) {
    const r = cairns.dispatch(op, args || {});
    if (!r || r.ok === false) {
        throw new Error("dispatch " + op + " failed: " +
                        JSON.stringify(r.error || r));
    }
    return r.result;
}
function _count() { return _call("cairns.prefab.count").count; }

run("step 1: engine starts empty", () => {
    const c = _count();
    if (c !== 0) {
        throw new Error("expected count=0 at boot, got " + c);
    }
});

run("step 2: load 3 paths, count becomes 3", () => {
    for (const p of BATCH_A) {
        const r = _call("cairns.prefab.load", { path: p });
        if (!r.ok) {
            throw new Error("load failed for " + p + ": " + JSON.stringify(r));
        }
    }
    const c = _count();
    if (c !== 3) {
        throw new Error("expected count=3 after batch A, got " + c);
    }
});

run("step 3: snapshot prefab handles", () => {
    const r = _call("cairns.debug.snapshotPrefabHandles");
    if (typeof r.snapshot_size !== "number" || r.snapshot_size === 0) {
        throw new Error("snapshot returned empty: " + JSON.stringify(r));
    }
});

run("step 4: load 3 more paths, count becomes 6", () => {
    for (const p of BATCH_B) {
        const r = _call("cairns.prefab.load", { path: p });
        if (!r.ok) {
            throw new Error("load failed for " + p + ": " + JSON.stringify(r));
        }
    }
    const c = _count();
    if (c !== 6) {
        throw new Error("expected count=6 after batch B, got " + c);
    }
});

run("step 5: first 3 prefabs' Mesh::Hot handles unchanged (APPEND-only)", () => {
    const r = _call("cairns.debug.assertAppendOnly");
    if (r.mismatches !== 0) {
        throw new Error("APPEND-only contract violated: " + r.mismatches +
                        " mismatch(es) -- batch B mutated batch A's handles");
    }
});

JSON.stringify(results);
