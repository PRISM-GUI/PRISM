// yffi's C ABI: the static archive cargo builds, and one scalar round-tripping through it. If
// libyrs.h and the crate it declares disagree, or the archive is missing a Rust runtime symbol,
// this is where it shows before any of prism::crdt exists.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

// libyrs.h is a C header without an extern "C" guard: included bare from C++, its declarations
// would get C++ linkage and never meet the C symbols the archive exports.
extern "C" {
#include <libyrs.h>
}

TEST_CASE("yrs links") {
  YDoc* doc = ydoc_new();
  REQUIRE(doc != nullptr);

  Branch* map = ymap(doc, "m");
  REQUIRE(map != nullptr);

  YTransaction* write = ydoc_write_transaction(doc, 0, nullptr);
  REQUIRE(write != nullptr);
  YInput value = yinput_float(1.5);
  ymap_insert(map, write, "k", &value);
  ytransaction_commit(write);

  YTransaction* read = ydoc_read_transaction(doc);
  REQUIRE(read != nullptr);
  YOutput* stored = ymap_get(map, read, "k");
  REQUIRE(stored != nullptr);
  const double* number = youtput_read_float(stored);
  REQUIRE(number != nullptr);
  CHECK(*number == 1.5);
  youtput_destroy(stored);
  ytransaction_commit(read);

  ydoc_destroy(doc);
}
