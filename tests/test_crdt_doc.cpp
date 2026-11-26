// crdt::Doc: model fields travel between replicas through ycpp (doc/design/sync.md). Each test
// stands up two Docs and asks what a peer sees; the values are the plan's (Task 2, Step 1).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/crdt/doc.hpp>
#include <prism/crdt/field.hpp>
#include <prism/ui/delegate.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::crdt;

namespace {

enum class Mode { idle, run };

// Two Docs synced both ways. Keep it alive: dropping the Connections unsyncs them again.
struct Wired {
    core::Connection to_b;
    core::Connection to_a;
};

Wired wire(Doc& a, Doc& b) {
    return {
        a.on_update().connect([&b](std::span<const std::byte> update) { b.apply_update(update); }),
        b.on_update().connect([&a](std::span<const std::byte> update) { a.apply_update(update); }),
    };
}

} // namespace

TEST_CASE("set on one replica reaches the other") {
    Doc a;
    Doc b;
    Field<double> gain_a;
    Field<double> gain_b;
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);
    Wired wired = wire(a, b);

    gain_a = 2.5;

    CHECK(gain_b.get() == 2.5);
}

TEST_CASE("each supported type round-trips") {
    Doc a;
    Doc b;
    Field<int> count_a;
    Field<std::int64_t> large_a;
    Field<double> gain_a;
    Field<bool> muted_a;
    Field<std::string> name_a;
    Field<Mode> mode_a;
    Field<int> count_b;
    Field<std::int64_t> large_b;
    Field<double> gain_b;
    Field<bool> muted_b;
    Field<std::string> name_b;
    Field<Mode> mode_b;
    a.bind("count", count_a);
    b.bind("count", count_b);
    a.bind("large", large_a);
    b.bind("large", large_b);
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);
    a.bind("muted", muted_a);
    b.bind("muted", muted_b);
    a.bind("name", name_a);
    b.bind("name", name_b);
    a.bind("mode", mode_a);
    b.bind("mode", mode_b);
    Wired wired = wire(a, b);

    const std::int64_t wide = std::int64_t{1} << 60;   // beyond 2^53: travels as decimal text
    count_a = 7;
    large_a = wide;
    gain_a = 2.5;
    muted_a = true;
    name_a = "ada";
    mode_a = Mode::run;

    CHECK(count_b.get() == 7);
    CHECK(large_b.get() == wide);
    CHECK(gain_b.get() == 2.5);
    CHECK(muted_b.get() == true);
    CHECK(name_b.get() == "ada");
    CHECK(mode_b.get() == Mode::run);
}

TEST_CASE("an enum travels by name") {
    Doc a;
    Field<Mode> mode;
    a.bind("mode", mode);

    mode = Mode::run;

    CHECK(a.to_json().find("\"run\"") != std::string::npos);
}

TEST_CASE("no echo") {
    Doc a;
    Doc b;
    Field<double> gain_a;
    Field<double> gain_b;
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);
    Wired wired = wire(a, b);
    int echoed = 0;
    core::Connection count =
        b.on_update().connect([&echoed](std::span<const std::byte>) { ++echoed; });

    gain_a = 2.5;
    CHECK(gain_b.get() == 2.5);
    CHECK(echoed == 0);

    // A remote apply inside a transaction: the field's deferred change reaches the Doc when the
    // guard ends, after apply_update has returned. The map already holds that value by then, so
    // nothing is written and nothing is sent: still no echo.
    Doc origin;
    Doc mirror;
    Field<double> origin_gain;
    Field<double> mirror_gain;
    origin.bind("gain", origin_gain);
    mirror.bind("gain", mirror_gain);
    int answered = 0;
    core::Connection count_mirror =
        mirror.on_update().connect([&answered](std::span<const std::byte>) { ++answered; });
    origin_gain = 3.5;
    const auto update = origin.encode_since(mirror.state_vector());
    {
        TransactionGuard guard;
        mirror.apply_update(update);
    }
    CHECK(mirror_gain.get() == 3.5);
    CHECK(answered == 0);
}

TEST_CASE("concurrent sets converge") {
    Doc a;
    Doc b;
    Field<int> number_a;
    Field<int> number_b;
    a.bind("number", number_a);
    b.bind("number", number_b);

    number_a = 1;
    number_b = 2;
    // Both diffs are cut before either side applies anything, or the second would be empty.
    const auto from_b = b.encode_since(a.state_vector());
    const auto from_a = a.encode_since(b.state_vector());
    b.apply_update(from_a);
    a.apply_update(from_b);

    CHECK(number_a.get() == number_b.get());
}

