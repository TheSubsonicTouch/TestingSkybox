#include "../src/menu_core.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace tw;

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

int main() {
    std::string weather;
    std::string levelTarget;
    int levelValue = -1;
    bool invincible = false;
    float tod = -1.0f;

    DebugHooks hooks;
    hooks.vehicleNames = {"vehA", "vehB"};
    hooks.npcNames = {"npcA"};
    hooks.weaponNames = {"wpnA"};
    hooks.weatherPresets = {"Sunny", "Cloudy", "Light rain", "Heavy rain"};
    hooks.levelTargets = {"Enforcer", "Rackets", "Triad", "Rivals"};
    hooks.setWeather = [&](const std::string& n) {
        weather = n;
        return std::string("Weather: ") + n;
    };
    hooks.setLevel = [&](const std::string& t, int v) {
        levelTarget = t;
        levelValue = v;
        return std::string("Level set: ") + t + " -> " + std::to_string(v);
    };
    hooks.setInvincible = [&](bool on) { invincible = on; };
    hooks.setTimeOfDay = [&](float v) { tod = v; };

    Menu menu;
    buildDebugMenu(menu, hooks);
    CHECK(menu.items().size() == 9);
    menu.handleKey(Key::Toggle);
    CHECK(menu.visible());

    // Item 0 is the weather selector.
    CHECK(menu.items()[0].kind == ItemKind::Selector);
    CHECK(menu.items()[0].label == "Weather");
    menu.handleKey(Key::Right);
    CHECK(menu.items()[0].optionIndex == 1);
    menu.handleKey(Key::Select);
    CHECK(weather == "Cloudy");
    CHECK(menu.status() == "Weather: Cloudy");

    // Items 1 and 2: level target / value; item 3 applies.
    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 1);
    CHECK(menu.items()[1].kind == ItemKind::Selector);
    CHECK(menu.items()[1].optionIndex == 0);
    menu.handleKey(Key::Right);  // Enforcer -> Rackets
    CHECK(menu.items()[1].optionIndex == 1);

    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 2);
    CHECK(menu.items()[2].kind == ItemKind::Slider);
    CHECK(menu.items()[2].value == 1.0f);
    for (int i = 0; i < 5; ++i) menu.handleKey(Key::Right);
    CHECK(menu.items()[2].value == 6.0f);
    for (int i = 0; i < 100; ++i) menu.handleKey(Key::Left);
    CHECK(menu.items()[2].value == 1.0f);   // clamped
    for (int i = 0; i < 100; ++i) menu.handleKey(Key::Right);
    CHECK(menu.items()[2].value == 50.0f);  // clamped

    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 3);
    menu.handleKey(Key::Select);
    CHECK(levelTarget == "Rackets");
    CHECK(levelValue == 50);
    CHECK(menu.status() == "Level set: Rackets -> 50");

    // Item 4: teleport action; no hook -> reported, not crashed.
    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 4);
    menu.handleKey(Key::Select);
    CHECK(menu.status() == "Teleport: not wired yet");

    // Item 5: invincible toggle.
    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 5);
    menu.handleKey(Key::Select);
    CHECK(invincible);

    // Item 7: time of day slider.
    menu.handleKey(Key::Down);  // Hide HUD
    menu.handleKey(Key::Down);  // Time of day
    CHECK(menu.selectedIndex() == 7);
    menu.handleKey(Key::Right);
    CHECK(tod > 12.49f && tod < 12.51f);

    // Wrap at the bottom: 8 -> 0.
    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 8);
    menu.handleKey(Key::Down);
    CHECK(menu.selectedIndex() == 0);

    // Rendering includes the selected marker and a selector value.
    auto lines = menu.renderLines();
    CHECK(!lines.empty());
    bool sawSelected = false, sawSelector = false, sawSlider = false;
    for (auto& l : lines) {
        if (l.rfind("> ", 0) == 0) sawSelected = true;
        if (l.find("Weather < ") != std::string::npos) sawSelector = true;
        if (l.find("Level value < ") != std::string::npos) sawSlider = true;
    }
    CHECK(sawSelected);
    CHECK(sawSelector);
    CHECK(sawSlider);

    // Back closes the menu.
    CHECK(menu.handleKey(Key::Back));
    CHECK(!menu.visible());

    // No level targets -> the level items are omitted entirely.
    DebugHooks sparse;
    sparse.weatherPresets = {"Sunny"};
    Menu m2;
    buildDebugMenu(m2, sparse);
    CHECK(m2.items().size() == 6);  // weather, teleport, invincible, HUD, time, reload

    if (failures == 0) std::printf("All menu core tests passed.\n");
    return failures == 0 ? 0 : 1;
}
