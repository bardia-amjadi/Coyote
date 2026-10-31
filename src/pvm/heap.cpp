#include "heap.hpp"
#include "ops.hpp"

#include <algorithm>

namespace pvm {

Heap::~Heap() {
    while (head_) {
        Obj* next = head_->next;
        delete head_;
        head_ = next;
    }
}

void Heap::collect(const RootsFn& roots) {
    ++collections_;

    // Mark: explicit work list.
    std::vector<Obj*> work;
    auto mark = [&](const Value& v) {
        const Value* p = &v;
        if (p->kind == Type::Obj) {  // boxed values hold the reference
            p = p->boxed.get();
            if (!p) return;
        }
        if ((p->kind == Type::Array || p->kind == Type::Map || p->kind == Type::Class || p->kind == Type::Fn) && p->ref && !p->ref->marked) {
            p->ref->marked = true;
            work.push_back(p->ref);
        }
    };
    roots(mark);
    while (!work.empty()) {
        Obj* o = work.back();
        work.pop_back();
        for (const Value& v : o->items) mark(v);
    }

    // Sweep: free every unmarked object, clear marks on survivors.
    Obj** link = &head_;
    while (*link) {
        Obj* o = *link;
        if (o->marked) {
            o->marked = false;
            link = &o->next;
        } else {
            *link = o->next;
            elements_ -= o->items.size();
            --objects_;
            ++freed_;
            delete o;
        }
    }

    objectTrigger_ = std::max<size_t>(1024, objects_ * 2);
    elementTrigger_ = std::max<size_t>(1u << 20, elements_ * 2);
}

void Heap::ensureRoom(size_t count, const RootsFn& roots) {
    if (count > options_.maxElements || elements_ + count > options_.maxElements) {
        collect(roots);
        if (count > options_.maxElements || elements_ + count > options_.maxElements)
            throw HeapError{"out of memory: the heap limit of " + std::to_string(options_.maxElements) +
                            " array elements would be exceeded"};
    }
}

Obj* Heap::allocate(const std::shared_ptr<const Type>& type, size_t count, const Value& fill, const RootsFn& roots) {
    if (options_.stress || objects_ >= objectTrigger_ || elements_ >= elementTrigger_) collect(roots);
    ensureRoom(count, roots);
    Obj* o = new Obj;
    o->type = type;
    o->items.assign(count, fill);
    o->next = head_;
    head_ = o;
    ++objects_;
    elements_ += count;
    return o;
}

Obj* Heap::allocateMap(const std::shared_ptr<const Type>& type, const RootsFn& roots) {
    return allocate(type, 0, Value::none(), roots);
}

void Heap::compactMap(Obj* m) {
    size_t out = 0;
    for (size_t i = 0; i < m->items.size(); ++i) {
        if (!m->alive[i]) continue;
        if (out != i) {
            m->items[out] = std::move(m->items[i]);
            m->keys[out] = std::move(m->keys[i]);
        }
        m->alive[out] = 1;
        ++out;
    }
    elements_ -= m->items.size() - out;
    m->items.resize(out);
    m->keys.resize(out);
    m->alive.resize(out);
    m->index.clear();
    for (size_t i = 0; i < out; ++i) m->index.emplace(mapKeyText(m->keys[i]), i);
}

void Heap::reserve(size_t count, const RootsFn& roots) {
    if (options_.stress) collect(roots);
    ensureRoom(count, roots);
    elements_ += count;
}

} // namespace pvm