TEST_CASE("joining loads the existing state") {
    Doc a;
    Field<double> gain_a;
    a.bind("gain", gain_a);
    gain_a = 3.0;

    // A joining replica takes the state, then binds: the map's value wins over the field's.
    Doc c;
    c.apply_update(a.encode_since(c.state_vector()));
    Field<double> gain_c;
    c.bind("gain", gain_c);
    CHECK(gain_c.get() == 3.0);

    // Same for a field that brought a value of its own.
    Doc late;
    late.apply_update(a.encode_since(late.state_vector()));
    Field<double> gain_late{42.0};
    late.bind("gain", gain_late);
    CHECK(gain_late.get() == 3.0);
}

TEST_CASE("a joining peer never overwrites with its default") {
    Doc a;
    Field<double> gain_a;
    a.bind("gain", gain_a);
    gain_a = 3.0;

    // c binds before it has any state: binding writes nothing, so its default 0 never competes with
    // a's 3, and catching up loads 3 over it.
    Doc c;
    Field<double> gain_c;
    c.bind("gain", gain_c);
    CHECK(gain_c.get() == 0.0);

    const auto from_a = a.encode_since(c.state_vector());
    const auto from_c = c.encode_since(a.state_vector());
    c.apply_update(from_a);
    a.apply_update(from_c);

    CHECK(gain_c.get() == 3.0);
    CHECK(gain_a.get() == 3.0);
}

TEST_CASE("wrong type from a peer is ignored") {
    Doc numbers;
    Field<double> gain;
    numbers.bind("gain", gain);
    gain = 1.5;

    Doc texts;
    Field<std::string> text;
    texts.apply_update(numbers.encode_since(texts.state_vector()));
    texts.bind("gain", text);
    text = "loud";

    const auto louder = texts.encode_since(numbers.state_vector());
    CHECK_NOTHROW(numbers.apply_update(louder));

    CHECK(gain.get() == 1.5);
}

TEST_CASE("equal set emits nothing") {
    Doc a;
    Doc b;
    Field<double> gain_a;
    Field<double> gain_b;
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);
    Wired wired = wire(a, b);
    gain_a = 2.5;
    CHECK(gain_b.get() == 2.5);

    int emitted = 0;
    core::Connection count = a.on_update().connect([&emitted](std::span<const std::byte>) {
        ++emitted;
    });
    gain_a = 2.5;

    CHECK(emitted == 0);
}

TEST_CASE("destruction in either order") {
    Field<double> survivor;
    {
        Doc doc;
        doc.bind("gain", survivor);
        survivor = 1.0;
    }
    survivor = 2.0;   // the Doc is gone; the field is a plain core::Field again
    CHECK(survivor.get() == 2.0);

    Doc doc;
    {
        Field<double> transient;
        doc.bind("gain", transient);
        transient = 3.0;
    }
    // The Doc outlives its field: a real update touching that key must not reach the dead field.
    Doc other;
    Field<double> elsewhere;
    other.bind("gain", elsewhere);
    elsewhere = 4.0;
    doc.apply_update(other.encode_since(doc.state_vector()));
}

TEST_CASE("an unbound crdt::Field behaves as core::Field") {
    Field<int> count;
    int observed = 0;
    core::Connection watching =
        count.on_change().connect([&observed](const int&) { ++observed; });

    count = 2;
    count += 3;
    ++count;

    CHECK(count.get() == 6);
    CHECK(observed == 3);
}

TEST_CASE("two replicas can each write a different member of one nested struct") {
    // Unwired, each replica writes a different member of the same nested struct. Under a nested
    // map each replica would create its own map for "in" and one of the two would lose the merge.
    // A dotted key is a plain key of the root map, so both writes survive it.
    Doc a;
    Doc b;
    Field<bool> on_a;
    Field<int> level_a;
    Field<bool> on_b;
    Field<int> level_b;
    a.map("in").bind("on", on_a);
    a.map("in").bind("level", level_a);
    b.map("in").bind("on", on_b);
    b.map("in").bind("level", level_b);

    on_a = true;
    level_b = 5;

    const auto from_a = a.encode_since(b.state_vector());
    const auto from_b = b.encode_since(a.state_vector());
    a.apply_update(from_b);
    b.apply_update(from_a);

    CHECK(on_a.get() == true);
    CHECK(on_b.get() == true);
    CHECK(level_a.get() == 5);
    CHECK(level_b.get() == 5);
}

