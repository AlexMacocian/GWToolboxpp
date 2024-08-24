#include "stdafx.h"

#include <GWCA/Context/PartyContext.h>
#include <GWCA/Context/CharContext.h>

#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/PlayerMgr.h>

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/GameEntities/Player.h>

#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>

#include <Modules/Resources.h>
#include <Modules/PartyBroadcastModule.h>
#include <Modules/Updater.h>

#include <Utils/GuiUtils.h>
#include <Utils/TextUtils.h>

#include <easywsclient/easywsclient.hpp>
#include <nlohmann/json.hpp>

#include <Defines.h>
#include <Timer.h>
#include <GWCA/Context/GameContext.h>
#include <GWCA/Context/AccountContext.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <base64.h>
#include <wincrypt.h>

using json = nlohmann::json;

namespace {
    bool faulty = false;
    bool enabled = false;
    std::string api_key;
    easywsclient::WebSocket* ws = nullptr;
    std::string last_update_content;
    clock_t last_update_timestamp = 0;
    bool need_to_send_party_searches = true;
    clock_t failed_to_send_ts = 0;

    struct PartySearchAdvertisement {
        uint32_t party_id = 0;
        uint8_t party_size = 0;
        uint8_t hero_count = 0;
        uint8_t search_type = 0; // 0=hunting, 1=mission, 2=quest, 3=trade, 4=guild
        uint8_t hardmode = 0;
        uint16_t district_number = 0;
        uint8_t language = 0;
        uint8_t primary = 0;
        uint8_t secondary = 0;
        uint8_t level = 0;
        std::string message;
        std::string sender;
    };
    std::vector<PartySearchAdvertisement> party_search_advertisements;

    void to_json(nlohmann::json& j, const PartySearchAdvertisement& p)
    {
        j = nlohmann::json{{"i", p.party_id}, {"t", p.search_type}, {"p", p.primary}, {"s", p.sender}};

        // The following fields can be assumed to be reasonable defaults by the server, so only need to send if they're not standard.
        if (p.party_size > 1) j["ps"] = p.party_size;
        if (p.hero_count) j["hc"] = p.hero_count;
        if (p.hardmode) j["hm"] = p.hardmode;
        if (p.language) j["dl"] = p.language;
        if (p.secondary) j["sc"] = p.secondary;
        if (p.district_number) j["dn"] = p.district_number;
        if (p.message.size()) j["ms"] = p.message;
        if (p.level != 20) j["l"] = p.level;
    }

    std::string encode_to_base64(std::vector<BYTE>& input)
    {
        unsigned input_size = input.size();
        unsigned output_size = ((input_size + 2) / 3) * 4 + 1; // +1 for the null terminator
        std::string output_buffer;
        output_buffer.resize(output_size);
        const auto encoded_length = b64_enc(input.data(), input_size, output_buffer.data());
        return std::string(output_buffer.data(), encoded_length + 1); // +1, b64 returns the index of the last char
    }

