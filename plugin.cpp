// =====================================================================
//  Chest Glow - an SKSE plugin for Skyrim
//
//  What it does, in plain words:
//    1. Whenever a chest appears in the world, look inside it.
//    2. Sort each weapon into a tier by its gold value:
//         Legendary (yellow), Epic (purple) or Common (green).
//       The chest glows in the color of the best tier inside it.
//    3. Whenever items move in or out of a chest, look again, so the
//       glow goes away once you take the weapon.
//
//  Settings live in ChestGlow.ini. If SKSE Menu Framework is installed,
//  they can also be changed in-game from its menu.
// =====================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdarg>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr auto INI_PATH = "Data/SKSE/Plugins/ChestGlow.ini";

    // ---------------------------------------------------------------
    //  A tiny log file: Data/SKSE/Plugins/ChestGlow.log
    //  (With Mod Organizer 2 it lands in the "overwrite" folder.)
    // ---------------------------------------------------------------
    void Log(const std::string& a_text)
    {
        static std::mutex    lock;
        static std::ofstream file("Data/SKSE/Plugins/ChestGlow.log", std::ios::trunc);
        std::scoped_lock     guard(lock);
        if (file) {
            file << a_text << std::endl;
        }
    }

    // ---------------------------------------------------------------
    //  Settings (defaults here; ChestGlow.ini and the menu change them)
    // ---------------------------------------------------------------
    // One rarity tier: a gold range ("from" and "to", both included) and a color.
    struct Tier
    {
        const char* name;     // shown in the menu and log
        const char* iniName;  // used for the keys in ChestGlow.ini
        bool        enabled;
        int         from;
        int         to;
        bool        mustBeEnchanted;
        RE::Color   color;
    };

    // Best tier first. If ranges overlap, the better tier wins.
    constexpr int TIER_COUNT = 3;

    struct Settings
    {
        bool enabled = true;

        Tier tiers[TIER_COUNT] = {
            { "Legendary", "Legendary", true, 1000, 10000, false, RE::Color(255, 200, 0, 255) },  // yellow
            { "Epic", "Epic", true, 100, 1000, false, RE::Color(170, 60, 255, 255) },             // purple
            { "Common", "Common", true, 0, 100, false, RE::Color(60, 220, 80, 255) },             // green
        };
        bool artifactsAreLegendary = true;  // Daedric artifacts always count as Legendary

        float     glowStrength = 1.0f;                  // 0.0 (faint) to 1.0 (strong)
        float     pulseSpeed = 1.0f;                    // how fast the glow breathes; 0 = steady

        bool showConsoleMessages = false;  // print to the ~ console when a chest glows
        bool detailedLog = false;          // write every chest check to ChestGlow.log

        float scanRadius = 3000.0f;  // how far around you to look for containers (game units)
        float scanInterval = 2.0f;   // seconds between looks

        std::string glowTexture;  // optional: a texture path to use instead of the automatic pick
    };

    Settings g_settings;

    std::string Trim(std::string a_text)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        a_text.erase(a_text.begin(), std::find_if(a_text.begin(), a_text.end(), notSpace));
        a_text.erase(std::find_if(a_text.rbegin(), a_text.rend(), notSpace).base(), a_text.end());
        return a_text;
    }

    std::string ToLower(std::string a_text)
    {
        std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a_text;
    }

    RE::Color ParseColor(const std::string& a_text, RE::Color a_fallback)
    {
        std::stringstream ss(a_text);
        std::string       part;
        int               values[3]{};
        for (int i = 0; i < 3; ++i) {
            if (!std::getline(ss, part, ',')) {
                return a_fallback;
            }
            try {
                values[i] = std::clamp(std::stoi(Trim(part)), 0, 255);
            } catch (...) {
                return a_fallback;
            }
        }
        return RE::Color(static_cast<std::uint8_t>(values[0]), static_cast<std::uint8_t>(values[1]),
            static_cast<std::uint8_t>(values[2]), 255);
    }

    std::string ColorToText(const RE::Color& a_color)
    {
        return std::to_string(a_color.red) + ", " + std::to_string(a_color.green) + ", " + std::to_string(a_color.blue);
    }

    void LoadSettings()
    {
        std::ifstream file(INI_PATH);
        if (!file) {
            Log("ChestGlow.ini not found, using default settings.");
            return;
        }

        std::unordered_map<std::string, std::string> values;
        std::string                                  line;
        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            values[ToLower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
        }

        auto getInt = [&](const char* a_key, int a_fallback) {
            auto it = values.find(a_key);
            if (it == values.end()) return a_fallback;
            try { return std::stoi(it->second); } catch (...) { return a_fallback; }
        };
        auto getFloat = [&](const char* a_key, float a_fallback) {
            auto it = values.find(a_key);
            if (it == values.end()) return a_fallback;
            try { return std::stof(it->second); } catch (...) { return a_fallback; }
        };
        auto getBool = [&](const char* a_key, bool a_fallback) {
            auto it = values.find(a_key);
            if (it == values.end()) return a_fallback;
            auto v = ToLower(it->second);
            return v == "1" || v == "true" || v == "yes" || v == "on";
        };
        auto getColor = [&](const char* a_key, RE::Color a_fallback) {
            auto it = values.find(a_key);
            return it == values.end() ? a_fallback : ParseColor(it->second, a_fallback);
        };

        g_settings.enabled = getBool("enabled", g_settings.enabled);
        for (auto& tier : g_settings.tiers) {
            const std::string key = ToLower(tier.iniName);
            tier.enabled = getBool((key + "enabled").c_str(), tier.enabled);
            tier.from = std::max(0, getInt((key + "from").c_str(), tier.from));
            tier.to = std::max(0, getInt((key + "to").c_str(), tier.to));
            tier.mustBeEnchanted = getBool((key + "mustbeenchanted").c_str(), tier.mustBeEnchanted);
            tier.color = getColor((key + "color").c_str(), tier.color);
        }
        g_settings.artifactsAreLegendary = getBool("artifactsarelegendary", g_settings.artifactsAreLegendary);
        g_settings.glowStrength = std::clamp(getFloat("glowstrength", g_settings.glowStrength), 0.0f, 1.0f);
        g_settings.pulseSpeed = std::max(0.0f, getFloat("pulsespeed", g_settings.pulseSpeed));
        g_settings.showConsoleMessages = getBool("showconsolemessages", g_settings.showConsoleMessages);
        g_settings.detailedLog = getBool("detailedlog", g_settings.detailedLog);
        g_settings.scanRadius = std::clamp(getFloat("scanradius", g_settings.scanRadius), 500.0f, 10000.0f);
        g_settings.scanInterval = std::clamp(getFloat("scaninterval", g_settings.scanInterval), 0.5f, 10.0f);
        if (auto it = values.find("glowtexture"); it != values.end()) {
            g_settings.glowTexture = it->second;
        }

        std::string summary = "Settings loaded.";
        for (const auto& tier : g_settings.tiers) {
            summary += std::string(" ") + tier.name + ": " + std::to_string(tier.from) + " to " + std::to_string(tier.to) +
                       (tier.enabled ? "" : " (off)") + ".";
        }
        Log(summary);
    }

    bool SaveSettings()
    {
        std::ofstream file(INI_PATH, std::ios::trunc);
        if (!file) {
            Log("ERROR: could not save ChestGlow.ini.");
            return false;
        }
        auto b = [](bool a_value) { return a_value ? "true" : "false"; };
        file << "; =========================================================\n"
             << ";  Chest Glow settings (saved from the in-game menu)\n"
             << ";  Edit with Notepad or in-game. Lines starting with ; are notes.\n"
             << "; =========================================================\n\n"
             << "[General]\n"
             << "; Turn the whole mod on or off.\n"
             << "Enabled = " << b(g_settings.enabled) << "\n\n"
             << "; Each tier is a gold range: From and To are both included.\n"
             << "; If ranges overlap, the better tier wins. A chest glows in the\n"
             << "; color of the best tier inside it. Colors are Red, Green, Blue (0 to 255).\n"
             << "; MustBeEnchanted = true means only enchanted weapons can be in that tier.\n\n";
        for (const auto& tier : g_settings.tiers) {
            const std::string n = tier.iniName;
            file << "[" << n << "]\n"
                 << n << "Enabled = " << b(tier.enabled) << "\n"
                 << n << "From = " << tier.from << "\n"
                 << n << "To = " << tier.to << "\n"
                 << n << "MustBeEnchanted = " << b(tier.mustBeEnchanted) << "\n"
                 << n << "Color = " << ColorToText(tier.color) << "\n\n";
        }
        file << "[Rules]\n"
             << "; Daedric artifacts always count as Legendary, whatever their value.\n"
             << "ArtifactsAreLegendary = " << b(g_settings.artifactsAreLegendary) << "\n\n"
             << "[Glow]\n"
             << "; How bright the glow is, from 0.0 (faint) to 1.0 (strong).\n"
             << "GlowStrength = " << g_settings.glowStrength << "\n"
             << "; How fast the glow gently pulses. 0 = steady glow.\n"
             << "PulseSpeed = " << g_settings.pulseSpeed << "\n"
             << "; Leave empty for the automatic pick, or paste a texture name from ChestGlow.log.\n"
             << "GlowTexture = " << g_settings.glowTexture << "\n\n"
             << "[Debug]\n"
             << "; If true, the ~ console prints a line each time a chest starts glowing.\n"
             << "ShowConsoleMessages = " << b(g_settings.showConsoleMessages) << "\n"
             << "; If true, ChestGlow.log records every chest check (for troubleshooting).\n"
             << "DetailedLog = " << b(g_settings.detailedLog) << "\n\n"
             << "[Performance]\n"
             << "; How far around you the mod looks for containers (Skyrim units; about 70 = 1 meter).\n"
             << "ScanRadius = " << g_settings.scanRadius << "\n"
             << "; Seconds between looks.\n"
             << "ScanInterval = " << g_settings.scanInterval << "\n";
        Log("Settings saved to ChestGlow.ini.");
        return true;
    }

    // ---------------------------------------------------------------
    //  Rarity rules
    // ---------------------------------------------------------------
    // A tier is identified by its position in the list: 0 = Legendary (best),
    // 1 = Epic, 2 = Common. NO_TIER means "doesn't glow".
    constexpr int NO_TIER = TIER_COUNT;

    const char* TierName(int a_tier)
    {
        return a_tier < TIER_COUNT ? g_settings.tiers[a_tier].name : "None (no glow)";
    }

    // Which tier a single weapon belongs to. Lower number = better.
    int GetWeaponTier(int a_value, bool a_enchanted, bool a_artifact)
    {
        if (a_artifact && g_settings.tiers[0].enabled) {
            return 0;  // Legendary
        }
        for (int i = 0; i < TIER_COUNT; ++i) {
            const auto& tier = g_settings.tiers[i];
            if (!tier.enabled || (tier.mustBeEnchanted && !a_enchanted)) {
                continue;
            }
            if (a_value >= tier.from && a_value <= tier.to) {
                return i;  // first match is the best, because the list is best-first
            }
        }
        return NO_TIER;
    }

    struct ChestReport
    {
        int         tier = NO_TIER;
        std::string chestName;
        std::string weaponName;  // the weapon that decided the color
        int         weaponValue = 0;
        bool        weaponEnchanted = false;
        bool        weaponIsArtifact = false;
        int         weaponsFound = 0;
    };

    // What the menu shows under "Last chest checked".
    std::mutex  g_reportLock;
    ChestReport g_lastReport;
    bool        g_hasReport = false;

    ChestReport CheckChest(RE::TESObjectREFR* a_chest)
    {
        ChestReport report;
        report.chestName = a_chest->GetName() ? a_chest->GetName() : "";

        // Only look at weapons.
        auto inventory = a_chest->GetInventory([](RE::TESBoundObject& a_obj) { return a_obj.IsWeapon(); });

        for (auto& [object, data] : inventory) {
            auto& [count, entry] = data;
            if (count <= 0 || !entry) {
                continue;
            }
            auto* weapon = object->As<RE::TESObjectWEAP>();
            if (!weapon) {
                continue;
            }
            ++report.weaponsFound;

            const int  value = entry->GetValue();
            const bool enchanted = entry->IsEnchanted() || weapon->formEnchanting != nullptr;
            const bool artifact = g_settings.artifactsAreLegendary && weapon->HasKeywordString("DaedricArtifact");

            const int tier = GetWeaponTier(value, enchanted, artifact);

            // Keep the best weapon (best tier, then highest value) as the "reason".
            const bool better = tier < report.tier || (tier == report.tier && value > report.weaponValue);
            if (better || report.weaponName.empty()) {
                report.tier = std::min(report.tier, tier);
                report.weaponName = weapon->GetName() ? weapon->GetName() : "(unnamed)";
                report.weaponValue = value;
                report.weaponEnchanted = enchanted;
                report.weaponIsArtifact = artifact;
            }
        }
        return report;
    }

    // ---------------------------------------------------------------
    //  The two glow effects (built in code, no Creation Kit needed)
    // ---------------------------------------------------------------
    RE::TESEffectShader* g_shaders[TIER_COUNT] = {};  // one glow per tier
    std::string          g_textureInUse;

    bool IsOurShader(const RE::TESEffectShader* a_shader)
    {
        return a_shader && std::find(std::begin(g_shaders), std::end(g_shaders), a_shader) != std::end(g_shaders);
    }

    bool ShadersReady()
    {
        return std::all_of(std::begin(g_shaders), std::end(g_shaders), [](auto* s) { return s != nullptr; });
    }

    // Find a vanilla glow effect to borrow textures from, so ours renders properly.
    RE::TESEffectShader* FindTemplateShader()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return nullptr;
        }

        auto containsAny = [](const std::string& a_text, std::initializer_list<const char*> a_words) {
            for (auto* word : a_words) {
                if (a_text.find(word) != std::string::npos) {
                    return true;
                }
            }
            return false;
        };

        RE::TESEffectShader*  preferred = nullptr;
        RE::TESEffectShader*  fallback = nullptr;
        std::set<std::string> seen;

        Log("Textures you can use for GlowTexture in ChestGlow.ini:");
        for (auto* shader : dataHandler->GetFormArray<RE::TESEffectShader>()) {
            if (!shader || shader->fillTexture.textureName.empty()) {
                continue;
            }
            if (shader->data.flags.any(RE::EffectShaderData::Flags::kDisableTextureShader)) {
                continue;
            }
            std::string original = shader->fillTexture.textureName.c_str();
            std::string name = ToLower(original);

            if (seen.insert(name).second) {
                Log("  " + original);
            }

            // Skip elemental textures (lightning, fire, ice...). They have their own
            // strong colors and patterns that drown out ours.
            if (containsAny(name, { "shock", "lightning", "fire", "flame", "frost", "ice", "blood", "burn", "lava",
                                      "smoke", "poison", "spark", "drain", "absorb" })) {
                continue;
            }

            // Plain, soft textures tint cleanly to any color, so prefer those.
            if (!preferred && containsAny(name, { "white", "grey", "gray", "glow", "blank", "gradient", "soft", "smooth" })) {
                preferred = shader;
            }
            if (!fallback) {
                fallback = shader;
            }
        }
        return preferred ? preferred : fallback;
    }

    // Applies our color, brightness and pulse settings to a glow effect.
    void ApplyLook(RE::TESEffectShader* a_shader, RE::Color a_color)
    {
        if (!a_shader) {
            return;
        }
        auto& d = a_shader->data;

        d.fillTextureEffectColorKey1 = a_color;
        d.fillTextureEffectColorKey2 = a_color;
        d.fillTextureEffectColorKey3 = a_color;
        d.edgeEffectColor = a_color;
        d.edgeColor = a_color;

        // Fade in quickly, then stay on (persistent) with a gentle pulse.
        const float strength = g_settings.glowStrength;
        const float pulse = g_settings.pulseSpeed > 0.0f ? strength * 0.3f : 0.0f;

        d.fillTextureEffectAlphaFadeInTime = 0.5f;
        d.fillTextureEffectFullAlphaTime = 0.0f;
        d.fillTextureEffectAlphaFadeOutTime = 0.5f;
        d.fillTextureEffectFullAlphaRatio = strength;
        d.fillTextureEffectPersistentAlphaRatio = strength * 0.6f;
        d.fillTextureEffectAlphaPulseAmplitude = pulse;
        d.fillTextureEffectAlphaPulseFrequency = g_settings.pulseSpeed;

        d.edgeEffectAlphaFadeInTime = 0.5f;
        d.edgeEffectFullAlphaTime = 0.0f;
        d.edgeEffectAlphaFadeOutTime = 0.5f;
        d.edgeEffectFullAlphaRatio = strength;
        d.edgeEffectPersistentAlphaRatio = strength;
        d.edgeEffectAlphaPulseAmplitude = pulse;
        d.edgeEffectAlphaPulseFrequency = g_settings.pulseSpeed;
    }

    RE::TESEffectShader* MakeGlowShader(RE::TESEffectShader* a_template, RE::Color a_color)
    {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESEffectShader>();
        if (!factory) {
            return nullptr;
        }
        auto* shader = static_cast<RE::TESEffectShader*>(factory->Create());
        if (!shader) {
            return nullptr;
        }

        // Start from the vanilla effect's settings and texture...
        shader->data = a_template->data;
        shader->fillTexture.textureName = a_template->fillTexture.textureName;
        if (!g_settings.glowTexture.empty()) {
            shader->fillTexture.textureName = g_settings.glowTexture.c_str();
        }
        // No borrowed color palette: it would override our colors.
        shader->membranePaletteTexture.textureName = "";
        shader->holesTexture.textureName = "";
        shader->particleShaderTexture.textureName = "";

        auto& d = shader->data;
        d.flags.reset(RE::EffectShaderData::Flags::kGreyscaleToColor);

        // ...then turn off sparkle particles and debris, which would look messy on chests.
        d.flags.set(RE::EffectShaderData::Flags::kDisableParticleShader);
        d.flags.reset(RE::EffectShaderData::Flags::kSkinOnly, RE::EffectShaderData::Flags::kIgnoreWeapons);
        d.addonModels = nullptr;
        d.ambientSound = nullptr;
        if (d.edgeEffectFallOff <= 0.0f) {
            d.edgeEffectFallOff = 1.0f;
        }
        if (d.colorScale <= 0.0f) {
            d.colorScale = 1.0f;
        }

        ApplyLook(shader, a_color);
        return shader;
    }

    bool CreateShaders()
    {
        auto* templ = FindTemplateShader();
        if (!templ) {
            Log("ERROR: could not find a vanilla effect to base the glow on.");
            return false;
        }
        Log(std::string("Borrowing settings from the effect with texture: ") + templ->fillTexture.textureName.c_str());
        g_textureInUse = templ->fillTexture.textureName.c_str();
        if (!g_settings.glowTexture.empty()) {
            Log("Using your GlowTexture from ChestGlow.ini instead: " + g_settings.glowTexture);
            g_textureInUse = g_settings.glowTexture;
        }

        for (int i = 0; i < TIER_COUNT; ++i) {
            g_shaders[i] = MakeGlowShader(templ, g_settings.tiers[i].color);
        }
        if (!ShadersReady()) {
            Log("ERROR: could not create the glow effects.");
            return false;
        }
        Log("Glow effects created.");
        return true;
    }

    // ---------------------------------------------------------------
    //  Turning the glow on and off
    // ---------------------------------------------------------------

    // Asks the game itself to play an effect on an object. (Older versions of the
    // library don't include this, so we call the game's own function directly,
    // the same way newer versions of the library do.)
    RE::ShaderReferenceEffect* PlayShader(RE::TESObjectREFR* a_ref, RE::TESEffectShader* a_shader, float a_duration)
    {
        using func_t = RE::ShaderReferenceEffect* (*)(RE::TESObjectREFR*, RE::TESEffectShader*, float,
            RE::TESObjectREFR*, bool, bool, RE::NiAVObject*, bool);
        static REL::Relocation<func_t> func{ RELOCATION_ID(19446, 19872) };
        return func(a_ref, a_shader, a_duration, nullptr, false, false, nullptr, false);
    }

    bool IsChest(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref || a_ref->IsDisabled() || a_ref->IsMarkedForDeletion()) {
            return false;
        }
        auto* base = a_ref->GetBaseObject();
        return base && base->Is(RE::FormType::Container);
    }

    // Removes any of OUR glows from this chest (never touches other mods' effects).
    void RemoveGlow(RE::TESObjectREFR* a_chest)
    {
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) {
            return;
        }
        processLists->ForEachShaderEffect([&](auto&& a_effect) {
            RE::ShaderReferenceEffect* effect = nullptr;
            if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(a_effect)>>) {
                effect = a_effect;
            } else {
                effect = &a_effect;
            }
            if (effect && IsOurShader(effect->effectData)) {
                auto target = effect->target.get();
                if (target && target.get() == a_chest) {
                    effect->finished = true;
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    void DebugLog(const std::string& a_text)
    {
        if (g_settings.detailedLog) {
            Log(a_text);
        }
    }

    std::string Describe(RE::TESObjectREFR* a_chest)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", a_chest->GetFormID());
        const char* name = a_chest->GetName();
        return std::string("'") + (name && *name ? name : "(no name)") + "' [" + id + "]";
    }

    // Which glow tier a chest should have right now (NO_TIER = none).
    int DesiredTier(const ChestReport& a_report)
    {
        return g_settings.enabled ? a_report.tier : NO_TIER;
    }

    // Puts the right glow on a chest (or none).
    void SetGlow(RE::TESObjectREFR* a_chest, int a_tier, const ChestReport& a_report)
    {
        RemoveGlow(a_chest);
        if (a_tier < TIER_COUNT) {
            PlayShader(a_chest, g_shaders[a_tier], -1.0f);  // -1 = glow until we remove it
            if (g_settings.showConsoleMessages) {
                RE::ConsoleLog::GetSingleton()->Print("[ChestGlow] %s chest '%s' because of '%s' (%d gold)",
                    TierName(a_tier), a_report.chestName.c_str(), a_report.weaponName.c_str(), a_report.weaponValue);
            }
        }
    }

    void RememberReport(const ChestReport& a_report)
    {
        if (a_report.weaponsFound > 0) {
            std::scoped_lock guard(g_reportLock);
            g_lastReport = a_report;
            g_hasReport = true;
        }
    }

    void LogCheck(RE::TESObjectREFR* a_chest, const ChestReport& a_report, const char* a_source)
    {
        if (!g_settings.detailedLog) {
            return;
        }
        std::string line = std::string("[") + a_source + "] " + Describe(a_chest) + ": " +
                           std::to_string(a_report.weaponsFound) + " weapon(s)";
        if (a_report.weaponsFound > 0) {
            line += ", best '" + a_report.weaponName + "' (" + std::to_string(a_report.weaponValue) + " gold)";
        }
        line += std::string(" -> ") + TierName(DesiredTier(a_report));
        Log(line);
    }

    // Checks one chest right now and sets its glow. Main thread only.
    void UpdateChest(RE::TESObjectREFR* a_chest, const char* a_source)
    {
        if (!ShadersReady() || !IsChest(a_chest)) {
            return;
        }
        if (!a_chest->Is3DLoaded()) {
            RemoveGlow(a_chest);
            return;  // not visible yet; the nearby scan will catch it once it is
        }

        const ChestReport report = CheckChest(a_chest);
        LogCheck(a_chest, report, a_source);
        RememberReport(report);
        SetGlow(a_chest, DesiredTier(report), report);
    }

    // Run the check a moment later, on the game's main thread (the safe way).
    void QueueUpdate(RE::FormID a_chestID, const char* a_source)
    {
        if (a_chestID == 0) {
            return;
        }
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([a_chestID, a_source]() {
                UpdateChest(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_chestID), a_source);
            });
        }
    }

    // ---------------------------------------------------------------
    //  The nearby scan: every few seconds, look at the containers around
    //  you and fix any glow that is missing or wrong. This is the safety net
    //  for chests the game doesn't tell us about.
    // ---------------------------------------------------------------

    // Which of our glows each object has right now, read from the game's list of active effects.
    std::unordered_map<RE::TESObjectREFR*, int> FindCurrentGlows()
    {
        std::unordered_map<RE::TESObjectREFR*, int> result;
        auto*                                       processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) {
            return result;
        }
        processLists->ForEachShaderEffect([&](auto&& a_effect) {
            RE::ShaderReferenceEffect* effect = nullptr;
            if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(a_effect)>>) {
                effect = a_effect;
            } else {
                effect = &a_effect;
            }
            if (effect && !effect->finished) {
                for (int i = 0; i < TIER_COUNT; ++i) {
                    if (effect->effectData == g_shaders[i]) {
                        if (auto target = effect->target.get()) {
                            result[target.get()] = i;
                        }
                        break;
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return result;
    }

    // Containers in the loaded area around you. Filled from the game's "area loaded"
    // signal (see CellAttachListener below). Main thread only.
    std::unordered_set<RE::FormID> g_nearbyContainers;

    // Scanning only runs once a save is fully loaded, and pauses during loading screens.
    std::atomic<bool> g_inWorld{ false };
    std::atomic<int>  g_scansToSkip{ 0 };     // wait a few seconds after loading before scanning
    std::atomic<bool> g_logNextScan{ false };  // log the first scan after each load step by step

    // a_force = re-apply every glow (used after changing colors in the menu).
    void ScanNearby(bool a_force)
    {
        if (!ShadersReady() || !g_inWorld) {
            return;
        }
        if (auto* ui = RE::UI::GetSingleton(); !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
            return;  // loading screen
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->GetParentCell()) {
            return;
        }

        const bool logSteps = g_settings.detailedLog && g_logNextScan.exchange(false);

        // Which known containers are close enough and visible.
        const auto  playerPos = player->GetPosition();
        const float maxDistSq = g_settings.scanRadius * g_settings.scanRadius;
        std::vector<RE::TESObjectREFR*> nearby;
        for (auto id : g_nearbyContainers) {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (!ref || playerPos.GetSquaredDistance(ref->GetPosition()) > maxDistSq) {
                continue;
            }
            if (IsChest(ref) && ref->Is3DLoaded()) {
                nearby.push_back(ref);
            }
        }
        if (logSteps) {
            Log("First scan after loading: " + std::to_string(g_nearbyContainers.size()) + " container(s) in the loaded area, " +
                std::to_string(nearby.size()) + " close enough to check.");
        }

        const auto current = FindCurrentGlows();
        for (auto* chest : nearby) {
            if (logSteps) {
                Log("  checking " + Describe(chest));
            }
            const ChestReport report = CheckChest(chest);
            const int         want = DesiredTier(report);
            const auto        it = current.find(chest);
            const int         have = it != current.end() ? it->second : NO_TIER;

            if (a_force || want != have) {
                if (!a_force) {
                    // The scan had to fix this one, so the game's own signal missed it.
                    LogCheck(chest, report, "scan fix");
                }
                RememberReport(report);
                SetGlow(chest, want, report);
            }
        }
        if (logSteps) {
            Log("First scan finished.");
        }
    }

    std::atomic<bool> g_scanQueued{ false };

    void QueueScan(bool a_force)
    {
        if (!a_force && g_scanQueued.exchange(true)) {
            return;  // one is already waiting
        }
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([a_force]() {
                if (!a_force) {
                    g_scanQueued = false;
                }
                ScanNearby(a_force);
            });
        } else if (!a_force) {
            g_scanQueued = false;
        }
    }

    // A small background timer that asks for a scan every few seconds.
    void StartScanner()
    {
        std::thread([]() {
            for (;;) {
                const auto ms = static_cast<int>(g_settings.scanInterval * 1000.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(500, ms)));
                if (!g_inWorld) {
                    continue;
                }
                if (g_scansToSkip > 0) {
                    --g_scansToSkip;
                    continue;
                }
                QueueScan(false);
            }
        }).detach();
        Log("Nearby scan started.");
    }

    // Re-applies the colors/brightness and re-checks every nearby chest. Used by the menu.
    void QueueRefreshAll()
    {
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([]() {
                for (int i = 0; i < TIER_COUNT; ++i) {
                    ApplyLook(g_shaders[i], g_settings.tiers[i].color);
                }
                ScanNearby(true);
            });
        }
    }

    // ---------------------------------------------------------------
    //  Listening to game events
    // ---------------------------------------------------------------

    // Fires when an object's 3D model loads or unloads, e.g. you walk into a room with a chest.
    class ObjectLoadedListener : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->loaded) {
                QueueUpdate(a_event->formID, "came into view");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Fires when items move between containers, e.g. you take a sword out of a chest.
    class ContainerChangedListener : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (a_event) {
                QueueUpdate(a_event->oldContainer, "items moved");
                QueueUpdate(a_event->newContainer, "items moved");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Fires for each object when the area around it loads or unloads.
    // We use it to keep a list of the containers around you.
    class CellAttachListener : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
            RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
        {
            if (!a_event || !a_event->reference) {
                return RE::BSEventNotifyControl::kContinue;
            }
            RE::TESObjectREFR* ref = &*a_event->reference;
            auto*              base = ref->GetBaseObject();
            if (!base || !base->Is(RE::FormType::Container)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto id = ref->GetFormID();
            const bool attached = a_event->attached != 0;
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([id, attached]() {
                    if (attached) {
                        g_nearbyContainers.insert(id);
                    } else {
                        g_nearbyContainers.erase(id);
                    }
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Fires when something is activated, e.g. you open a chest.
    class ActivateListener : public RE::BSTEventSink<RE::TESActivateEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event,
            RE::BSTEventSource<RE::TESActivateEvent>*) override
        {
            if (a_event && a_event->objectActivated) {
                QueueUpdate(a_event->objectActivated->GetFormID(), "opened");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    ObjectLoadedListener     g_objectLoadedListener;
    ContainerChangedListener g_containerChangedListener;
    ActivateListener         g_activateListener;
    CellAttachListener       g_cellAttachListener;

    // ---------------------------------------------------------------
    //  In-game settings page (only if SKSE Menu Framework is installed)
    //
    //  SKSE Menu Framework shares the functions it uses to draw menus.
    //  We look them up by name, so there is nothing extra to build or link.
    // ---------------------------------------------------------------
    namespace Menu
    {
        struct Vec2
        {
            float x, y;
        };
        struct Vec4
        {
            float x, y, z, w;
        };

        SKSE::WinAPI::HMODULE g_framework = nullptr;

        template <class T>
        T Find(const char* a_name)
        {
            return g_framework ? reinterpret_cast<T>(SKSE::WinAPI::GetProcAddress(g_framework, a_name)) : nullptr;
        }

        // The drawing functions we use.
        using TextV_t = void (*)(const char*, va_list);
        using TextColoredV_t = void (*)(Vec4, const char*, va_list);
        using Button_t = bool (*)(const char*, Vec2);
        using Checkbox_t = bool (*)(const char*, bool*);
        using SliderInt_t = bool (*)(const char*, int*, int, int, const char*, int);
        using InputInt_t = bool (*)(const char*, int*, int, int, int);
        using PushID_t = void (*)(int);
        using PushItemWidth_t = void (*)(float);
        using SliderFloat_t = bool (*)(const char*, float*, float, float, const char*, int);
        using ColorEdit3_t = bool (*)(const char*, float*, int);
        using Void_t = void (*)();
        using SeparatorText_t = void (*)(const char*);
        using SameLine_t = void (*)(float, float);
        using AddSectionItem_t = void (*)(const char*, void (*)());

        TextV_t         g_textWrappedV = nullptr;
        TextColoredV_t  g_textColoredV = nullptr;
        Button_t        g_button = nullptr;
        Checkbox_t      g_checkbox = nullptr;
        SliderInt_t     g_sliderInt = nullptr;
        InputInt_t      g_inputInt = nullptr;
        PushID_t        g_pushID = nullptr;
        Void_t          g_popID = nullptr;
        PushItemWidth_t g_pushItemWidth = nullptr;
        Void_t          g_popItemWidth = nullptr;
        SliderFloat_t   g_sliderFloat = nullptr;
        ColorEdit3_t    g_colorEdit3 = nullptr;
        Void_t          g_spacing = nullptr;
        SeparatorText_t g_separatorText = nullptr;
        SameLine_t      g_sameLine = nullptr;

        void Text(const char* a_fmt, ...)
        {
            va_list args;
            va_start(args, a_fmt);
            g_textWrappedV(a_fmt, args);
            va_end(args);
        }

        void TextColored(Vec4 a_color, const char* a_fmt, ...)
        {
            va_list args;
            va_start(args, a_fmt);
            g_textColoredV(a_color, a_fmt, args);
            va_end(args);
        }

        // Color pickers work with 0-1 numbers; the game uses 0-255.
        bool ColorPicker(const char* a_label, RE::Color& a_color)
        {
            float rgb[3] = { a_color.red / 255.0f, a_color.green / 255.0f, a_color.blue / 255.0f };
            if (!g_colorEdit3(a_label, rgb, 0)) {
                return false;
            }
            a_color = RE::Color(static_cast<std::uint8_t>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<std::uint8_t>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<std::uint8_t>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f), 255);
            return true;
        }

        std::string g_saveMessage;

        void Render()
        {
            bool changed = false;

            g_separatorText("General");
            changed |= g_checkbox("Mod enabled", &g_settings.enabled);
            g_checkbox("Print a console line when a chest glows", &g_settings.showConsoleMessages);
            g_checkbox("Detailed log (for troubleshooting)", &g_settings.detailedLog);
            g_sliderFloat("Scan distance", &g_settings.scanRadius, 500.0f, 10000.0f, "%.0f", 0);

            Text("Each tier is a gold range. 'From' and 'To' are both included. "
                 "If ranges overlap, the better tier wins.");
            for (int i = 0; i < TIER_COUNT; ++i) {
                auto& tier = g_settings.tiers[i];
                g_pushID(i);  // lets each tier reuse the same labels

                const std::string title = std::string(tier.name) + " tier";
                g_separatorText(title.c_str());
                changed |= g_checkbox("Enabled", &tier.enabled);
                g_pushItemWidth(160.0f);
                changed |= g_inputInt("From (gold)", &tier.from, 10, 100, 0);
                g_sameLine(0.0f, 20.0f);
                changed |= g_inputInt("To (gold)", &tier.to, 10, 100, 0);
                g_popItemWidth();
                tier.from = std::max(0, tier.from);
                tier.to = std::max(0, tier.to);
                changed |= ColorPicker("Color", tier.color);
                changed |= g_checkbox("Only enchanted weapons", &tier.mustBeEnchanted);
                if (i == 0) {
                    changed |= g_checkbox("Daedric artifacts are always Legendary", &g_settings.artifactsAreLegendary);
                }
                if (tier.from > tier.to) {
                    TextColored({ 1.0f, 0.6f, 0.2f, 1.0f }, "'From' is bigger than 'To', so nothing can be in this tier.");
                }

                g_popID();
            }

            g_separatorText("Glow look");
            changed |= g_sliderFloat("Brightness", &g_settings.glowStrength, 0.0f, 1.0f, "%.2f", 0);
            changed |= g_sliderFloat("Pulse speed (0 = steady)", &g_settings.pulseSpeed, 0.0f, 5.0f, "%.1f", 0);
            Text("Texture in use: %s", g_textureInUse.c_str());
            Text("(To change the texture, edit GlowTexture in ChestGlow.ini and restart the game.)");

            if (changed) {
                QueueRefreshAll();  // see the result right away on nearby chests
            }

            g_spacing();
            if (g_button("Save settings", { 0.0f, 0.0f })) {
                g_saveMessage = SaveSettings() ? "Saved." : "Could not save. Check ChestGlow.log.";
            }
            g_sameLine(0.0f, -1.0f);
            if (g_button("Re-check nearby chests", { 0.0f, 0.0f })) {
                QueueRefreshAll();
            }
            if (!g_saveMessage.empty()) {
                Text("%s", g_saveMessage.c_str());
            }
            Text("Changes apply right away. Press Save to keep them for next time.");

            g_separatorText("Last chest checked (for testing)");
            ChestReport report;
            bool        has = false;
            {
                std::scoped_lock guard(g_reportLock);
                report = g_lastReport;
                has = g_hasReport;
            }
            if (!has) {
                Text("No chest with weapons checked yet. Walk near one, or add a weapon to one.");
            } else {
                Vec4 color{ 0.7f, 0.7f, 0.7f, 1.0f };
                if (report.tier < TIER_COUNT) {
                    const auto& c = g_settings.tiers[report.tier].color;
                    color = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, 1.0f };
                }
                Text("Chest: %s", report.chestName.c_str());
                TextColored(color, "Result: %s", TierName(report.tier));
                Text("Best weapon inside: %s", report.weaponName.c_str());
                Text("Its value: %d gold  |  Enchanted: %s  |  Daedric artifact: %s", report.weaponValue,
                    report.weaponEnchanted ? "yes" : "no", report.weaponIsArtifact ? "yes" : "no");
            }
        }

        void Register()
        {
            // (CommonLib turns this name into its own Windows helper.)
            g_framework = GetModuleHandle(L"SKSEMenuFramework");
            if (!g_framework) {
                Log("SKSE Menu Framework not found, so there is no in-game menu. (Settings still work from ChestGlow.ini.)");
                return;
            }

            g_textWrappedV = Find<TextV_t>("igTextWrappedV");
            g_textColoredV = Find<TextColoredV_t>("igTextColoredV");
            g_button = Find<Button_t>("igButton");
            g_checkbox = Find<Checkbox_t>("igCheckbox");
            g_sliderInt = Find<SliderInt_t>("igSliderInt");
            g_inputInt = Find<InputInt_t>("igInputInt");
            g_pushID = Find<PushID_t>("igPushID_Int");
            g_popID = Find<Void_t>("igPopID");
            g_pushItemWidth = Find<PushItemWidth_t>("igPushItemWidth");
            g_popItemWidth = Find<Void_t>("igPopItemWidth");
            g_sliderFloat = Find<SliderFloat_t>("igSliderFloat");
            g_colorEdit3 = Find<ColorEdit3_t>("igColorEdit3");
            g_spacing = Find<Void_t>("igSpacing");
            g_separatorText = Find<SeparatorText_t>("igSeparatorText");
            g_sameLine = Find<SameLine_t>("igSameLine");
            auto addSectionItem = Find<AddSectionItem_t>("AddSectionItem");

            if (!g_textWrappedV || !g_textColoredV || !g_button || !g_checkbox || !g_sliderInt || !g_inputInt ||
                !g_pushID || !g_popID || !g_pushItemWidth || !g_popItemWidth || !g_sliderFloat ||
                !g_colorEdit3 || !g_spacing || !g_separatorText || !g_sameLine || !addSectionItem) {
                Log("ERROR: this version of SKSE Menu Framework is missing something we need, so there is no in-game menu.");
                return;
            }

            addSectionItem("Chest Glow/Settings", Render);
            Log("Added the Chest Glow page to SKSE Menu Framework.");
        }
    }

    void OnDataLoaded()
    {
        LoadSettings();
        if (!CreateShaders()) {
            return;
        }
        auto* events = RE::ScriptEventSourceHolder::GetSingleton();
        if (!events) {
            Log("ERROR: could not access game events.");
            return;
        }
        events->AddEventSink<RE::TESObjectLoadedEvent>(&g_objectLoadedListener);
        events->AddEventSink<RE::TESContainerChangedEvent>(&g_containerChangedListener);
        events->AddEventSink<RE::TESActivateEvent>(&g_activateListener);
        events->AddEventSink<RE::TESCellAttachDetachEvent>(&g_cellAttachListener);
        StartScanner();
        Log("Chest Glow is running.");

        Menu::Register();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            g_inWorld = false;  // pause scanning while a save loads
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            g_scansToSkip = 2;  // give the game a few seconds to settle
            g_logNextScan = true;
            g_inWorld = true;
            break;
        default:
            break;
        }
    });

    return true;
}