TEST_CASE("an integer travels as a Yjs number") {
    // A Yjs number is a double, and a JS peer reads a 64-bit long as a BigInt: so a whole number a
    // double holds exactly leaves as a number, and a wider one as decimal text.
    CHECK(std::get<double>(value_detail::to_value(7)) == 7.0);
    CHECK(std::get<std::string>(value_detail::to_value(std::int64_t{1} << 60)) ==
          "1152921504606846976");

    // Reading back: an exact whole number converts, a fraction and one beyond 2^53 do not.
    int count = 0;
    CHECK(value_detail::from_value(Value{7.0}, count));
    CHECK(count == 7);
    CHECK_FALSE(value_detail::from_value(Value{2.5}, count));
    CHECK(count == 7);
    CHECK_FALSE(value_detail::from_value(Value{1e300}, count));
    CHECK(count == 7);
}

TEST_CASE("a widget shares its value and keeps its own settings") {
    // A type with a `value` member of a shared scalar travels through that member alone: two
    // replicas of one slider may be laid out differently and still show the same number.
    Doc a;
    Doc b;
    Field<ui::Slider<double>> gain_a{
        ui::Slider<double>{.value = 0.5, .min = 0.0, .max = 1.0, .step = 0.01, .label = "Gain"}};
    Field<ui::Slider<double>> gain_b{
        ui::Slider<double>{.value = 0.5, .min = 0.0, .max = 10.0, .step = 0.5, .label = "Volume"}};
    a.bind("gain", gain_a);
    b.bind("gain", gain_b);
    Wired wired = wire(a, b);

    gain_a.update([](ui::Slider<double>& s) { s.value = 0.8; });

    CHECK(gain_b.get().value == 0.8);
    CHECK(gain_b.get().min == 0.0);
    CHECK(gain_b.get().max == 10.0);
    CHECK(gain_b.get().step == 0.5);
    CHECK(gain_b.get().label == "Volume");
    CHECK(gain_a.get().label == "Gain");
}

TEST_CASE("a plain number from a peer reaches a widget's value") {
    // What pycrdt writes into the key is a number, not this replica's widget: the widget reads it
    // through its value member and keeps its own local range.
    Doc widgets;
    Field<ui::Slider<double>> gain{
        ui::Slider<double>{.value = 0.0, .min = 0.0, .max = 1.0, .label = "Gain"}};
    widgets.bind("gain", gain);

    Doc peer;
    Field<double> plain;
    peer.bind("gain", plain);
    plain = 0.75;

    widgets.apply_update(peer.encode_since(widgets.state_vector()));

    CHECK(gain.get().value == 0.75);
    CHECK(gain.get().label == "Gain");
}

TEST_CASE("a dropdown's enum travels by name") {
    Doc a;
    Doc b;
    Field<ui::Dropdown<Mode>> mode_a{ui::Dropdown<Mode>{.labels = {"idle", "run"}}};
    Field<ui::Dropdown<Mode>> mode_b;
    a.bind("mode", mode_a);
    b.bind("mode", mode_b);
    Wired wired = wire(a, b);

    mode_a.update([](ui::Dropdown<Mode>& d) { d.value = Mode::run; });

    CHECK(mode_b.get().value == Mode::run);
    CHECK(mode_b.get().labels.empty());   // the labels are this replica's own
    CHECK(a.to_json().find("\"run\"") != std::string::npos);
}

TEST_CASE("a widget reaches a peer as a plain number") {
    Doc doc;
    Field<ui::Slider<double>> gain{
        ui::Slider<double>{.value = 0.0, .min = 0.0, .max = 1.0, .step = 0.01, .label = "Gain"}};
    doc.bind("gain", gain);

    gain.update([](ui::Slider<double>& s) { s.value = 0.8; });

    const std::string json = doc.to_json();
    CHECK(json.find("\"gain\":0.8") != std::string::npos);
    CHECK(json.find("\"min\"") == std::string::npos);
    CHECK(json.find("\"label\"") == std::string::npos);
}
