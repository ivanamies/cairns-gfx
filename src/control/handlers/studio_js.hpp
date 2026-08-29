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
// id (returned by cairns.world.instantiate). Transform setters store
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
    get position()   { return this._position; }
    set position(v)  { this._position = v; }  // TODO lower to cairns.entity.setTransform
    get rotation()   { return this._rotation; }
    set rotation(q)  { this._rotation = q; }
    get localScale() { return this._localScale; }
    set localScale(v){ this._localScale = v; }
    LookAt(_target)  { /* TODO */ }
    Translate(v)     { this._position = this._position.add(v); }
}

class GameObject {
    constructor(entity_id, world_id) {
        this._entity = entity_id;
        this._world = world_id;
        this.transform = new Transform(this);
        this._components = new Map();
        this._active = true;
    }

    static InstantiateAsync(asset_id, world_id = 0) {
        // The `Async` suffix + caller's `await` preserves the SYNTACTIC
        // divergence (Refinement 1) that makes spawn+upload's eventual
        // asynchrony visible. Today the body is sync and returns the
        // GameObject directly -- `await` on a non-Promise resolves to
        // the value immediately, so call sites don't change. When real
        // async upload lands this returns a Promise and the `await` does
        // genuine work. Deliberately NOT a Promise today: QuickJS's
        // promise machinery leaves shutdown state that trips an assert at
        // runtime free time (see studio_notes.md "QuickJS async
        // limitation").
        const r = cairns.dispatch("cairns.world.instantiate", {
            world: world_id, asset: asset_id,
        });
        if (!r.ok) {
            throw new Error("InstantiateAsync: " +
                (r.error && r.error.message ? r.error.message : "?"));
        }
        return new GameObject(r.result.entity, world_id);
    }

    AddComponent(typeName) {
        let c = this._components.get(typeName);
        if (!c) {
            c = new Component(this, typeName);
            this._components.set(typeName, c);
        }
        return c;
    }
    GetComponent(typeName) { return this._components.get(typeName) || null; }
    RemoveComponent(typeName) { this._components.delete(typeName); }

    SetActive(active) { this._active = !!active; }
    get activeSelf()  { return this._active; }
    get entity()      { return this._entity; }
    get world()       { return this._world; }
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
)JS";

}  // namespace cairns::control