    std::optional<std::vector<BYTE>> get_module_hash_bytes(HMODULE hModule)
    {
        MODULEINFO modInfo = {0};
        if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO))) {
            Log::Error("Failed to get toolbox module information");
            return std::nullopt;
        }

        auto modBase = (BYTE*)modInfo.lpBaseOfDll;
        auto modSize = modInfo.SizeOfImage;

        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;

        if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            Log::Error("Failed to get crypt context");
            return std::nullopt;
        }
        if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            CryptReleaseContext(hProv, 0);
            Log::Error("Failed to create a hash of toolbox module");
            return std::nullopt;
        }
        if (!CryptHashData(hHash, modBase, modSize, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            Log::Error("Failed to create a hashdata of toolbox module");
            return std::nullopt;
        }

        DWORD hash_size = 0;
        DWORD hash_size_len = sizeof(DWORD);
        if (!CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hash_size, &hash_size_len, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            Log::Error("Failed to get hash parameters of toolbox module");
            return std::nullopt;
        }

        std::vector<BYTE> hash_result;
        hash_result.resize(hash_size);
        if (!CryptGetHashParam(hHash, HP_HASHVAL, hash_result.data(), &hash_size, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            return std::nullopt;
        }

        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);

        return hash_result;
    }

    std::optional<std::string> calculate_toolbox_hash()
    {
        HMODULE hModule = NULL;
        if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)calculate_toolbox_hash, &hModule) == 0) {
            Log::Error("Failed to get toolbox module handle");
            return std::nullopt;
        }

        TCHAR module_path[MAX_PATH] = {0};
        if (GetModuleFileName(hModule, module_path, MAX_PATH) == 0) {
            std::cerr << "Failed to get module file name" << std::endl;
            return std::nullopt;
        }

        auto hash_bytes = get_module_hash_bytes(hModule);
        if (!hash_bytes.has_value()) {
            return std::nullopt;
        }

        return encode_to_base64(hash_bytes.value());
    }

    std::optional<std::string> get_api_key() {
        const auto module_hash = calculate_toolbox_hash();
        if (!module_hash.has_value()) {
            Log::Error("Failed to create party.toolbox.com API key. Failed to calculate toolbox hash");
            return std::nullopt;
        }

        GWToolboxRelease current_release;
        if (!Updater::GetCurrentVersionInfo(&current_release)) {
            Log::Error("Failed to get current toolbox version info");
            return std::nullopt;
        }

        return std::format("gwtoolbox-{}-{}", current_release.version, module_hash.value());
    }

    void disconnect_ws() {
        if (ws) {
            ws->close();
            ws->poll();
            delete ws;
            ws = nullptr;
            last_update_content = "";
            last_update_timestamp = 0;
        }
    }
    bool is_websocket_ready() {
        return ws && ws->getReadyState() == easywsclient::WebSocket::readyStateValues::OPEN;
    }

    // @Cleanup: Get gw account uuid for this.
    std::string get_uuid() {
        const auto c = GW::GetCharContext();
        if (!(c && c->player_uuid))
            return "";
        return std::format("{}-{}-{}-{}", c->player_uuid[0], c->player_uuid[1], c->player_uuid[2], c->player_uuid[3]);
    }

    // Run this on a worker thread!!
    bool connect_ws() {
        if (is_websocket_ready())
            return true;
        disconnect_ws();

        if (!api_key.size())
            return false;



        easywsclient::HeaderKeyValuePair headers = {
            {"User-Agent", "GWToolboxpp"},
            {"X-Api-Key", api_key},
            {"X-Account-Uuid", get_uuid()},
            {"X-Bot-Version", "100"}
        };
        ws = easywsclient::WebSocket::from_url("wss://party.gwtoolbox.com", headers);
        if (!ws) {
            return false;
        }

        ws->poll();
        if (!is_websocket_ready()) {
            disconnect_ws();
            return false;
        }
        last_update_content = "";
        last_update_timestamp = 0;
        return true;
    }
    // Run on worker thread!
    bool send_payload(const std::string payload) {
        if (!connect_ws()) {
            return false;
        }
        Log::Log("Websocket Send:\n%s", payload.c_str());
        ws->send(payload);
        return true;
    }

    // Run on game thread!
    std::vector<PartySearchAdvertisement> collect_party_searches() {
        ASSERT(GW::GameThread::IsInGameThread());
        const auto pc = GW::GetPartyContext();
        const auto searches = pc ? &pc->party_search : nullptr;
        const auto district_number = GW::Map::GetDistrict();
        const auto district_language = GW::Map::GetLanguage();
        std::vector<PartySearchAdvertisement> ads;
        if (searches) {
            for (const auto search : *searches) {
                if (!search) {
                    continue;
                }
                ASSERT(search->party_leader && *search->party_leader);

                PartySearchAdvertisement ad;
                ad.party_id = search->party_search_id;
                ad.party_size = static_cast<uint8_t>(search->party_size);
                ad.hero_count = static_cast<uint8_t>(search->hero_count);
                ad.search_type = static_cast<uint8_t>(search->party_search_type);
                ad.hardmode = static_cast<uint8_t>(search->hardmode);
                ad.district_number = static_cast<uint16_t>(search->district);
                ad.language = static_cast<uint8_t>(search->language);
                ad.primary = static_cast<uint8_t>(search->primary);
                ad.secondary = static_cast<uint8_t>(search->secondary);
                ad.level = static_cast<uint8_t>(search->level);
                ad.sender = TextUtils::WStringToString(search->party_leader);
                if (search->message && *search->message) {
                    ad.message = TextUtils::WStringToString(search->message);
                }
                ads.push_back(ad);
            }
        }
        const auto players = GW::PlayerMgr::GetPlayerArray();
        if (players) {
            for (const auto& player : *players) {
                if (!(player.party_size > 1 && player.name))
                    continue;
                const auto agent = static_cast<GW::AgentLiving*>(GW::Agents::GetAgentByID(player.agent_id));
                if (!(agent && agent->GetIsLivingType()))
                    continue; // Although the player might be present, the party size depends on the agent being in compass range
                const auto sender = TextUtils::WStringToString(player.name);
                const auto found = std::ranges::find_if(ads.begin(), ads.end(), [sender](const PartySearchAdvertisement& e) {
                    return sender == e.sender;
                    });
                if (found != ads.end())
                    continue; // Player has a party search entry
                PartySearchAdvertisement ad;
                ad.party_id = (0xffff0000 | player.player_number);
                ad.party_size = static_cast<uint8_t>(player.party_size);
                ad.district_number = static_cast<uint16_t>(district_number);
                ad.language = static_cast<uint8_t>(district_language);
                ad.primary = static_cast<uint8_t>(player.primary);
                ad.secondary = static_cast<uint8_t>(player.secondary);
                ad.level = static_cast<uint8_t>(agent->level);
                ad.sender = sender;
                ads.push_back(ad);
            }
        }

        return ads;
    }


    std::string last_party_searches_payload = "";
    // Run on game thread!
    bool send_current_party_searches() {
        if (!GW::Map::GetIsMapLoaded()) {
            return false;
        }

        json j;
        j["type"] = "client_parties";
        j["map_id"] = (uint32_t)GW::Map::GetMapID();
        j["district_region"] = (int)GW::Map::GetRegion();
        j["parties"] = collect_party_searches();

        const auto payload = j.dump();
        if (last_party_searches_payload != payload) {
            if (send_payload(payload)) {
                last_party_searches_payload = payload;
                last_update_timestamp = TIMER_INIT();
                return true;
            }
        }
        return false;
    }

    void on_websocket_message(const std::string& message) {
        Log::Log("Websocket message\n%s", message.c_str());
    }

    GW::HookEntry OnUIMessage_Hook;

    void OnUIMessage(GW::HookStatus*, GW::UI::UIMessage, void*, void*) {
        need_to_send_party_searches = true;
    }
}

