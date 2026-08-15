#include "quartz/client/Functions.hpp"
#include "quartz/client/runtime/QuickJS.hpp"
#include "quartz/client/runtime/RuntimeBindingEngine.hpp"
#include <fstream>

namespace quartz::client
{
    std::string_view runtimeTerrariaAstrofluxMigrationScript() noexcept
    {
        static constexpr std::string_view Source = R"JS(// Migrated from Quartz runtime material bindings v11.
// Low-level native/object bindings remain graph nodes because they already handle process reattach,
// register-relative signature capture and object layout. Derived state + shader automation lives here.

if (!q.state.configured) {
    q.graph.ensureBinding("Native::Terraria.Player*::LocalInstance", {
        source: "NativeAddress", signal: 1, writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60,
        processName: "Terraria.bin.x86_64", rebindMode: "ExecutableExact",
        rebindPattern: "/home/raony/.local/share/Steam/steamapps/common/Terraria/Terraria.bin.x86_64",
        addressMode: "Signature", signatureExecutableOnly: true,
        signature: "49 8B C7 48 63 80 1C 08 00 00 F3 0F 2A C0 49 63 87 18 08 00 00 F3 0F 2A C8 F3 0F 5E C1 F3 0F 10 0D ?? ?? ?? ?? F3 0F 59 C1",
        signatureResolve: "RegisterRelativeCapture", signatureInstructionSize: 7, signatureRegister: "r15",
        signatureRegisterDisplacementOffset: 3, signatureDisplacementType: "I32", signatureCaptureTimeoutSeconds: 10,
        priority: 0, order: 0, group: "Terraria / Address Resolution"
    });
    q.graph.ensureBinding("Native::Terraria.World::Active", {
        source: "NativeProcess", valueType: "bool", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60,
        processName: "Terraria.bin.x86_64", rebindMode: "ExecutableExact",
        rebindPattern: "/home/raony/.local/share/Steam/steamapps/common/Terraria/Terraria.bin.x86_64",
        addressMode: "Signature", signatureExecutableOnly: true,
        signature: "F7 00 01 00 00 00 74 08 66 66 90 E8 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 0F B6 00 85 C0 0F 84 ?? ?? ?? ?? B8 ?? ?? ?? ?? 48 0F B6 00 85 C0 75 ??",
        signatureResolve: "Address32AtOffset", signatureResultOffset: 17, signatureInstructionSize: 5,
        priority: 5, order: 0, group: "Terraria / Address Resolution"
    });
    q.graph.ensureObject("Terraria.Player", {description: "Native model for the local Terraria Player object", fields: [
        {name: "Reserved_0000_0813", type: "FillerCustom", customFillerBytes: 0x814},
        {name: "statLifeMax2", type: "U32"}, {name: "Reserved_0818_081B", type: "Filler4"}, {name: "statLife", type: "U32"}
    ]});
    q.graph.ensurePointer("Pointer::Terraria.Player::Local", {descriptor: "Terraria.Player", baseBinding: "Native::Terraria.Player*::LocalInstance", group: "Terraria / Pointer Instances"});
    q.graph.ensureBinding("Model::Terraria.Player::statLife", {source: "ObjectField", object: "Terraria.Player", pointer: "Pointer::Terraria.Player::Local", field: "statLife", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Terraria / Player Model"});
    q.graph.ensureBinding("Model::Terraria.Player::statLifeMax2", {source: "ObjectField", object: "Terraria.Player", pointer: "Pointer::Terraria.Player::Local", field: "statLifeMax2", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Terraria / Player Model"});

    q.graph.ensureBinding("Native::Astroflux.PlayerShip*::LocalInstance", {source: "NativeAddress", signal: 1, writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, processName: "Astroflux.exe", rebindMode: "NameExact", module: "Astroflux.exe", addressMode: "AddressChain", address: "+0x6137C28", priority: 0, group: "Astroflux / Address Resolution"});
    q.graph.ensureObject("Astroflux.PlayerShip", {description: "Local Astroflux PlayerShip layout", fields: [
        {name: "shieldHp", type: "I32", manualOffset: true, offset: 0xAC},
        {name: "shieldHpMax", type: "I32", manualOffset: true, offset: 0xB0}
    ]});
    q.graph.ensurePointer("Pointer::Astroflux.PlayerShip::Local", {descriptor: "Astroflux.PlayerShip", baseBinding: "Native::Astroflux.PlayerShip*::LocalInstance", group: "Astroflux / Pointer Instances"});
    q.graph.ensureBinding("Model::Astroflux.PlayerShip::shieldHp", {source: "ObjectField", object: "Astroflux.PlayerShip", pointer: "Pointer::Astroflux.PlayerShip::Local", field: "shieldHp", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Astroflux / PlayerShip Model"});
    q.graph.ensureBinding("Model::Astroflux.PlayerShip::shieldHpMax", {source: "ObjectField", object: "Astroflux.PlayerShip", pointer: "Pointer::Astroflux.PlayerShip::Local", field: "shieldHpMax", writeMaterial: false, clamp: false, smoothingHz: 0, updateHz: 60, priority: 10, group: "Astroflux / PlayerShip Model"});

    q.graph.ensureBank("Shader::SavedPreTerrariaId", {type: "String", description: "Shader active before Terraria took ownership"});
    q.graph.ensureBank("Shader::SavedPreAstrofluxId", {type: "String", description: "Shader active before Astroflux took ownership"});

    // The JS runtime replaces these old control nodes if this migration is installed over a v11 graph.
    ["Automation::Terraria.CapturePreWorldShader", "Automation::Terraria.HealthVisualization", "Automation::Terraria.RestorePreviousShader", "Automation::Terraria.DeathVisualization", "Automation::Terraria.WorldStateCoordinator", "Automation::Terraria.RespawnVisualization", "Automation::Astroflux.ShieldVisualization", "Automation::Astroflux.RestorePreviousShader", "Automation::Astroflux.ShieldDepletedVisualization"].forEach(name => q.graph.setControlEnabled(name, false));

    const terrariaBindings = ["Native::Terraria.Player*::LocalInstance", "Native::Terraria.World::Active", "Model::Terraria.Player::statLife", "Model::Terraria.Player::statLifeMax2"];
    const astroBindings = ["Native::Astroflux.PlayerShip*::LocalInstance", "Model::Astroflux.PlayerShip::shieldHp", "Model::Astroflux.PlayerShip::shieldHpMax"];
    q.graph.ensureProfile("Terraria Runtime - Enabled", {exclusive: true, bindings: terrariaBindings, scripts: [q.name]});
    q.graph.ensureProfile("Terraria Runtime - Disabled", {exclusive: true, bindings: [], scripts: []});
    q.graph.ensureProfile("Astroflux Runtime - Enabled", {exclusive: true, bindings: astroBindings, scripts: [q.name]});
    q.graph.ensureProfile("Astroflux Runtime - Disabled", {exclusive: true, bindings: [], scripts: []});
    q.state.configured = true;
}

const profile = q.graph.activeProfile();
const allowTerraria = !profile || profile === "Terraria Runtime - Enabled";
const allowAstroflux = !profile || profile === "Astroflux Runtime - Enabled";
const worldActive = allowTerraria && !!q.binding("Native::Terraria.World::Active");
const life = q.binding("Model::Terraria.Player::statLife");
const lifeMax = q.binding("Model::Terraria.Player::statLifeMax2");
const terrariaReadable = worldActive && Number.isFinite(life) && Number.isFinite(lifeMax) && lifeMax > 0;

if (worldActive && !q.state.terrariaWorldActive) q.graph.setBank("Shader::SavedPreTerrariaId", q.runtime.currentShader());
if (terrariaReadable) {
    const fraction = Math.max(0, Math.min(1, life / lifeMax));
    q.runtime.material("source.health", 0, fraction);
    q.runtime.shader(life <= 0 ? "shader.hp.death" : "shader.hp.slider", 0.35);
}
if (!worldActive && q.state.terrariaWorldActive) {
    const saved = q.bank("Shader::SavedPreTerrariaId");
    if (saved) q.runtime.shader(saved, 0.35);
}
q.state.terrariaWorldActive = worldActive;

const shield = q.binding("Model::Astroflux.PlayerShip::shieldHp");
const shieldMax = q.binding("Model::Astroflux.PlayerShip::shieldHpMax");
const astroReadable = allowAstroflux && Number.isFinite(shield) && Number.isFinite(shieldMax) && shieldMax > 0;
if (astroReadable && !q.state.astroReadable) q.graph.setBank("Shader::SavedPreAstrofluxId", q.runtime.currentShader());
if (astroReadable) {
    const fraction = Math.max(0, Math.min(1, shield / shieldMax));
    q.runtime.material("source.health", 0, fraction);
    q.runtime.shader(shield <= 0 ? "shader.hp.death" : "shader.hp.slider", 0.35);
}
if (!astroReadable && q.state.astroReadable) {
    const saved = q.bank("Shader::SavedPreAstrofluxId");
    if (saved) q.runtime.shader(saved, 0.35);
}
q.state.astroReadable = astroReadable;
)JS";
        return Source;
    }

    bool runtimeInstallTerrariaAstrofluxMigration(RuntimeBindingEngine& engine, std::string& error)
    {
        const auto directory=runtimeQuickJSScriptDirectory();std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec){error=ec.message();return false;}const auto path=directory/"terraria-astroflux-runtime.js";std::ofstream file(path,std::ios::binary|std::ios::trunc);if(!file){error="could not write "+path.string();return false;}const auto source=runtimeTerrariaAstrofluxMigrationScript();file.write(source.data(),static_cast<std::streamsize>(source.size()));if(!file){error="failed writing "+path.string();return false;}RuntimeScript* script=engine.findScriptByName("Terraria + Astroflux runtime");if(!script){script=&engine.addScript();std::snprintf(script->Name,sizeof(script->Name),"Terraria + Astroflux runtime");}script->External=true;script->Path=path.string();script->HotReload=true;script->UpdateHz=60.0f;script->TimeoutMs=8.0f;script->Enabled=true;std::snprintf(script->Group,sizeof(script->Group),"Games");engine.markChanged();runtimeResetWorkspaceScript(script->Id);error.clear();return true;
    }
}
