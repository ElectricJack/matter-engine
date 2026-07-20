#include "../selection_set.h"
#include <cassert>
#include <cstdio>

using viewer::SelectionSet;
using viewer::SelectedObject;

void test_replace() {
    SelectionSet sel;
    SelectedObject a{SelectedObject::Entity, 1};
    SelectedObject b{SelectedObject::Entity, 2};
    sel.replace(a);
    assert(sel.size() == 1);
    assert(sel.contains(a));
    assert(sel.primary() && *sel.primary() == a);
    sel.replace(b);
    assert(sel.size() == 1);
    assert(!sel.contains(a));
    assert(sel.contains(b));
    std::printf("  replace: OK\n");
}

void test_toggle() {
    SelectionSet sel;
    SelectedObject a{SelectedObject::Entity, 1};
    SelectedObject b{SelectedObject::Entity, 2};
    sel.toggle(a);
    assert(sel.size() == 1);
    sel.toggle(b);
    assert(sel.size() == 2);
    sel.toggle(a);  // remove a
    assert(sel.size() == 1);
    assert(!sel.contains(a));
    assert(sel.contains(b));
    std::printf("  toggle: OK\n");
}

void test_range() {
    SelectionSet sel;
    std::vector<SelectedObject> order;
    for (uint64_t i = 1; i <= 5; ++i)
        order.push_back({SelectedObject::Entity, i});
    sel.replace(order[1]);  // select id=2 as primary
    sel.extend_range(order[3], order);  // extend to id=4
    assert(sel.size() == 3);  // 2, 3, 4
    assert(sel.contains(order[1]));
    assert(sel.contains(order[2]));
    assert(sel.contains(order[3]));
    assert(!sel.contains(order[0]));
    assert(!sel.contains(order[4]));
    std::printf("  range: OK\n");
}

void test_validate() {
    SelectionSet sel;
    SelectedObject a{SelectedObject::Entity, 1};
    SelectedObject b{SelectedObject::Entity, 2};
    SelectedObject c{SelectedObject::Entity, 3};
    sel.replace(a);
    sel.toggle(b);
    sel.toggle(c);
    // Remove b (id=2)
    sel.validate([](const SelectedObject& o) { return o.id != 2; });
    assert(sel.size() == 2);
    assert(!sel.contains(b));
    assert(sel.contains(a));
    assert(sel.contains(c));
    std::printf("  validate: OK\n");
}

void test_clear() {
    SelectionSet sel;
    sel.replace({SelectedObject::Entity, 1});
    sel.toggle({SelectedObject::Entity, 2});
    sel.clear();
    assert(sel.empty());
    assert(sel.primary() == nullptr);
    std::printf("  clear: OK\n");
}

void test_mixed_kinds() {
    SelectionSet sel;
    SelectedObject entity{SelectedObject::Entity, 42};
    SelectedObject root{SelectedObject::BakedRoot, 42};
    sel.replace(entity);
    sel.toggle(root);
    assert(sel.size() == 2);  // same id but different kind
    assert(sel.contains(entity));
    assert(sel.contains(root));
    std::printf("  mixed_kinds: OK\n");
}

void test_primary_after_toggle_remove_primary() {
    SelectionSet sel;
    SelectedObject a{SelectedObject::Entity, 1};
    SelectedObject b{SelectedObject::Entity, 2};
    sel.replace(a);
    sel.toggle(b);
    assert(sel.primary() && *sel.primary() == b);  // b is most recent
    sel.toggle(b);  // remove b (the primary)
    assert(sel.size() == 1);
    assert(sel.primary() != nullptr);  // should still have a primary (a)
    std::printf("  primary_after_toggle_remove: OK\n");
}

int main() {
    std::printf("SelectionSet tests:\n");
    test_replace();
    test_toggle();
    test_range();
    test_validate();
    test_clear();
    test_mixed_kinds();
    test_primary_after_toggle_remove_primary();
    std::printf("All SelectionSet tests passed.\n");
    return 0;
}
