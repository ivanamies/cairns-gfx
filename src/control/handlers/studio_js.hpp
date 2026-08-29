// control/handlers/studio_js.hpp
//
// Embedded JS source autoloaded into the QuickJS context at
// RegisterScriptOps time. Defines a Unity-shaped scripting surface
// (`Vector3`, `Quaternion`, `GameObject`, `Transform`, `Camera`,
// `Application`, `Cairns`) that lowers to `cairns.*` ops via
// `cairns.dispatch`. The shape mirrors a widely-known proprietary
// game engine for agent-training-corpus density; divergences are
// LOUD (rename / runtime-throw / await), never doc-only -- see
// docs/studio_notes.md.
//
// Lives as a string literal so cairns_serve doesn't need to load a
// runtime file. Any syntax error fails loud at engine init.

#pragma once

namespace cairns::control {

inline constexpr const char* kStudioJsSource = R"JS(
'use strict';

// =====================================================================
// Math types -- pure JS, zero registry round-trips. Marshalled to
// arrays only at the cairns.dispatch boundary.
// =====================================================================

class Vector3 {
    constructor(x = 0, y = 0, z = 0) {
        this.x = +x; this.y = +y; this.z = +z;
    }
    add(o) { return new Vector3(this.x + o.x, this.y + o.y, this.z + o.z); }
    sub(o) { return new Vector3(this.x - o.x, this.y - o.y, this.z - o.z); }
    mul(s) { return new Vector3(this.x * s, this.y * s, this.z * s); }
    dot(o) { return this.x * o.x + this.y * o.y + this.z * o.z; }
    cross(o) {
        return new Vector3(
            this.y * o.z - this.z * o.y,
            this.z * o.x - this.x * o.z,
            this.x * o.y - this.y * o.x);
    }
    magnitude() { return Math.sqrt(this.dot(this)); }
    normalized() {
        const m = this.magnitude();
        return m > 0 ? this.mul(1.0 / m) : new Vector3(0, 0, 0);
    }
    toArray() { return [this.x, this.y, this.z]; }
}
Object.defineProperty(Vector3, 'zero',    { get() { return new Vector3(0,0,0); } });
Object.defineProperty(Vector3, 'one',     { get() { return new Vector3(1,1,1); } });
Object.defineProperty(Vector3, 'up',      { get() { return new Vector3(0,1,0); } });
Object.defineProperty(Vector3, 'right',   { get() { return new Vector3(1,0,0); } });
Object.defineProperty(Vector3, 'forward', { get() { return new Vector3(0,0,1); } });

class Vector2 {
    constructor(x = 0, y = 0) { this.x = +x; this.y = +y; }
    add(o) { return new Vector2(this.x + o.x, this.y + o.y); }
    sub(o) { return new Vector2(this.x - o.x, this.y - o.y); }
    mul(s) { return new Vector2(this.x * s, this.y * s); }
    toArray() { return [this.x, this.y]; }
}

class Quaternion {
    constructor(x = 0, y = 0, z = 0, w = 1) {
        this.x = +x; this.y = +y; this.z = +z; this.w = +w;
    }
    toArray() { return [this.x, this.y, this.z, this.w]; }
}
Object.defineProperty(Quaternion, 'identity', {
    get() { return new Quaternion(0,0,0,1); }
});
Quaternion.Euler = function(x, y, z) {
    const hx = x * 0.5 * Math.PI / 180.0;
    const hy = y * 0.5 * Math.PI / 180.0;
    const hz = z * 0.5 * Math.PI / 180.0;
    const cx = Math.cos(hx), sx = Math.sin(hx);
    const cy = Math.cos(hy), sy = Math.sin(hy);
    const cz = Math.cos(hz), sz = Math.sin(hz);
    return new Quaternion(
        sx*cy*cz - cx*sy*sz,
        cx*sy*cz + sx*cy*sz,
        cx*cy*sz - sx*sy*cz,
        cx*cy*cz + sx*sy*sz);
};

const Mathf = {
    PI: Math.PI,
    Deg2Rad: Math.PI / 180.0,
    Rad2Deg: 180.0 / Math.PI,
    Clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); },
    Lerp(a, b, t) { return a + (b - a) * t; },
    Abs: Math.abs,
    Sin: Math.sin, Cos: Math.cos, Tan: Math.tan,
    Sqrt: Math.sqrt,
    Max: Math.max, Min: Math.min,
};

// =====================================================================
// Entity wrappers. Today's stub-id world: GameObject wraps an entity
// id (returned by cairns.scene.instantiate). Transform setters store
// the value on the wrapper -- the cairns side doesn't yet honor
// per-entity transforms, so dispatching today would be a no-op. When
// `cairns.entity.setTransform` lands the setter body becomes a single
// cairns.dispatch and the call sites don't change.
// =====================================================================

