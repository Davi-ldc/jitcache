// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-image.md T1 and T2: property accesses. Their slow paths near-call the IC slow-path thunk of each access type
// (census D1): get, put, in and delete by id and by value, get_length, get_by_id_with_this and get_by_val_with_this
// through super, private names, accessors and brands, and instanceof. Object literals with accessors pass their names'
// impls to put_getter_by_id, put_setter_by_id and put_getter_setter_by_id (D2, UCBIdentifier); regexp literals pass
// their RegExp cells (C21, UCBConstantCell); stores of cells go through the write barrier (A4, D7); object literals and
// constructors fence on ARM64 (A5, C2, C22); typeof, == null and != null as values and as branches, and super's
// get_prototype_of decode structure IDs (A6, C27). Gets and puts on a proxy cache cases that call the ProxyHelpers
// builtins, which run often enough to compile, so their @mustValidateResultOfProxyGetAndSetTraps emits
// has_structure_with_flags, the one opcode of row C27 only builtins reach.
// The consumer checks that the images of the functions it names carry (T1): an IC slow-path call for each access type
// their caches use (D1); the accessors' identifiers (D2); the regexp cells (C21); the barrier threshold and the VM the
// write barriers and new_object's slow path pass (A4, D5, D7, C3); on ARM64 the mutator fence flag (A5, C2, C22); and
// the structure ID base (A6, C27).
(function main(role, scratch, artifact, sequence) {
    class Point {
        #secret;
        static #created = 0;
        constructor(x, y) {
            this.x = x;
            this.y = y;
            this.#secret = x * 10 + y;
            ++Point.#created;
        }
        get sum() {
            return this.x + this.y;
        }
        set both(value) {
            this.x = value;
            this.y = value;
        }
        #hidden() {
            return this.#secret + 1;
        }
        get #doubled() {
            return this.#secret * 2;
        }
        reveal() {
            return this.#hidden() + this.#doubled;
        }
        static isPoint(value) {
            return #secret in value;
        }
        static hasBrand(value) {
            return #hidden in value;
        }
        static created() {
            return Point.#created;
        }
    }
    class Point3D extends Point {
        constructor(x, y, z) {
            super(x, y);
            this.z = z;
        }
        get sum() {
            return super.sum + this.z;
        }
        inherited(name) {
            return typeof super[name];
        }
    }
    class Shape {
        static [Symbol.hasInstance](value) {
            return typeof value === "object" && value !== null && "sides" in value;
        }
    }

    function accessors(value) {
        let stored = value;
        return {
            get both() {
                return stored;
            },
            set both(next) {
                stored = next;
            },
            get onlyGetter() {
                return stored * 2;
            },
            set onlySetter(next) {
                stored = next + 1;
            },
            plain: value,
        };
    }
    function useAccessors(i) {
        const object = accessors(i);
        object.both = object.both + 1;
        object.onlySetter = object.onlyGetter;
        return object.both + object.plain;
    }

    function matches(text) {
        return /jit(cache)+/g.test(text) ? text.replace(/a/g, "A").length : -text.length;
    }

    function lengths(array, text) {
        return array.length + text.length;
    }

    function indexed(array, index, value) {
        array[index] = value;
        return array[index] + (array[index - 1] ?? 0);
    }

    function membership(object, key) {
        const hadKey = key in object;
        const hadX = "x" in object;
        delete object.temporary;
        delete object[key];
        return (hadKey ? 1 : 0) + (hadX ? 2 : 0) + (key in object ? 4 : 0);
    }

    function link(parent, child) {
        parent.next = child;
        parent.children = [child, { child }];
        parent[0] = child;
        return parent;
    }

    function shapes(value) {
        let result = 0;
        if (typeof value === "undefined")
            result += 1;
        if (value == null)
            result += 2;
        if (value != null && Object.getPrototypeOf(value) === Point3D.prototype)
            result += 4;
        const nullish = value == null;
        const present = value != null;
        return result + (nullish ? 8 : 0) + (present ? 16 : 0);
    }

    function kinds(value) {
        return (value instanceof Point ? 1 : 0) + (value instanceof Point3D ? 2 : 0) + (value instanceof Shape ? 4 : 0) + (value instanceof Array ? 8 : 0);
    }

    const handler = {
        get(target, key) {
            return key in target ? target[key] * 2 : 0;
        },
        set(target, key, value) {
            target[key] = value + 1;
            return true;
        },
    };
    const proxied = new Proxy({ a: 1, b: 2, c: 3 }, handler);
    function viaProxy(i) {
        proxied.a = i;
        proxied.b = i + 1;
        proxied.c = i + 2;
        return proxied.a + proxied.b + proxied.c;
    }

    let total = 0;
    let text = "";
    const array = [0, 1, 2, 3];
    for (let i = 0; i < 80; ++i) {
        const point = i % 2 ? new Point(i, 2) : new Point3D(i, 2, 3);
        point.both = point.sum;
        total += point.sum + point.reveal() + (Point.isPoint(point) ? 1 : 0) + (Point.isPoint({ }) ? 100 : 0);
        total += (Point.hasBrand(point) ? 1000 : 0) + (Point.hasBrand(array) ? 10000 : 0);
        total += viaProxy(i);
        if (point instanceof Point3D)
            text += point.inherited(i % 3 ? "reveal" : "missing")[0];
        total += useAccessors(i);
        total += matches(i % 3 ? "jitcachecache-banana" : "plain-banana");
        total += lengths(array, "length " + i);
        total += indexed(array, (i % 3) + 1, i);
        const object = { x: i, temporary: true, [`key${i % 4}`]: i };
        total += membership(object, `key${i % 5}`);
        total += link({ id: i }, { id: i + 1 }).children.length;
        total += shapes([undefined, null, point, { }][i % 4]);
        total += kinds([point, { sides: 3 }, [], null][i % 4]);
    }
    print(total);
    print(text);
    print(Point.created());

    // The case happened (HARNESS.md, Tests): every function below runs at least 40 times, so the producer compiled it and
    // it ran its import here. Point3D's constructor and inherited run for every other point, which is why the loop runs 80
    // times. The private methods and the object literal's accessors are compiled too, but no reference to them outlives
    // their calls.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const accessor = (object, name) => Object.getOwnPropertyDescriptor(object, name);
        const bodies = {
            isPoint: Point.isPoint, hasBrand: Point.hasBrand, reveal: Point.prototype.reveal, inherited: Point3D.prototype.inherited,
            "Point's sum getter": accessor(Point.prototype, "sum").get, "Point3D's sum getter": accessor(Point3D.prototype, "sum").get,
            "Point's both setter": accessor(Point.prototype, "both").set, "Shape's hasInstance": Shape[Symbol.hasInstance],
            "the proxy's get trap": handler.get, "the proxy's set trap": handler.set,
            accessors, useAccessors, matches, lengths, indexed, membership, link, shapes, kinds, viaProxy,
        };
        const constructors = { Point, Point3D };
        sections.expectImports(role, bodies);
        sections.expectImports(role, constructors, "construct");
        sections.expectTargets(role, { call: bodies, construct: constructors }, [
            { site: "D1", kind: "InlineCacheSlowPathThunk", form: "Call", eachMoldAccessType: true },
            { site: "D2", kind: "UCBIdentifier", form: "Pointer" },
            { site: "C21", kind: "UCBConstantCell", form: "Pointer" },
            { site: "A4, D5, D7", kind: "VMAddress", a: "BarrierThreshold", form: "Pointer" },
            { site: "D7, C3", kind: "VMAddress", a: "VM", form: "Pointer" },
            { site: "A5, C2, C22", kind: "VMAddress", a: "MutatorShouldBeFenced", form: "Pointer", arch: "arm64" },
            { site: "A6, C27", kind: "StructureIDBase", form: "Pointer" },
        ]);
    }
})(...arguments);
