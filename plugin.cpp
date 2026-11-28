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

        bool  seamLight = true;           // true = glowing seam with rays, false = whole chest glows
        float seamGlowStrength = 2.0f;    // how bright the seam glow and rays are
        bool  seamPulse = true;           // fade the seam glow up and down
        float pulseTime = 4.0f;           // seconds to fade from off to full (and back)
        bool  raysDance = true;           // rays sway and stretch on their own
        float danceSpeed = 1.0f;          // how fast the rays move
        float danceAmount = 1.0f;         // how far the rays move
        bool  lightSurroundings = false;  // also shine real light onto the floor and walls
        float lightRadius = 200.0f;       // how far that light reaches (game units)
        float lightBrightness = 1.5f;     // how bright that light is
        float seamHeight = 0.78f;       // where the seam is, as a share of the chest's height (0 = bottom, 1 = top)

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
        if (auto it = values.find("glowstyle"); it != values.end()) {
            g_settings.seamLight = ToLower(it->second) != "whole";
        }
        g_settings.seamGlowStrength = std::clamp(getFloat("seamglowstrength", g_settings.seamGlowStrength), 0.1f, 10.0f);
        g_settings.lightSurroundings = getBool("lightsurroundings", g_settings.lightSurroundings);
        g_settings.seamPulse = getBool("seampulse", g_settings.seamPulse);
        g_settings.pulseTime = std::clamp(getFloat("pulsetime", g_settings.pulseTime), 0.5f, 15.0f);
        g_settings.raysDance = getBool("raysdance", g_settings.raysDance);
        g_settings.danceSpeed = std::clamp(getFloat("dancespeed", g_settings.danceSpeed), 0.1f, 5.0f);
        g_settings.danceAmount = std::clamp(getFloat("danceamount", g_settings.danceAmount), 0.0f, 3.0f);
        g_settings.lightRadius = std::clamp(getFloat("lightradius", g_settings.lightRadius), 30.0f, 1000.0f);
        g_settings.lightBrightness = std::clamp(getFloat("lightbrightness", g_settings.lightBrightness), 0.1f, 10.0f);
        g_settings.seamHeight = std::clamp(getFloat("seamheight", g_settings.seamHeight), 0.0f, 1.0f);
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
             << "; Seam = glowing line and rays from the chest's lid seam. Whole = the whole chest glows.\n"
             << "GlowStyle = " << (g_settings.seamLight ? "Seam" : "Whole") << "\n"
             << "; How bright the seam glow and rays are.\n"
             << "SeamGlowStrength = " << g_settings.seamGlowStrength << "\n"
             << "; Fade the seam glow from off to full and back. PulseTime = seconds for one fade.\n"
             << "SeamPulse = " << b(g_settings.seamPulse) << "\n"
             << "PulseTime = " << g_settings.pulseTime << "\n"
             << "; Make the light rays sway and stretch. Speed: 1 = normal. Amount: 1 = normal, 0 = still.\n"
             << "RaysDance = " << b(g_settings.raysDance) << "\n"
             << "DanceSpeed = " << g_settings.danceSpeed << "\n"
             << "DanceAmount = " << g_settings.danceAmount << "\n"
             << "; Also shine real colored light onto the floor and walls around the chest.\n"
             << "LightSurroundings = " << b(g_settings.lightSurroundings) << "\n"
             << "; That light: how far it reaches (about 70 = 1 meter) and how bright it is.\n"
             << "LightRadius = " << g_settings.lightRadius << "\n"
             << "LightBrightness = " << g_settings.lightBrightness << "\n"
             << "; Where the seam is, as a share of the chest's height (0 = bottom, 1 = top).\n"
             << "SeamHeight = " << g_settings.seamHeight << "\n"
             << "; Whole-chest glow: how bright the glow is, from 0.0 (faint) to 1.0 (strong).\n"
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

    // ---------------------------------------------------------------
    //  Seam light: a real colored light placed at the chest's lid seam.
    //  It's attached to the chest's 3D model, so it's never saved into
    //  your save file and disappears with the chest's model.
    // ---------------------------------------------------------------
    RE::TESObjectLIGH* g_lightForms[TIER_COUNT] = {};

    void ApplyLightLook(RE::TESObjectLIGH* a_light, RE::Color a_color)
    {
        if (!a_light) {
            return;
        }
        a_light->data.color = a_color;
        a_light->data.radius = static_cast<std::uint32_t>(g_settings.lightRadius);
        a_light->data.fallofExponent = 1.0f;
        a_light->fade = g_settings.lightBrightness;
    }

    bool CreateLightForms()
    {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectLIGH>();
        if (!factory) {
            return false;
        }
        for (int i = 0; i < TIER_COUNT; ++i) {
            g_lightForms[i] = static_cast<RE::TESObjectLIGH*>(factory->Create());
            ApplyLightLook(g_lightForms[i], g_settings.tiers[i].color);
        }
        return std::all_of(std::begin(g_lightForms), std::end(g_lightForms), [](auto* l) { return l != nullptr; });
    }

    // The game's own functions (same ones Light Placer uses).
    RE::NiAVObject* GenDynamicLight(RE::TESObjectLIGH* a_light, RE::TESObjectREFR* a_ref, RE::NiNode* a_node)
    {
        using func_t = RE::NiAVObject* (*)(RE::TESObjectLIGH*, RE::TESObjectREFR*, RE::NiNode*, char, char, char);
        static REL::Relocation<func_t> func{ RELOCATION_ID(17208, 17610) };
        return func(a_light, a_ref, a_node, true, true, false);
    }

    void RemoveFromLighting(RE::NiAVObject* a_light)
    {
        static REL::Relocation<void**> shaderState{ RELOCATION_ID(513211, 390951) };  // BSShaderManager::State
        auto* shadowSceneNode = shaderState.get() ? shaderState.get()[0] : nullptr;   // its first member
        if (!shadowSceneNode || !a_light) {
            return;
        }
        using func_t = void (*)(void*, RE::NiAVObject*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(99697, 106331) };  // ShadowSceneNode::RemoveLight
        func(shadowSceneNode, a_light);
    }

    // One light ray on a chest, and how it moves.
    struct Ray
    {
        RE::NiPointer<RE::NiNode> node;
        float                     yaw = 0.0f;     // which way it faces (outward from its side)
        float                     tilt = 0.0f;    // how far its top leans away from the chest
        float                     lean = 0.0f;    // fans out toward the chest's ends
        float                     height = 1.0f;  // its normal size
        float                     phase = 0.0f;   // so rays don't all move together
        float                     rate = 1.0f;    // each ray moves at a slightly different pace
    };

    struct SeamLight
    {
        RE::NiPointer<RE::NiNode>     node;
        RE::NiPointer<RE::NiAVObject> light;
        RE::NiAVObject*               root = nullptr;  // the chest model it was attached to
        int                           tier = NO_TIER;
        std::vector<Ray>              rays;
        std::vector<RE::BSEffectShaderMaterial*> materials;  // for the pulse
    };
    std::unordered_map<RE::FormID, SeamLight> g_seamLights;  // main thread only

    void RemoveSeamLight(RE::FormID a_chestID)
    {
        auto it = g_seamLights.find(a_chestID);
        if (it == g_seamLights.end()) {
            return;
        }
        auto& entry = it->second;
        if (entry.light) {
            RemoveFromLighting(entry.light.get());
        }
        if (entry.node && entry.node->parent) {
            entry.node->parent->DetachChild(entry.node.get());
        }
        g_seamLights.erase(it);
    }

    void RemoveAllSeamLights()
    {
        std::vector<RE::FormID> ids;
        for (auto& [id, entry] : g_seamLights) {
            ids.push_back(id);
        }
        for (auto id : ids) {
            RemoveSeamLight(id);
        }
    }

    // Is this chest's seam light still in place? (Its model may have reloaded.)
    bool SeamLightValid(RE::TESObjectREFR* a_chest, const SeamLight& a_entry)
    {
        return a_entry.node && a_entry.node->parent && a_chest->Get3D() == a_entry.root;
    }

    // ---- The glowing seam model (meshes\ChestGlow\Seam<Tier>.nif) ----
    const char* SEAM_MODELS[TIER_COUNT] = {
        "ChestGlow\\SeamLegendary.nif",
        "ChestGlow\\SeamEpic.nif",
        "ChestGlow\\SeamCommon.nif",
    };
    const char* RAY_MODELS[TIER_COUNT] = {
        "ChestGlow\\SeamRayLegendary.nif",
        "ChestGlow\\SeamRayEpic.nif",
        "ChestGlow\\SeamRayCommon.nif",
    };
    RE::NiPointer<RE::NiNode> g_seamTemplates[TIER_COUNT];
    RE::NiPointer<RE::NiNode> g_rayTemplates[TIER_COUNT];
    bool                      g_seamModelMissing = false;
    bool                      g_rayModelMissing = false;

    // The game's own model loader and copier (same ones Light Placer uses).
    RE::NiNode* LoadModel(const char* a_path)
    {
        struct Args  // BSModelDB::DBTraits::ArgsType
        {
            std::uint32_t lodMult{ 0 };
            std::uint32_t texLoadLevel{ 3 };
            bool          unk8{ true };
            bool          unk9{ false };
            bool          unkA{ true };
            bool          postProcess{ true };
        };
        static_assert(sizeof(Args) == 0xC);
        using func_t = std::uint32_t (*)(const char*, RE::NiPointer<RE::NiNode>&, const Args&);
        static REL::Relocation<func_t> func{ RELOCATION_ID(74040, 75782) };  // BSModelDB::Demand

        RE::NiPointer<RE::NiNode> model;
        const Args                args{};
        if (func(a_path, model, args) != 0 || !model) {
            return nullptr;
        }
        auto* result = model.get();
        result->IncRefCount();  // keep it alive after 'model' goes away
        return result;
    }

    RE::NiAVObject* CloneModel(RE::NiAVObject* a_model)
    {
        using func_t = RE::NiObject* (*)(RE::NiObject*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(68835, 70187) };  // NiObject::Clone
        return static_cast<RE::NiAVObject*>(func(a_model));
    }

    RE::NiNode* GetSeamTemplate(int a_tier)
    {
        if (!g_seamTemplates[a_tier] && !g_seamModelMissing) {
            auto* model = LoadModel(SEAM_MODELS[a_tier]);
            if (!model) {
                g_seamModelMissing = true;
                Log(std::string("ERROR: could not load meshes\\") + SEAM_MODELS[a_tier] +
                    ". Is the Chest Glow assets folder installed? Using whole-chest glow instead.");
                return nullptr;
            }
            g_seamTemplates[a_tier].reset(model);
            model->DecRefCount();  // the NiPointer holds it now
            Log(std::string("Loaded seam model ") + SEAM_MODELS[a_tier]);
        }
        return g_seamTemplates[a_tier].get();
    }

    RE::NiNode* GetRayTemplate(int a_tier)
    {
        if (!g_rayTemplates[a_tier] && !g_rayModelMissing) {
            auto* model = LoadModel(RAY_MODELS[a_tier]);
            if (!model) {
                g_rayModelMissing = true;
                Log(std::string("ERROR: could not load meshes\\") + RAY_MODELS[a_tier] + ". The seam will glow without rays.");
                return nullptr;
            }
            g_rayTemplates[a_tier].reset(model);
            model->DecRefCount();
            Log(std::string("Loaded ray model ") + RAY_MODELS[a_tier]);
        }
        return g_rayTemplates[a_tier].get();
    }

    // ---- Rotation helpers (for placing and moving the rays) ----
    RE::NiMatrix3 RotX(float a)
    {
        RE::NiMatrix3 m;
        const float   c = std::cos(a), s = std::sin(a);
        m.entry[1][1] = c, m.entry[1][2] = -s, m.entry[2][1] = s, m.entry[2][2] = c;
        return m;
    }
    RE::NiMatrix3 RotY(float a)
    {
        RE::NiMatrix3 m;
        const float   c = std::cos(a), s = std::sin(a);
        m.entry[0][0] = c, m.entry[0][2] = s, m.entry[2][0] = -s, m.entry[2][2] = c;
        return m;
    }
    RE::NiMatrix3 RotZ(float a)
    {
        RE::NiMatrix3 m;
        const float   c = std::cos(a), s = std::sin(a);
        m.entry[0][0] = c, m.entry[0][1] = -s, m.entry[1][0] = s, m.entry[1][1] = c;
        return m;
    }

    float SecondsNow()
    {
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        return std::chrono::duration<float>(clock::now() - start).count();
    }

    // Puts a ray in its pose for time a_time: swaying side to side and stretching up and down.
    void PoseRay(Ray& a_ray, float a_time)
    {
        float sway = 0.0f;
        float stretch = 1.0f;
        if (g_settings.raysDance && g_settings.danceAmount > 0.0f) {
            const float t = a_time * g_settings.danceSpeed * a_ray.rate;
            const float amount = g_settings.danceAmount;
            // Two waves mixed together so the motion doesn't look mechanical.
            sway = amount * 0.22f * (std::sin(t * 1.7f + a_ray.phase) * 0.7f + std::sin(t * 2.9f + a_ray.phase * 1.3f) * 0.3f);
            stretch = 1.0f + amount * 0.25f * std::sin(t * 1.3f + a_ray.phase * 0.7f);
        }
        auto& local = a_ray.node->local;
        local.rotate = RotZ(a_ray.yaw) * RotX(a_ray.tilt) * RotY(a_ray.lean + sway);
        local.scale = std::max(0.1f, a_ray.height * stretch);
    }

    // Glow brightness for time a_time: a smooth fade from off to full and back.
    float PulseBrightness(float a_time)
    {
        if (!g_settings.seamPulse) {
            return g_settings.seamGlowStrength;
        }
        constexpr float pi = 3.14159265f;
        const float     cycle = 2.0f * g_settings.pulseTime;  // up and back down
        const float     wave = 0.5f - 0.5f * std::cos(2.0f * pi * std::fmod(a_time, cycle) / cycle);
        return g_settings.seamGlowStrength * wave;
    }

    // Recolors every glowing part of a seam model (and lists its materials, for the pulse).
    void ColorSeamModel(RE::NiAVObject* a_model, RE::Color a_color,
        std::vector<RE::BSEffectShaderMaterial*>* a_materials = nullptr)
    {
        RE::BSVisit::TraverseScenegraphGeometries(a_model, [&](RE::BSGeometry* a_geometry) {
            auto& props = a_geometry->GetGeometryRuntimeData().properties;
            auto* shader = netimmerse_cast<RE::BSEffectShaderProperty*>(props[RE::BSGeometry::States::kEffect].get());
            if (shader && shader->material) {
                auto* material = static_cast<RE::BSEffectShaderMaterial*>(shader->material);
                material->baseColor.red = a_color.red / 255.0f;
                material->baseColor.green = a_color.green / 255.0f;
                material->baseColor.blue = a_color.blue / 255.0f;
                material->baseColor.alpha = 1.0f;
                material->baseColorScale = PulseBrightness(SecondsNow());
                if (a_materials) {
                    a_materials->push_back(material);
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }

    // Adds the seam glow (and, if turned on, a real light) to a chest. Returns false if
    // the seam model isn't available, so the caller can fall back to the whole-chest glow.
    bool AddSeamLight(RE::TESObjectREFR* a_chest, int a_tier)
    {
        auto* model = a_chest->Get3D();
        auto* root = model ? model->AsNode() : nullptr;
        auto* base = a_chest->GetBaseObject();
        auto* seamTemplate = GetSeamTemplate(a_tier);
        if (!root || !base || !seamTemplate) {
            return false;
        }

        // Find the seam from the chest's size: centered, at SeamHeight of its height.
        const auto& bounds = base->boundData;
        float minX = bounds.boundMin.x, maxX = bounds.boundMax.x;
        float minY = bounds.boundMin.y, maxY = bounds.boundMax.y;
        float minZ = bounds.boundMin.z, maxZ = bounds.boundMax.z;
        if (maxX <= minX || maxY <= minY || maxZ <= minZ) {
            minX = -30.0f, maxX = 30.0f, minY = -16.0f, maxY = 16.0f, minZ = 0.0f, maxZ = 34.0f;  // unknown: a normal chest
        }

        // 'holder' sits at the seam. The light (if any) goes here.
        auto* holder = RE::NiNode::Create(32);  // room for the glow, the rays and the light
        if (!holder) {
            return false;
        }
        holder->name = "ChestGlowSeam";
        holder->local.translate.x = (minX + maxX) * 0.5f;
        holder->local.translate.y = (minY + maxY) * 0.5f;
        holder->local.translate.z = minZ + (maxZ - minZ) * g_settings.seamHeight;

        SeamLight entry;
        constexpr float margin = 0.6f;  // sit just outside the chest's surface

        // 'stretch' stretches the 1x1 seam model to the chest's width and depth
        // (heights stay in normal units, so the glow line keeps the same thickness).
        auto* stretch = RE::NiNode::Create(1);
        auto* glow = CloneModel(seamTemplate);
        if (stretch && glow) {
            stretch->name = "ChestGlowSeamStretch";
            stretch->local.rotate.entry[0][0] = (maxX - minX) + 2.0f * margin;
            stretch->local.rotate.entry[1][1] = (maxY - minY) + 2.0f * margin;
            stretch->local.rotate.entry[2][2] = 1.0f;
            ColorSeamModel(glow, g_settings.tiers[a_tier].color, &entry.materials);
            stretch->AttachChild(glow, true);
            holder->AttachChild(stretch, true);
        }

        // Light rays: separate copies placed along each side, so each can move on its own.
        if (auto* rayTemplate = GetRayTemplate(a_tier)) {
            constexpr float pi = 3.14159265f;
            const float     halfX = (maxX - minX) * 0.5f + margin + 0.3f;
            const float     halfY = (maxY - minY) * 0.5f + margin + 0.3f;
            const float     seed = static_cast<float>(a_chest->GetFormID() % 997);
            struct Side
            {
                float halfLength;  // half the side's length
                float yaw;         // turns the ray to face outward from this side
            };
            const Side sides[4] = { { halfX, 0.0f }, { halfX, pi }, { halfY, pi * 0.5f }, { halfY, -pi * 0.5f } };

            for (int side = 0; side < 4; ++side) {
                const float half = sides[side].halfLength;
                const int   count = std::clamp(static_cast<int>(std::lround(half * 2.0f / 11.0f)), 2, 7);
                for (int i = 0; i < count; ++i) {
                    auto* node = RE::NiNode::Create(1);
                    auto* ray = CloneModel(rayTemplate);
                    if (!node || !ray) {
                        continue;
                    }
                    const float along = -half + (i + 0.5f) * (2.0f * half / count);  // position along the side
                    const float t = along / half;                                    // -1 .. 1
                    // Position: on the side, facing outward ("out" is -Y before turning).
                    const float a = sides[side].yaw;
                    const float px = along, py = -(side < 2 ? halfY : halfX);
                    node->name = "ChestGlowRay";
                    node->local.translate.x = px * std::cos(a) - py * std::sin(a);
                    node->local.translate.y = px * std::sin(a) + py * std::cos(a);
                    node->local.translate.z = 0.0f;

                    Ray r;
                    r.node.reset(node);
                    r.yaw = a;
                    r.tilt = 0.35f;                                         // lean away from the chest
                    r.lean = t * 0.35f;                                     // fan out toward the ends
                    r.height = 0.75f + 0.25f * std::cos(seed + i * 2.1f);  // vary heights a bit
                    r.phase = seed * 0.37f + side * 1.9f + i * 2.3f;
                    r.rate = 0.8f + 0.4f * std::fabs(std::sin(seed + side * 3.1f + i * 1.7f));
                    PoseRay(r, SecondsNow());

                    ColorSeamModel(ray, g_settings.tiers[a_tier].color, &entry.materials);
                    node->AttachChild(ray, true);
                    holder->AttachChild(node, true);
                    entry.rays.push_back(std::move(r));
                }
            }
        }

        root->AttachChild(holder, true);
        RE::NiUpdateData update{};
        holder->Update(update);

        RE::NiAVObject* light = nullptr;
        if (g_settings.lightSurroundings && g_lightForms[a_tier]) {
            light = GenDynamicLight(g_lightForms[a_tier], a_chest, holder);
            holder->Update(update);
        }

        entry.node.reset(holder);
        entry.light.reset(light);
        entry.root = model;
        entry.tier = a_tier;
        g_seamLights[a_chest->GetFormID()] = std::move(entry);
        return true;
    }

    // ---- Animation: runs about 30 times a second on the game's main thread ----
    void AnimateSeams()
    {
        if (g_seamLights.empty()) {
            return;
        }
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
            return;  // menus open: hold still
        }
        const float time = SecondsNow();
        const float brightness = PulseBrightness(time);
        const bool  dance = g_settings.raysDance && g_settings.danceAmount > 0.0f;
        RE::NiUpdateData update{};

        for (auto& [id, entry] : g_seamLights) {
            if (!entry.node || !entry.node->parent) {
                continue;  // its chest model went away; the scan will tidy it up
            }
            for (auto* material : entry.materials) {
                material->baseColorScale = brightness;
            }
            if (dance) {
                for (auto& ray : entry.rays) {
                    PoseRay(ray, time);
                    ray.node->Update(update);
                }
            }
        }
    }

    std::atomic<bool> g_animateQueued{ false };

    void StartAnimator()
    {
        std::thread([]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                if (!g_settings.seamLight || (!g_settings.seamPulse && !g_settings.raysDance)) {
                    continue;
                }
                if (g_animateQueued.exchange(true)) {
                    continue;  // the last one hasn't run yet
                }
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() {
                        g_animateQueued = false;
                        AnimateSeams();
                    });
                } else {
                    g_animateQueued = false;
                }
            }
        }).detach();
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
        if (a_chest) {
            RemoveSeamLight(a_chest->GetFormID());
        }
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
            if (!g_settings.seamLight || !AddSeamLight(a_chest, a_tier)) {
                PlayShader(a_chest, g_shaders[a_tier], -1.0f);  // -1 = glow until we remove it
            }
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

        // Seam lights (dropping any whose chest model went away).
        std::vector<RE::FormID> stale;
        for (auto& [id, entry] : g_seamLights) {
            auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (chest && SeamLightValid(chest, entry)) {
                result[chest] = entry.tier;
            } else {
                stale.push_back(id);
            }
        }
        for (auto id : stale) {
            RemoveSeamLight(id);
        }
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
                    ApplyLightLook(g_lightForms[i], g_settings.tiers[i].color);
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
                        RemoveSeamLight(id);
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

            g_separatorText("Glow style");
            changed |= g_checkbox("Glowing seam with rays (untick: whole chest glows)", &g_settings.seamLight);

            g_separatorText("Seam glow");
            changed |= g_sliderFloat("Seam glow brightness", &g_settings.seamGlowStrength, 0.1f, 10.0f, "%.1f", 0);
            changed |= g_checkbox("Pulse (fade on and off)", &g_settings.seamPulse);
            if (g_settings.seamPulse) {
                g_sliderFloat("Pulse time (seconds from off to full)", &g_settings.pulseTime, 0.5f, 15.0f, "%.1f", 0);
            }
            changed |= g_checkbox("Dancing rays", &g_settings.raysDance);
            if (g_settings.raysDance) {
                g_sliderFloat("Dance speed", &g_settings.danceSpeed, 0.1f, 5.0f, "%.1f", 0);
                g_sliderFloat("Dance amount", &g_settings.danceAmount, 0.0f, 3.0f, "%.1f", 0);
            }
            changed |= g_sliderFloat("Seam height (0 = bottom, 1 = top)", &g_settings.seamHeight, 0.0f, 1.0f, "%.2f", 0);
            changed |= g_checkbox("Also light up the floor and walls", &g_settings.lightSurroundings);
            if (g_settings.lightSurroundings) {
                changed |= g_sliderFloat("Light reach", &g_settings.lightRadius, 30.0f, 1000.0f, "%.0f", 0);
                changed |= g_sliderFloat("Light brightness", &g_settings.lightBrightness, 0.1f, 10.0f, "%.1f", 0);
            }

            g_separatorText("Whole-chest glow");
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
        if (!CreateLightForms()) {
            Log("ERROR: could not create the surrounding lights. That option won't work.");
            g_settings.lightSurroundings = false;
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
        StartAnimator();
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
            RemoveAllSeamLights();
            g_nearbyContainers.clear();
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
