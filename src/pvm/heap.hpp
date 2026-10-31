// Managed heap and garbage collector for the PVM. See docs/architecture.md ("Garbage collection").
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "value.hpp"

namespace pvm {

// A heap object: a growable array of values, or a map. `next` links all live objects for the sweep
// phase. `type` is the full runtime type (array<T> or map<K,V>), used by obj type tests and dynamic
// operations.
//
// A map keeps its entries in insertion order: `keys[i]` / `items[i]` is entry i, `alive[i]` is false
// for a removed entry (compacted away once dead entries outnumber live ones), and `index` finds an
// entry by the canonical text of its key. Keys are always scalars, so they never refer to the heap.
struct Obj {
    Obj* next = nullptr;
    bool marked = false;
    std::shared_ptr<const Type> type;
    std::vector<Value> items;
    // Map only
    std::vector<Value> keys;
    std::vector<char> alive;
    std::unordered_map<std::string, size_t> index;
    size_t live = 0;
    // Closure only (type kind Fn): the function index; `items` holds the captured values.
    int func = -1;

    bool isMap() const { return type && type->kind == Type::Map; }
};

struct HeapError {
    std::string message;
};

// The collector asks the interpreter for its roots: every value that is reachable without going
// through the heap (operand stack, frame locals). The visitor must be called once per root value.
using MarkFn = const std::function<void(const Value&)>&;
using RootsFn = std::function<void(MarkFn)>;

struct HeapOptions {
    size_t maxElements = 8000000;  // limit on live array slots across the whole heap
    bool stress = false;           // collect before every allocation (finds missing roots)
};

// Precise, non-moving, stop-the-world mark-and-sweep collector.
//
//  * Roots: operand stack and frame locals (supplied by the interpreter through RootsFn).
//    Hosts never hold references: host functions receive and return only scalars and strings.
//  * Reachability: an object is live if a root refers to it, or a live array holds it. Marking
//    uses an explicit work list, so deeply nested arrays cannot overflow the native stack.
//  * Collection runs only inside allocate()/reserve(), i.e. at instructions that create or grow
//    objects, never in the middle of one. The interpreter keeps operands on its stack until after
//    the allocation, so operands stay rooted.
//  * Trigger: when the number of objects or slots has doubled since the last collection (with a
//    floor), or on every allocation in stress mode.
//  * Native resources: heap objects own only memory (vectors of Values); there are no finalizers.
class Heap {
public:
    explicit Heap(HeapOptions options = {}) : options_(options) {}
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // Creates an array object of the given type with `count` slots, each a copy of `fill`.
    Obj* allocate(const std::shared_ptr<const Type>& type, size_t count, const Value& fill, const RootsFn& roots);

    // Creates an empty map object of the given type.
    Obj* allocateMap(const std::shared_ptr<const Type>& type, const RootsFn& roots);

    // Removes dead entries from a map (after removals) and fixes the accounting.
    void compactMap(Obj* map);

    // Accounts for `count` more slots in an existing array (push). May collect; throws HeapError
    // when the heap limit would be exceeded even after collecting.
    void reserve(size_t count, const RootsFn& roots);

    size_t collections() const { return collections_; }
    size_t freedObjects() const { return freed_; }
    size_t liveObjects() const { return objects_; }
    size_t liveElements() const { return elements_; }

private:
    HeapOptions options_;
    Obj* head_ = nullptr;
    size_t objects_ = 0;
    size_t elements_ = 0;
    size_t objectTrigger_ = 1024;
    size_t elementTrigger_ = 1u << 20;
    size_t collections_ = 0;
    size_t freed_ = 0;

    void collect(const RootsFn& roots);
    void ensureRoom(size_t count, const RootsFn& roots);
};

} // namespace pvm
