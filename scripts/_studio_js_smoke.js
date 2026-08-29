// #225 R5: studio_js smoke test. Exercises the new Unity-shaped surface
// (Prefab / Scene / Prefabs / Editor) and the #226 reserved stubs. Each
// assertion either returns a value or throws -- the wrapper sh script
// counts both via the printed { ok, fail, errors } summary.

function _assertEq(name, got, want) {
    if (got !== want) {
        throw new Error(name + ": got " + JSON.stringify(got) +
                        " want " + JSON.stringify(want));
    }
}
function _assertThrows(name, fn, matchSubstr) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    if (!threw) {
        throw new Error(name + ": expected to throw, did not");
    }
    if (matchSubstr && threw.message.indexOf(matchSubstr) < 0) {
        throw new Error(name + ": threw '" + threw.message +
                        "' but expected to contain '" + matchSubstr + "'");
    }
}

const results = { ok: 0, fail: 0, errors: [] };
function run(name, fn) {
    try { fn(); results.ok++; }
    catch (e) {
        results.fail++;
        results.errors.push(name + ": " + (e.message || e));
    }
}

// ── Clone bin: Unity-named globals must exist + behave Unity-ish. ──
run("Vector3.zero", () => {
    const v = Vector3.zero;
    _assertEq("zero.x", v.x, 0);
});
run("Mathf.Lerp", () => _assertEq("lerp", Mathf.Lerp(0, 10, 0.5), 5));

// ── New nouns: Prefab + Scene + Editor + Prefabs. ──
// #224 L9: boot loads zero prefabs. The script must explicitly load
// at least one before testing Prefabs.list / Scene.instantiate. The
// JS owns the catalog -- cairns.prefab.load takes a single path; the
// loop belongs in the script.
run("cairns.prefab.load (single path) succeeds", () => {
    const r = cairns.dispatch("cairns.prefab.load", { path: "aatrox.glb" });
    if (!r.ok || !r.result || !r.result.ok) {
        throw new Error("aatrox.glb load failed: " + JSON.stringify(r));
    }
});

run("Prefabs.list returns non-empty after load", () => {
    const ps = Prefabs.list();
    if (!Array.isArray(ps) || ps.length === 0) {
        throw new Error("expected non-empty array, got " + JSON.stringify(ps));
    }
    if (typeof ps[0].id !== "number") {
        throw new Error("prefab.id should be number");
    }
});

run("Editor.newScene returns Scene handle", () => {
    const s = Editor.newScene();
    if (typeof s.id !== "number") {
        throw new Error("scene.id should be number");
    }
});

run("Scene.instantiate(prefab) returns GameObject", () => {
    const prefab = Prefabs.list()[0];
    // active engine scene is implicit today; pass scene id 0 explicitly.
    const scene = new Scene(0);
    const go = scene.instantiate(prefab, Vector3.zero);
    if (typeof go.entity !== "number") {
        throw new Error("go.entity should be number");
    }
});

// ── Refuse bin: there is NO global Instantiate. ──
run("global Instantiate refused", () => {
    _assertThrows("Instantiate", () => Instantiate(),
                   "Instantiate is not defined");
});

// ── R6 stubs: #226 features throw loud. ──
run("Editor.thumbnails throws #226 CAP-2", () => {
    _assertThrows("Editor.thumbnails", () => Editor.thumbnails(),
                   "#226 CAP-2");
});
run("Editor.compose throws #226 CAP-3", () => {
    _assertThrows("Editor.compose", () => Editor.compose(),
                   "#226 CAP-3");
});
run("Scene.addCamera throws #226 CAP-1", () => {
    _assertThrows("Scene.addCamera", () => (new Scene(0)).addCamera(),
                   "#226 CAP-1");
});

JSON.stringify(results);