class Transform {
    constructor(go) {
        this._go = go;
        this._position = new Vector3(0, 0, 0);
        this._rotation = new Quaternion(0, 0, 0, 1);
        this._localScale = new Vector3(1, 1, 1);
    }
    // #229 C4.3: setters write through to cairns.entity.setTRS (the cache is
    // the JS-side value, write-through only). [N-node] every op carries
    // this._go._scene so a Transform edits its own document, not "the" scene.
    _apply() {
        cairns.dispatch("cairns.entity.setTRS", {
            scene: this._go._scene, entity: this._go._entity,
            t: [this._position.x, this._position.y, this._position.z],
            r: [this._rotation.x, this._rotation.y, this._rotation.z,
                this._rotation.w],
            s: [this._localScale.x, this._localScale.y, this._localScale.z],
        });
    }
    get position()   { return this._position; }
    set position(v)  { this._position = v; this._apply(); }
    get rotation()   { return this._rotation; }
    set rotation(q)  { this._rotation = q; this._apply(); }
    get localScale() { return this._localScale; }
    set localScale(v){ this._localScale = v; this._apply(); }
    LookAt(_target)  { /* TODO */ }
    Translate(v)     { this._position = this._position.add(v); this._apply(); }
    // Unity: parent is a Transform; also accept a GameObject or null (unparent).
    SetParent(parent) {
        let pe = null;
        if (parent) {
            pe = (parent._go && parent._go._entity !== undefined)
                ? parent._go._entity
                : (parent._entity !== undefined ? parent._entity : null);
        }
        const args = { scene: this._go._scene, entity: this._go._entity };
        if (pe !== null) { args.parent = pe; }
        cairns.dispatch("cairns.entity.setParent", args);
    }
}

class GameObject {
    // #225 R4: `scene_id` IS the composition binding (Unity Scene == cairns
    // container of GameObjects). Old `world_id` parameter kept as a positional
    // alias for one release.
    constructor(entity_id, scene_id) {
        this._entity = entity_id;
        this._scene = scene_id;
        this.transform = new Transform(this);
        this._components = new Map();
        this._active = true;
    }

    static InstantiateAsync(prefab_or_asset_id, scene_id = 0) {
        // Refused-wart compatibility shim: Unity's global Instantiate
        // silently targets the active scene, which is the multi-scene wart
        // cairns refuses (§4 of the rename plan). Use Scene.instantiate
        // (a method on a specific Scene handle) instead. Kept for one
        // release for legacy call sites; lowers to cairns.scene.instantiate
        // with the active scene's NDJSON-side id.
        const r = cairns.dispatch("cairns.scene.instantiate", {
            scene: scene_id, prefab: prefab_or_asset_id,
        });
        if (!r.ok) {
            throw new Error("InstantiateAsync: " +
                (r.error && r.error.message ? r.error.message : "?"));
        }
        return new GameObject(r.result.entity, scene_id);
    }

    // #229 C4.2/C4.3: lower onto the generic component ops (was a JS-only Map).
    // props optional (type-specific; see cairns.entity.componentTypes). Returns
    // a Component handle caching the engine-side data.
    AddComponent(typeName, props) {
        cairns.dispatch("cairns.entity.addComponent", {
            scene: this._scene, entity: this._entity,
            type: typeName, props: props || {},
        });
        let c = this._components.get(typeName);
        if (!c) {
            c = new Component(this, typeName);
            this._components.set(typeName, c);
        }
        return c;
    }
    GetComponent(typeName) {
        const r = cairns.dispatch("cairns.entity.getComponent", {
            scene: this._scene, entity: this._entity, type: typeName,
        });
        if (!r.ok || !r.result || !r.result.has) {
            return null;
        }
        let c = this._components.get(typeName);
        if (!c) {
            c = new Component(this, typeName);
            this._components.set(typeName, c);
        }
        c._data = r.result;
        return c;
    }
    RemoveComponent(typeName) {
        cairns.dispatch("cairns.entity.removeComponent", {
            scene: this._scene, entity: this._entity, type: typeName,
        });
        this._components.delete(typeName);
    }

    SetActive(active) { this._active = !!active; }
    get activeSelf()  { return this._active; }
    get entity()      { return this._entity; }
    get scene()       { return this._scene; }
    // Legacy alias (one release).
    get world()       { return this._scene; }

