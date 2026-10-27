#include "../src/json_util.h"

#include <cstdio>
#include <string>

int main() {
    FILE* f = std::fopen("state_copy.json", "rb");
    if (!f) { std::printf("no state_copy.json\n"); return 1; }
    std::string text;
    char buf[8192];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    std::fclose(f);

    tw::json::Value root;
    if (!tw::json::parse(text, root)) { std::printf("parse failed\n"); return 1; }
    const std::string out = tw::json::dump(root, 1);

    tw::json::Value root2;
    if (!tw::json::parse(out, root2)) { std::printf("re-parse failed\n"); return 1; }

    // Spot-check a few fields survive the round trip.
    const tw::json::Value* pdb = root2.isObject() ? root2.find("player") : nullptr;
    pdb = pdb && pdb->isObject() ? pdb->find("PlayerDataBag") : nullptr;
    if (!pdb) { std::printf("player.PlayerDataBag missing\n"); return 1; }
    const tw::json::Value* name = pdb->find("Name");
    const tw::json::Value* xp = pdb->find("EnforcerXP");
    if (!name || !name->isString()) { std::printf("Name missing/not string\n"); return 1; }
    if (!xp || !xp->isNumber()) { std::printf("EnforcerXP missing/not number\n"); return 1; }
    std::printf("round-trip ok: Name=%s EnforcerXP=%lld\n", name->str.c_str(),
                xp->isNumber() ? (xp->type == tw::json::Value::Type::Int ? xp->integer
                                                                         : (long long)xp->number)
                               : -1);
    return 0;
}