void PartyBroadcast::SaveSettings(ToolboxIni* ini)
{
    ToolboxModule::SaveSettings(ini);
    SAVE_BOOL(enabled);
}

void PartyBroadcast::LoadSettings(ToolboxIni* ini)
{
    ToolboxModule::LoadSettings(ini);
    LOAD_BOOL(enabled);
}

void PartyBroadcast::Update(float) {
    if (!(enabled && api_key.size() && is_websocket_ready()))
        disconnect_ws();
    if (need_to_send_party_searches && (!failed_to_send_ts || TIMER_DIFF(failed_to_send_ts) > 5000)) {
        need_to_send_party_searches = !send_current_party_searches();
        if(need_to_send_party_searches)
            failed_to_send_ts = TIMER_INIT();
    }
    if (ws) {
        ws->poll();
        ws->dispatch(on_websocket_message);
    }
}

void PartyBroadcast::Initialize()
{
    ToolboxModule::Initialize();
    const auto key_result = get_api_key();
    if (!key_result.has_value()) {
        Log::Error("Failed to get API key for party.gwtoolbox.com. Disabling PartyBroadcast module");
        enabled = false;
        faulty = true;
        return;
    }

    api_key = key_result.value();
    need_to_send_party_searches = true;

    const GW::UI::UIMessage ui_messages[] = {
        GW::UI::UIMessage::kMapLoaded,
        (GW::UI::UIMessage)0x10000134, // Party search updated
        (GW::UI::UIMessage)0x10000132, // Party search updated
        (GW::UI::UIMessage)0x10000133, // Party search Remove 
        (GW::UI::UIMessage)0x10000131, // Party search Add
        (GW::UI::UIMessage)0x10000048 // Player size updated (in current range)
    };
    for (auto message_id : ui_messages) {
        GW::UI::RegisterUIMessageCallback(&OnUIMessage_Hook, message_id, OnUIMessage, 0x8000);
    }
}

void PartyBroadcast::Terminate() {
    ToolboxModule::Terminate();
    if (faulty) {
        return;
    }

    GW::UI::RemoveUIMessageCallback(&OnUIMessage_Hook);
    disconnect_ws();
}

void PartyBroadcast::DrawSettingsInternal()
{
    if (faulty) {
        return;
    }

    ImGui::NewLine();
    ImGui::Text("Party Broadcast Integration - https://party.gwtoolbox.com", "");
    ImGui::Indent();
    ImGui::Checkbox("Broadcast Party Searches", &enabled);
    ImGui::ShowHelp("Post party searches to https://party.gwtoolbox.com");
    ImGui::Unindent();
}