    // #229 C4.3: lower the lifecycle/name/find surface onto the entity ops.
    Destroy() {
        cairns.dispatch("cairns.entity.destroy",
                        { scene: this._scene, entity: this._entity });
    }
    get name()  { return this._name; }
    set name(n) {
        this._name = n;
        cairns.dispatch("cairns.entity.setName",
                        { scene: this._scene, entity: this._entity, name: n });
    }
    // Unity GameObject.Find, but explicit-scene ([N-node]); default scene 0.
    static Find(name, scene = 0) {
        const r = cairns.dispatch("cairns.entity.find",
                                  { scene: scene, name: name });
        if (r.ok && r.result && r.result.found) {
            return new GameObject(r.result.entity, scene);
        }
        return null;
    }
}

class Component {
    constructor(go, typeName) {
        this._go = go;
        this._type = typeName;
        this.enabled = true;
    }
    get gameObject() { return this._go; }
    get transform()  { return this._go.transform; }
    get type()       { return this._type; }
}

// #229 C4.2: Unity Time, lowered onto cairns.time.get (deterministic sim
// clock, fixed timestep). Read each access -- no client-side caching.
const Time = {
    _snap() {
        const r = cairns.dispatch("cairns.time.get", {});
        return r.ok && r.result ? r.result : { time: 0, dt: 0, frame: 0 };
    },
    get time()       { return this._snap().time; },
    get deltaTime()  { return this._snap().dt; },
    get frameCount() { return this._snap().frame; },
};

// =====================================================================
// Static surfaces. Camera.main is loud-strict (Refinement 1): no
// silent return-first-or-tagged. Cairns.onFrame is reserved so it
// doesn't get back-named to a refused MonoBehaviour-style Update().
// =====================================================================

const Camera = {
    get main() {
        // TODO: query cairns.viewport.list (or equivalent) for camera
        // components. Today: nothing registered -> always throw.
        const cameras = [];
        if (cameras.length === 0) {
            throw new Error(
                "Camera.main: no Camera component exists. " +
                "Add one via gameObject.AddComponent('Camera') first.");
        }
        if (cameras.length > 1) {
            throw new Error(
                "Camera.main: multiple cameras (" + cameras.length +
                "). Use cairns.viewport.list to disambiguate.");
        }
        return cameras[0];
    },
};

const Application = {
    Quit() { return cairns.dispatch("cairns.app.quit", {}); },
};

const Cairns = {
    onFrame(_cb) {
        throw new Error(
            "Cairns.onFrame: events.frame channel not yet wired. " +
            "Surface reserved; lands when the event/subscribe channel ships.");
    },
    // #224 L3: the loading-system instrument.
    loader: {
        trace()    { const r = cairns.dispatch("cairns.loader.trace", {});
                     return r && r.ok ? r.result : null; },
        counters() { const r = cairns.dispatch("cairns.loader.counters", {});
                     return r && r.ok ? r.result : null; },
    },
};

// =====================================================================
// #225 R4: Unity-shaped Prefab + Scene + Editor surface.
//
//   Prefab  = a loaded GLB (Unity's Instantiate target).
//   Scene   = a container of GameObjects (Unity Scene == cairns container).
//   Prefabs = Resources.Load-shaped sync loaders, returns Prefab[].
//   Editor  = the multi-scene compositor. A NEW NOUN Unity has no word for
//             (a viewport whose pixels are a shader function of other
//             viewports' targets). New names because there is no Unity
//             concept to clone, not to signal a divergence.
//
// Instantiate is a METHOD ON A SCENE -- there is no global Instantiate()
// (the active-scene Unity wart is the one Unity feature cairns refuses;
// see plan §4 "Refuse" bin). Every Instantiate() carries an explicit
// scene target.
// =====================================================================

function _dispatchOk(op, args, fnName) {
    const r = cairns.dispatch(op, args || {});
    if (!r || r.ok === false) {
        const msg = (r && r.error && r.error.message) ? r.error.message : "?";
        throw new Error(fnName + ": " + msg);
    }
    return (r && r.result !== undefined) ? r.result : r;
}

class Prefab {
    constructor(id, name, extent) {
        this.id = id;
        this.name = name || ("prefab" + id);
        this.extent = +extent || 0.0;
    }
}

class Scene {
    constructor(id) { this.id = id; }

    // Unity: Object.Instantiate(prefab, position, rotation) -- but BOUND
    // to THIS scene (no implicit active scene). Returns a GameObject.
    // position is a Vector3 or undefined (default 0,0,-3 today).
    // rotation is a Quaternion or undefined (default identity).
    instantiate(prefab, position, rotation) {
        const args = { scene: this.id, prefab: prefab && prefab.id };
        if (position) {
            args.x = position.x; args.y = position.y; args.z = position.z;
        }
        if (rotation) {
            args.rotation = rotation.toArray();
        }
        const r = _dispatchOk("cairns.scene.instantiate", args,
                              "Scene.instantiate");
        return new GameObject(r.entity, this.id);
    }

