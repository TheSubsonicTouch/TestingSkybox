// Unlocks all content: vehicles, weapons, clothing, enforcer upgrades,
// favours, and maxes levels. Edits state.json and LocalServer/state.json.
std::string UnlockAllContent() {
    const std::string dir = GameDir();
    if (dir.empty()) return "UnlockAll failed: game directory not found";

    const char* files[] = {"state.json", "LocalServer/state.json"};
    int edited = 0;

    // Load gamedb for UIDs
    std::string gamedbPath = dir + "LocalServer/devdata/gamedb.json";
    std::string gamedbText;
    if (!ReadFileText(gamedbPath, gamedbText)) {
        return "UnlockAll failed: gamedb.json not found";
    }
    json::Value gamedb;
    if (!json::parse(gamedbText, gamedb) || !gamedb.isObject()) {
        return "UnlockAll failed: gamedb.json parse error";
    }

    // Collect all UIDs from gamedb
    std::vector<std::string> enforcerUpgradeUIDs;
    std::vector<std::string> storeVehicleUIDs;
    std::vector<std::string> storeWeaponUIDs;
    std::vector<std::string> storeClothingUIDs;

    json::Value* eu = member(gamedb, "UFG::EnforcerUpgrade");
    if (eu && eu->isArray()) {
        for (auto& item : eu->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) enforcerUpgradeUIDs.push_back(uid->str);
        }
    }

    json::Value* sv = member(gamedb, "UFG::StoreVehicle");
    if (sv && sv->isArray()) {
        for (auto& item : sv->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeVehicleUIDs.push_back(uid->str);
        }
    }

    json::Value* sw = member(gamedb, "UFG::StoreWeapon");
    if (sw && sw->isArray()) {
        for (auto& item : sw->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeWeaponUIDs.push_back(uid->str);
        }
    }

    json::Value* sc = member(gamedb, "UFG::StoreClothing");
    if (sc && sc->isArray()) {
        for (auto& item : sc->array) {
            json::Value* uid = member(item, "$uid");
            if (uid && uid->isString()) storeClothingUIDs.push_back(uid->str);
        }
    }

    TW_Log("UnlockAll: %zu enforcer upgrades, %zu vehicles, %zu weapons, %zu clothing",
           enforcerUpgradeUIDs.size(), storeVehicleUIDs.size(),
           storeWeaponUIDs.size(), storeClothingUIDs.size());

    for (const char* rel : files) {
        const std::string path = dir + rel;
        std::string text;
        if (!ReadFileText(path, text)) continue;
        json::Value root;
        if (!json::parse(text, root) || !root.isObject()) continue;

        bool changed = false;
        json::Value* player = member(root, "player");
        if (!player || !player->isObject()) continue;

        // 1. EnforcerUpgradesBag - add all enforcer upgrades
        json::Value* eub = member(*player, "EnforcerUpgradesBag");
        if (!eub) {
            eub = &(*player)["EnforcerUpgradesBag"];
            eub->type = json::Value::Type::Array;
        }
        if (eub->isArray()) {
            eub->array.clear();
            for (const auto& uid : enforcerUpgradeUIDs) {
                json::Value entry;
                entry.type = json::Value::Type::Object;
                entry["Item1"].setString(uid);
                entry["Item2"].setInt(0);
                eub->array.push_back(std::move(entry));
            }
            changed = true;
        }

        // 2. InventoryBag - add all vehicles and weapons
        json::Value* ib = member(*player, "InventoryBag");
        if (!ib) {
            ib = &(*player)["InventoryBag"];
            ib->type = json::Value::Type::Array;
        }
        if (ib->isArray()) {
            std::set<std::string> existingStoreItemIDs;
            for (auto& item : ib->array) {
                if (item.isObject()) {
                    json::Value* sid = member(item, "StoreItemID");
                    if (sid && sid->isString()) existingStoreItemIDs.insert(sid->str);
                }
            }
            int nextInvID = 7018;
            for (const auto& uid : storeVehicleUIDs) {
                if (existingStoreItemIDs.find(uid) == existingStoreItemIDs.end()) {
                    json::Value entry;
                    entry.type = json::Value::Type::Object;
                    entry["SecondsElapsed"].setInt(0);
                    entry["TotalSeconds"].setInt(0);
                    entry["StoreItemID"].setString(uid);
                    entry["InventoryItemID"].setString(std::to_string(nextInvID++));
                    entry["AccountID"].setString("1");
                    ib->array.push_back(std::move(entry));
                    changed = true;
                }
            }
            for (const auto& uid : storeWeaponUIDs) {
                if (existingStoreItemIDs.find(uid) == existingStoreItemIDs.end()) {
                    json::Value entry;
                    entry.type = json::Value::Type::Object;
                    entry["SecondsElapsed"].setInt(0);
                    entry["TotalSeconds"].setInt(0);
                    entry["StoreItemID"].setString(uid);
                    entry["InventoryItemID"].setString(std::to_string(nextInvID++));
                    entry["AccountID"].setString("1");
                    ib->array.push_back(std::move(entry));
                    changed = true;
                }
            }
        }

        // 3. CustomizationBag - add all clothing
        json::Value* cb = member(*player, "CustomizationBag");
        if (!cb) {
            cb = &(*player)["CustomizationBag"];
            cb->type = json::Value::Type::Object;
        }
        if (cb->isObject()) {
            json::Value* csiu = member(*cb, "ClothingStoreItemUIDs");
            if (!csiu) {
                csiu = &(*cb)["ClothingStoreItemUIDs"];
                csiu->type = json::Value::Type::Array;
            }
            if (csiu->isArray()) {
                std::set<std::string> existing;
                for (auto& item : csiu->array) {
                    if (item.isString()) existing.insert(item.str);
                }
                for (const auto& uid : storeClothingUIDs) {
                    if (existing.find(uid) == existing.end()) {
                        json::Value entry;
                        entry.setString(uid);
                        csiu->array.push_back(std::move(entry));
                        changed = true;
                    }
                }
            }
            changed = true;
        }

        // 4. FavoursBag - mark all as available
        json::Value* fb = member(*player, "FavoursBag");
        if (fb && fb->isArray()) {
            for (auto& favour : fb->array) {
                if (favour.isObject()) {
                    favour["SecondsElapsed"].setInt(0);
                    favour["TotalSeconds"].setInt(0);
                    favour["IsNew"].setInt(1);
                }
            }
            changed = true;
        }

        // 5. Max levels (Enforcer, Rackets, Triad)
        const long long maxXP = 17177050LL; // Level 50
        json::Value* pdb = member(*player, "PlayerDataBag");
        if (pdb && pdb->isObject()) {
            pdb->operator[]("EnforcerXP").setInt(maxXP);
            pdb->operator[]("TriadLevel").setInt(50);
            changed = true;
        }
        json::Value* tb = member(*player, "TurfBag");
        if (tb && tb->isObject()) {
            json::Value* turf = member(*tb, "Turf");
            if (turf && turf->isObject()) {
                for (const char* key : {"Safehouse", "Warehouse", "Temple"}) {
                    json::Value* b = member(*turf, key);
                    if (b && b->isObject()) b->operator[]("RacketLevel").setInt(10);
                }
            }
            json::Value* rackets = member(*tb, "Rackets");
            if (rackets && rackets->isArray()) {
                for (auto& r : rackets->array) {
                    if (r.isObject()) r["RacketLevel"].setInt(10);
                }
            }
            changed = true;
        }

        // Also apply to opponents
        json::Value* opps = member(root, "opponents");
        if (opps && opps->isArray()) {
            for (auto& o : opps->array) {
                if (!o.isObject()) continue;
                json::Value* data = member(o, "data");
                if (data && data->isObject()) {
                    json::Value* opp_pdb = member(*data, "PlayerDataBag");
                    if (opp_pdb && opp_pdb->isObject()) {
                        opp_pdb->operator[]("EnforcerXP").setInt(maxXP);
                        opp_pdb->operator[]("TriadLevel").setInt(50);
                    }
                    json::Value* opp_tb = member(*data, "TurfBag");
                    if (opp_tb && opp_tb->isObject()) {
                        json::Value* opp_turf = member(*opp_tb, "Turf");
                        if (opp_turf && opp_turf->isObject()) {
                            for (const char* key : {"Safehouse", "Warehouse", "Temple"}) {
                                json::Value* b = member(*opp_turf, key);
                                if (b && b->isObject()) b->operator[]("RacketLevel").setInt(10);
                            }
                        }
                        json::Value* opp_rackets = member(*opp_tb, "Rackets");
                        if (opp_rackets && opp_rackets->isArray()) {
                            for (auto& r : opp_rackets->array) {
                                if (r.isObject()) r["RacketLevel"].setInt(10);
                            }
                        }
                    }
                    json::Value* opp_fb = member(*data, "FavoursBag");
                    if (opp_fb && opp_fb->isArray()) {
                        for (auto& favour : opp_fb->array) {
                            if (favour.isObject()) {
                                favour["SecondsElapsed"].setInt(0);
                                favour["TotalSeconds"].setInt(0);
                                favour["IsNew"].setInt(1);
                            }
                        }
                    }
                    changed = true;
                }
            }
        }

        if (changed && WriteFileText(path, json::dump(root, 1))) ++edited;
    }

    if (edited == 0) return "UnlockAll failed: state.json not found or unreadable";

    Touch(dir + "LocalServer/handlers.py");
    return "UnlockAll: done (" + std::to_string(edited) + " file" + (edited == 1 ? "" : "s") + ")";
}