// One model body, written the way a user writes it: everything goes through the `model` alias.
// Included twice, once per flavor, inside a namespace -- so nothing may be #included from here.

struct M {
    model::Field<double> gain;
    model::Field<int> count;
    model::Derived<double> doubled{[this] { return gain.get() * 2.0; }, gain};
};

void check(M& m) {
    int changes = 0;
    m.gain.observe([&changes](const double&) { ++changes; });
    double doubled = -1.0;
    m.doubled.observe([&doubled](const double& value) { doubled = value; });

    m.gain = 1.5;
    CHECK(m.gain.get() == 1.5);
    CHECK(changes == 1);
    CHECK(m.doubled.get() == 3.0);
    CHECK(doubled == 3.0);

    m.count = 4;
    CHECK(m.count.get() == 4);

    m.gain = 1.5;   // the same value again: nothing fires
    CHECK(changes == 1);
}
