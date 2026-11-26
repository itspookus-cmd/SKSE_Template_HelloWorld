// =====================================================================
//  Chest Glow - an SKSE plugin for Skyrim
//
//  What it does, in plain words:
//    1. Whenever a chest appears in the world, look inside it.
//    2. If it holds a "Legendary" weapon, make the chest glow yellow.
//       Otherwise, if it holds an "Epic" weapon, make it glow purple.
//    3. Whenever items move in or out of a chest, look again, so the
//       glow goes away once you take the weapon.
//
//  All the numbers and colors live in ChestGlow.ini, so you can tweak
//  them without rebuilding.
// =====================================================================

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace
{
    // ---------------------------------------------------------------
    //  A tiny log file: Data/SKSE/Plugins/ChestGlow.log
    //  (With Mod Organizer 2 it lands in the "overwrite" folder.)
    // ---------------------------------------------------------------
    void Log(const std::string& a_text)
    {
        static std::ofstream file("Data/SKSE/Plugins/ChestGlow.log", std::ios::trunc);
        if (file) {
            file << a_text << std::endl;
        }
    }

    // ---------------------------------------------------------------
    //  Settings (defaults here; ChestGlow.ini overrides them)
    // ---------------------------------------------------------------
    struct Settings
    {
        int  legendaryMinValue = 2000;  // weapons worth this much gold or more are Legendary
        bool artifactsAreLegendary = true;  // Daedric artifacts always count as Legendary
        int  epicMinValue = 500;  // enchanted weapons worth this much or more are Epic
        bool epicMustBeEnchanted = true;

        RE::Color legendaryColor{ 255, 200, 0, 255 };  // yellow
        RE::Color epicColor{ 170, 60, 255, 255 };      // purple
        float     glowStrength = 1.0f;   // 0.0 (faint) to 1.0 (strong)
        float     pulseSpeed = 1.0f;     // how fast the glow breathes; 0 = steady

        bool showConsoleMessages = false;  // print to the ~ console when a chest glows

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

    void LoadSettings()
    {
        std::ifstream file("Data/SKSE/Plugins/ChestGlow.ini");
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
            auto key = Trim(line.substr(0, eq));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            values[key] = Trim(line.substr(eq + 1));
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
            auto v = it->second;
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return v == "1" || v == "true" || v == "yes" || v == "on";
        };
        auto getColor = [&](const char* a_key, RE::Color a_fallback) {
            auto it = values.find(a_key);
            return it == values.end() ? a_fallback : ParseColor(it->second, a_fallback);
        };

        g_settings.legendaryMinValue = getInt("legendaryminvalue", g_settings.legendaryMinValue);
        g_settings.artifactsAreLegendary = getBool("artifactsarelegendary", g_settings.artifactsAreLegendary);
        g_settings.epicMinValue = getInt("epicminvalue", g_settings.epicMinValue);
        g_settings.epicMustBeEnchanted = getBool("epicmustbeenchanted", g_settings.epicMustBeEnchanted);
        g_settings.legendaryColor = getColor("legendarycolor", g_settings.legendaryColor);
        g_settings.epicColor = getColor("epiccolor", g_settings.epicColor);
        g_settings.glowStrength = std::clamp(getFloat("glowstrength", g_settings.glowStrength), 0.0f, 1.0f);
        g_settings.pulseSpeed = std::max(0.0f, getFloat("pulsespeed", g_settings.pulseSpeed));
        g_settings.showConsoleMessages = getBool("showconsolemessages", g_settings.showConsoleMessages);
        if (auto it = values.find("glowtexture"); it != values.end()) {
            g_settings.glowTexture = it->second;
        }

        Log("Settings loaded. Legendary >= " + std::to_string(g_settings.legendaryMinValue) +
            " gold, Epic >= " + std::to_string(g_settings.epicMinValue) + " gold.");
    }

    // ---------------------------------------------------------------
    //  Rarity rules
    // ---------------------------------------------------------------
    enum class Rarity
    {
        kNone,
        kEpic,
        kLegendary
    };

    Rarity GetChestRarity(RE::TESObjectREFR* a_chest)
    {
        Rarity best = Rarity::kNone;

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

            const int  value = entry->GetValue();
            const bool enchanted = entry->IsEnchanted() || weapon->formEnchanting != nullptr;
            const bool artifact = g_settings.artifactsAreLegendary && weapon->HasKeywordString("DaedricArtifact");

            if (artifact || value >= g_settings.legendaryMinValue) {
                return Rarity::kLegendary;  // can't do better than this, stop looking
            }
            if (value >= g_settings.epicMinValue && (enchanted || !g_settings.epicMustBeEnchanted)) {
                best = Rarity::kEpic;
            }
        }
        return best;
    }

    // ---------------------------------------------------------------
    //  The two glow effects (built in code, no Creation Kit needed)
    // ---------------------------------------------------------------
    RE::TESEffectShader* g_legendaryShader = nullptr;
    RE::TESEffectShader* g_epicShader = nullptr;

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
            std::string name = original;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

        // Start from the vanilla effect's settings and textures...
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

        // Color everything in our chosen color.
        d.fillTextureEffectColorKey1 = a_color;
        d.fillTextureEffectColorKey2 = a_color;
        d.fillTextureEffectColorKey3 = a_color;
        d.edgeEffectColor = a_color;
        d.edgeColor = a_color;

        // Fade in quickly, then stay on (persistent) with a gentle pulse.
        const float strength = g_settings.glowStrength;
        d.fillTextureEffectAlphaFadeInTime = 0.5f;
        d.fillTextureEffectFullAlphaTime = 0.0f;
        d.fillTextureEffectAlphaFadeOutTime = 0.5f;
        d.fillTextureEffectFullAlphaRatio = strength;
        d.fillTextureEffectPersistentAlphaRatio = strength * 0.6f;
        d.fillTextureEffectAlphaPulseAmplitude = g_settings.pulseSpeed > 0.0f ? strength * 0.3f : 0.0f;
        d.fillTextureEffectAlphaPulseFrequency = g_settings.pulseSpeed;

        d.edgeEffectAlphaFadeInTime = 0.5f;
        d.edgeEffectFullAlphaTime = 0.0f;
        d.edgeEffectAlphaFadeOutTime = 0.5f;
        d.edgeEffectFullAlphaRatio = strength;
        d.edgeEffectPersistentAlphaRatio = strength;
        d.edgeEffectAlphaPulseAmplitude = g_settings.pulseSpeed > 0.0f ? strength * 0.3f : 0.0f;
        d.edgeEffectAlphaPulseFrequency = g_settings.pulseSpeed;
        if (d.edgeEffectFallOff <= 0.0f) {
            d.edgeEffectFallOff = 1.0f;
        }
        if (d.colorScale <= 0.0f) {
            d.colorScale = 1.0f;
        }

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
        if (!g_settings.glowTexture.empty()) {
            Log("Using your GlowTexture from ChestGlow.ini instead: " + g_settings.glowTexture);
        }

        g_legendaryShader = MakeGlowShader(templ, g_settings.legendaryColor);
        g_epicShader = MakeGlowShader(templ, g_settings.epicColor);
        if (!g_legendaryShader || !g_epicShader) {
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
            if (effect && (effect->effectData == g_legendaryShader || effect->effectData == g_epicShader)) {
                auto target = effect->target.get();
                if (target && target.get() == a_chest) {
                    effect->finished = true;
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    void UpdateChest(RE::TESObjectREFR* a_chest)
    {
        if (!g_legendaryShader || !g_epicShader || !IsChest(a_chest)) {
            return;
        }

        RemoveGlow(a_chest);

        if (!a_chest->Is3DLoaded()) {
            return;  // not visible yet; we'll check again when it loads
        }

        const Rarity rarity = GetChestRarity(a_chest);
        RE::TESEffectShader* shader = nullptr;
        if (rarity == Rarity::kLegendary) {
            shader = g_legendaryShader;
        } else if (rarity == Rarity::kEpic) {
            shader = g_epicShader;
        }

        if (shader) {
            PlayShader(a_chest, shader, -1.0f);  // -1 = glow until we remove it
            if (g_settings.showConsoleMessages) {
                RE::ConsoleLog::GetSingleton()->Print("[ChestGlow] %s chest: %s",
                    rarity == Rarity::kLegendary ? "Legendary" : "Epic", a_chest->GetName());
            }
        }
    }

    // Run the check a moment later, on the game's main thread (the safe way).
    void QueueUpdate(RE::FormID a_chestID)
    {
        if (a_chestID == 0) {
            return;
        }
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([a_chestID]() {
                UpdateChest(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_chestID));
            });
        }
    }

    // ---------------------------------------------------------------
    //  Listening to game events
    // ---------------------------------------------------------------

    // Fires when an object's 3D model loads, e.g. you walk into a room with a chest.
    class ObjectLoadedListener : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
        {
            if (a_event && a_event->loaded) {
                QueueUpdate(a_event->formID);
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
                QueueUpdate(a_event->oldContainer);
                QueueUpdate(a_event->newContainer);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    ObjectLoadedListener     g_objectLoadedListener;
    ContainerChangedListener g_containerChangedListener;

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
        Log("Chest Glow is running.");
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* message) {
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            OnDataLoaded();
        }
    });

    return true;
}