    instantiateGrid(prefabs, viewport) {
        const r = _dispatchOk("cairns.scene.instantiateGrid", {
            scene: this.id,
            count: prefabs ? prefabs.length : 0,
            prefabs: prefabs ? prefabs.map(p => p.id) : [],
            fit_viewport: viewport ? viewport.id : 0,
        }, "Scene.instantiateGrid");
        return (r.entities || []).map(e => new GameObject(e, this.id));
    }

    clear() {
        return _dispatchOk("cairns.scene.clear", { scene: this.id },
                           "Scene.clear");
    }

    // Camera entities: per the plan worked example (§4b), cameras are
    // entities IN the scene. Stub today (#226 CAP-1 wires up real cameras
    // as entities); throws loud so call sites don't silently succeed.
    addCamera(/*lookAtOrPose*/) {
        throw new Error(
            "Scene.addCamera: per-scene camera entities not wired yet (#226 CAP-1)");
    }
}

const Prefabs = {
    // Unity Resources.Load shape: SYNC main-thread load. Returns Prefab[].
    // `source` is a logical label (today: ignored; just returns Prefabs
    // with sequential ids out of the pre-loaded GLB pool from cairns init).
    load(source, count, cursor = 0) {
        const r = _dispatchOk("cairns.prefab.loadBatch", {
            source, cursor, count,
        }, "Prefabs.load");
        return (r.prefabs || []).map(
            p => new Prefab(p.id, p.name, p.extent));
    },
    loadOne(source, index) {
        return Prefabs.load(source, 1, index)[0];
    },

    // Snapshot of the pre-loaded Prefab pool (cairns init populated this
    // from kDebugGlbs[]). Useful while cairns.prefab.loadBatch is still
    // a stub: lets a script enumerate what's already there.
    list() {
        const count = _dispatchOk(
            "cairns.prefab.count", {}, "Prefabs.list").count || 0;
        const out = [];
        for (let i = 0; i < count; i++) {
            const dims = _dispatchOk(
                "cairns.prefab.dims", { prefab: i }, "Prefabs.list");
            out.push(new Prefab(i, "prefab" + i, dims.extent_max));
        }
        return out;
    },
};

const Editor = {
    scenes: [],

    // NOT SceneManager.LoadScene (single-active model is refused).
    // Each Editor.newScene returns a fresh Scene handle; they are all
    // simultaneously live, instantiable, and viewport-bindable.
    newScene() {
        const r = _dispatchOk("cairns.scene.create", {},
                              "Editor.newScene");
        const s = new Scene(r.scene);
        Editor.scenes.push(s);
        return s;
    },

    // Bind a scene to a viewport. With multiple Editor.show() calls each
    // routing a different Scene to a different viewport, the engine
    // composes them into one swap image -- the multi-scene compositor.
    show(scene, viewport) {
        return _dispatchOk("cairns.viewport.setScene", {
            viewport: viewport && viewport.id,
            scene: scene && scene.id,
        }, "Editor.show");
    },

    // #225 R6 stubs reserving #226 capabilities. Loud throws so the
    // Unity prior (active scene; SceneManager.LoadScene) never fires
    // here and nothing silently half-works (the Cairns.onFrame precedent).
    thumbnails(/*scene, opts*/) {
        throw new Error(
            "Editor.thumbnails: virtualized RT pool not wired yet (#226 CAP-2)");
    },
    compose(/*scene, viewport, opts*/) {
        throw new Error(
            "Editor.compose: multi-target composition pass not wired yet (#226 CAP-3)");
    },
};

// =====================================================================
// Publish to global. script.eval snippets see all of these by name.
// =====================================================================
globalThis.Vector3    = Vector3;
globalThis.Vector2    = Vector2;
globalThis.Quaternion = Quaternion;
globalThis.Mathf      = Mathf;
globalThis.Transform  = Transform;
globalThis.GameObject = GameObject;
globalThis.Component  = Component;
globalThis.Camera     = Camera;
globalThis.Application= Application;
globalThis.Cairns     = Cairns;
// #225 R4: Unity-shaped Prefab/Scene/Prefabs + the non-Unity Editor.
globalThis.Prefab     = Prefab;
globalThis.Scene      = Scene;
globalThis.Prefabs    = Prefabs;
globalThis.Editor     = Editor;
)JS";

}  // namespace cairns::control
