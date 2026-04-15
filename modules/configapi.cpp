/*
 * Copyright (C) 2004-2026 ZNC, see the NOTICE file for details.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file configapi.cpp
 * @brief JSON REST API for ZNC configuration management.
 *
 * Exposes ZNC configuration via HTTP endpoints.  All responses are JSON.
 * Authentication is done via HTTP Basic Auth (same credentials as webadmin).
 * Only ZNC admin users may access this API.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * ENDPOINTS
 * ──────────────────────────────────────────────────────────────────────────
 *
 *  GET  /mods/global/configapi/
 *       → JSON object describing all available endpoints.
 *
 *  GET  /mods/global/configapi/config
 *       → JSON object with global ZNC settings (AnonIPLimit, MaxBufferSize,
 *         HideVersion, ProtectWebSessions, AuthOnlyViaModule,
 *         SplitUserConfig, ConnectDelay, ServerThrottle, StatusPrefix, Skin,
 *         SSLCertFile, SSLKeyFile, SSLDHParamFile, SSLCiphers, SSLProtocols,
 *         ConfigWriteDelay, PidFile).
 *
 *  POST /mods/global/configapi/config
 *       → Accepts the same keys as form parameters to update global settings.
 *         Triggers an immediate WriteConfig().
 *
 *  GET  /mods/global/configapi/users
 *       → JSON array of user names.
 *
 *  GET  /mods/global/configapi/users/<name>
 *       → JSON object with user settings (Nick, AltNick, Ident, RealName,
 *         BindHost, Admin, MultiClients, MaxNetworks, AuthOnlyViaModule,
 *         DenyLoadMod, DenySetBindHost, etc.).
 *
 *  POST /mods/global/configapi/users/<name>
 *       → Create or update user.  Pass fields as form parameters.
 *         Special parameter: action=delete  to remove the user.
 *
 *  GET  /mods/global/configapi/users/<name>/networks
 *       → JSON array of network names for the user.
 *
 *  GET  /mods/global/configapi/users/<name>/networks/<net>
 *       → JSON object with network settings (Nick, AltNick, Ident, RealName,
 *         BindHost, IRCConnectEnabled, Servers, TrustedFingerprints).
 *
 *  POST /mods/global/configapi/users/<name>/networks/<net>
 *       → Create or update network.  action=delete removes it.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * NOTES
 * ──────────────────────────────────────────────────────────────────────────
 * • Since ZNC's HTTP layer only exposes GET and POST, method overriding is
 *   done via the  _method  query/form parameter (PUT and DELETE are
 *   translated to the appropriate write/delete action).
 * • CSRF protection is bypassed for this module because API clients
 *   authenticate via HTTP Basic Auth.
 * • All output is UTF-8 JSON.
 */

#include <znc/znc.h>
#include <znc/IRCNetwork.h>
#include <znc/Server.h>
#include <znc/WebModules.h>
#include <znc/Modules.h>
#include <znc/User.h>

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external library required)
// ---------------------------------------------------------------------------

namespace {

// Escape a string value for embedding in JSON.
CString JsonEscape(const CString& s) {
    CString out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

CString JsonStr(const CString& s) { return "\"" + JsonEscape(s) + "\""; }
CString JsonBool(bool b)          { return b ? "true" : "false"; }
CString JsonNum(unsigned int n)   { return CString(n); }
CString JsonNum(double d)         { return CString(d); }

// Build JSON object from a list of "key": value pairs.
CString JsonObj(std::initializer_list<std::pair<CString, CString>> fields) {
    CString out = "{";
    bool first = true;
    for (const auto& kv : fields) {
        if (!first) out += ",";
        out += JsonStr(kv.first) + ":" + kv.second;
        first = false;
    }
    out += "}";
    return out;
}

// Build a JSON array from a list of pre-serialised values.
CString JsonArr(const VCString& items) {
    CString out = "[";
    bool first = true;
    for (const auto& item : items) {
        if (!first) out += ",";
        out += item;
        first = false;
    }
    out += "]";
    return out;
}

// Standard JSON response envelopes.
CString JsonOk(const CString& data) {
    return JsonObj({{"ok", "true"}, {"data", data}});
}
CString JsonError(const CString& msg, int code = 400) {
    return JsonObj({{"ok", "false"},
                    {"error", JsonStr(msg)},
                    {"code", CString(code)}});
}

}  // namespace

// ---------------------------------------------------------------------------
// CConfigAPIMod
// ---------------------------------------------------------------------------

class CConfigAPIMod : public CModule {
  public:
    MODCONSTRUCTOR(CConfigAPIMod) {}

    // -----------------------------------------------------------------------
    // Module web interface hooks
    // -----------------------------------------------------------------------

    bool WebRequiresLogin() override { return true; }
    bool WebRequiresAdmin() override { return true; }

    // API clients use Basic Auth and don't have a CSRF token.
    bool ValidateWebRequestCSRFCheck(CWebSock& /*WebSock*/,
                                     const CString& /*sPage*/) override {
        return true;
    }

    bool OnWebRequest(CWebSock& WebSock, const CString& sPageName,
                      CTemplate& /*Tmpl*/) override {
        // Parse the sub-path: e.g. "users/alice/networks/freenode"
        VCString vPath;
        sPageName.Split("/", vPath);
        // Remove empty tokens from leading slash variants.
        while (!vPath.empty() && vPath.front().empty()) {
            vPath.erase(vPath.begin());
        }

        const CString& sResource = vPath.empty() ? CString("index") : vPath[0];
        const bool bPost = WebSock.IsPost();

        // ── GET / → API info ────────────────────────────────────────────────
        if (sResource == "index") {
            return SendJson(WebSock, 200, JsonOk(JsonObj({
                {"description", JsonStr("ZNC Configuration REST API")},
                {"version",     JsonStr(CZNC::GetVersion())},
                {"endpoints",   JsonArr({
                    JsonStr("GET  /configapi/"),
                    JsonStr("GET  /configapi/config"),
                    JsonStr("POST /configapi/config"),
                    JsonStr("GET  /configapi/users"),
                    JsonStr("GET  /configapi/users/<name>"),
                    JsonStr("POST /configapi/users/<name>  [action=delete]"),
                    JsonStr("GET  /configapi/users/<name>/networks"),
                    JsonStr("GET  /configapi/users/<name>/networks/<net>"),
                    JsonStr("POST /configapi/users/<name>/networks/<net>  [action=delete]"),
                })}
            })));
        }

        // ── /config ─────────────────────────────────────────────────────────
        if (sResource == "config" && vPath.size() == 1) {
            if (bPost) return HandleConfigUpdate(WebSock);
            return HandleConfigRead(WebSock);
        }

        // ── /users ──────────────────────────────────────────────────────────
        if (sResource == "users") {
            if (vPath.size() == 1) {
                // GET /users → list
                if (!bPost) return HandleUsersList(WebSock);
                return SendJson(WebSock, 405, JsonError("Method not allowed", 405));
            }

            const CString& sUsername = vPath[1];

            if (vPath.size() == 2) {
                if (bPost) return HandleUserWrite(WebSock, sUsername);
                return HandleUserRead(WebSock, sUsername);
            }

            if (vPath.size() >= 3 && vPath[2] == "networks") {
                if (vPath.size() == 3) {
                    if (!bPost) return HandleNetworksList(WebSock, sUsername);
                    return SendJson(WebSock, 405, JsonError("Method not allowed", 405));
                }
                const CString& sNetwork = vPath[3];
                if (bPost)
                    return HandleNetworkWrite(WebSock, sUsername, sNetwork);
                return HandleNetworkRead(WebSock, sUsername, sNetwork);
            }
        }

        return SendJson(WebSock, 404, JsonError("Not found", 404));
    }

  private:
    // -----------------------------------------------------------------------
    // Helper: send JSON response
    // -----------------------------------------------------------------------

    bool SendJson(CWebSock& WebSock, int nCode, const CString& sJson) {
        const CString sStatus =
            CString(nCode) + " " + (nCode == 200   ? "OK"
                                    : nCode == 400  ? "Bad Request"
                                    : nCode == 404  ? "Not Found"
                                    : nCode == 405  ? "Method Not Allowed"
                                    : nCode == 409  ? "Conflict"
                                                    : "Error");
        WebSock.PrintHeader(sJson.length(),
                            "application/json; charset=UTF-8", nCode, sStatus);
        WebSock.Write(sJson);
        WebSock.Close(Csock::CLT_AFTERWRITE);
        return false;  // tell the framework we already sent the response
    }

    // -----------------------------------------------------------------------
    // /config  GET
    // -----------------------------------------------------------------------

    bool HandleConfigRead(CWebSock& WebSock) {
        CZNC& znc = CZNC::Get();
        return SendJson(WebSock, 200, JsonOk(JsonObj({
            {"AnonIPLimit",          JsonNum(znc.GetAnonIPLimit())},
            {"MaxBufferSize",        JsonNum(znc.GetMaxBufferSize())},
            {"HideVersion",          JsonBool(znc.GetHideVersion())},
            {"ProtectWebSessions",   JsonBool(znc.GetProtectWebSessions())},
            {"AuthOnlyViaModule",    JsonBool(znc.GetAuthOnlyViaModule())},
            {"SplitUserConfig",      JsonBool(znc.GetSplitUserConfig())},
            {"ConnectDelay",         JsonNum(znc.GetConnectDelay())},
            {"StatusPrefix",         JsonStr(znc.GetStatusPrefix())},
            {"Skin",                 JsonStr(znc.GetSkinName())},
            {"SSLCertFile",          JsonStr(znc.GetPemLocation())},
            {"SSLKeyFile",           JsonStr(znc.GetKeyLocation())},
            {"SSLDHParamFile",       JsonStr(znc.GetDHParamLocation())},
            {"SSLCiphers",           JsonStr(znc.GetSSLCiphers())},
            {"SSLProtocols",         JsonStr(znc.GetSSLProtocols())},
            {"ConfigWriteDelay",     JsonNum(znc.GetConfigWriteDelay())},
        })));
    }

    // -----------------------------------------------------------------------
    // /config  POST – update global settings
    // -----------------------------------------------------------------------

    bool HandleConfigUpdate(CWebSock& WebSock) {
        CZNC& znc = CZNC::Get();

        auto SetUInt = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (znc.*setter)(sVal.ToUInt());
        };
        auto SetBool = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (znc.*setter)(sVal.ToBool());
        };
        auto SetStr = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (znc.*setter)(sVal);
        };

        SetUInt("AnonIPLimit",      &CZNC::SetAnonIPLimit);
        SetUInt("MaxBufferSize",    &CZNC::SetMaxBufferSize);
        SetBool("HideVersion",      &CZNC::SetHideVersion);
        SetBool("ProtectWebSessions", &CZNC::SetProtectWebSessions);
        SetBool("AuthOnlyViaModule",  &CZNC::SetAuthOnlyViaModule);
        SetBool("SplitUserConfig",    &CZNC::SetSplitUserConfig);
        SetUInt("ConnectDelay",     &CZNC::SetConnectDelay);
        SetStr("StatusPrefix",      &CZNC::SetStatusPrefix);
        SetStr("Skin",              &CZNC::SetSkinName);
        SetUInt("ConfigWriteDelay", &CZNC::SetConfigWriteDelay);

        znc.WriteConfig();

        return SendJson(WebSock, 200, JsonOk(JsonStr("Global config updated")));
    }

    // -----------------------------------------------------------------------
    // /users  GET
    // -----------------------------------------------------------------------

    bool HandleUsersList(CWebSock& WebSock) {
        VCString vUsers;
        for (const auto& it : CZNC::Get().GetUserMap()) {
            vUsers.push_back(JsonStr(it.first));
        }
        return SendJson(WebSock, 200, JsonOk(JsonArr(vUsers)));
    }

    // -----------------------------------------------------------------------
    // /users/<name>  GET
    // -----------------------------------------------------------------------

    bool HandleUserRead(CWebSock& WebSock, const CString& sUsername) {
        CUser* pUser = CZNC::Get().FindUser(sUsername);
        if (!pUser) {
            return SendJson(WebSock, 404,
                            JsonError("User not found: " + sUsername, 404));
        }

        VCString vNetworks;
        for (const CIRCNetwork* pNet : pUser->GetNetworks()) {
            vNetworks.push_back(JsonStr(pNet->GetName()));
        }

        return SendJson(WebSock, 200, JsonOk(JsonObj({
            {"Username",              JsonStr(pUser->GetUsername())},
            {"Nick",                  JsonStr(pUser->GetNick(false))},
            {"AltNick",               JsonStr(pUser->GetAltNick(false))},
            {"Ident",                 JsonStr(pUser->GetIdent(false))},
            {"RealName",              JsonStr(pUser->GetRealName())},
            {"BindHost",              JsonStr(pUser->GetBindHost())},
            {"Admin",                 JsonBool(pUser->IsAdmin())},
            {"MultiClients",          JsonBool(pUser->MultiClients())},
            {"MaxNetworks",           JsonNum(pUser->MaxNetworks())},
            {"ChanBufferSize",        JsonNum(pUser->GetChanBufferSize())},
            {"QueryBufferSize",       JsonNum(pUser->GetQueryBufferSize())},
            {"AutoClearChanBuffer",   JsonBool(pUser->AutoClearChanBuffer())},
            {"AutoClearQueryBuffer",  JsonBool(pUser->AutoClearQueryBuffer())},
            {"AuthOnlyViaModule",     JsonBool(pUser->AuthOnlyViaModule())},
            {"DenyLoadMod",           JsonBool(pUser->DenyLoadMod())},
            {"DenySetBindHost",       JsonBool(pUser->DenySetBindHost())},
            {"StatusPrefix",          JsonStr(pUser->GetStatusPrefix())},
            {"Timezone",              JsonStr(pUser->GetTimezone())},
            {"Networks",              JsonArr(vNetworks)},
        })));
    }

    // -----------------------------------------------------------------------
    // /users/<name>  POST – create, update or delete user
    // -----------------------------------------------------------------------

    bool HandleUserWrite(CWebSock& WebSock, const CString& sUsername) {
        const CString sAction = WebSock.GetParam("action");

        // ── DELETE action ────────────────────────────────────────────────────
        if (sAction.Equals("delete") ||
            WebSock.GetParam("_method").Equals("delete")) {
            CUser* pUser = CZNC::Get().FindUser(sUsername);
            if (!pUser) {
                return SendJson(WebSock, 404,
                                JsonError("User not found: " + sUsername, 404));
            }
            if (!CZNC::Get().DeleteUser(pUser->GetUsername())) {
                return SendJson(WebSock, 409,
                                JsonError("Could not delete user " + sUsername +
                                              " (maybe the last admin?)",
                                          409));
            }
            CZNC::Get().WriteConfig();
            return SendJson(WebSock, 200,
                            JsonOk(JsonStr("User deleted: " + sUsername)));
        }

        // ── CREATE or UPDATE ─────────────────────────────────────────────────
        CUser* pUser = CZNC::Get().FindUser(sUsername);
        bool bNew = (pUser == nullptr);

        if (bNew) {
            pUser = new CUser(sUsername);
            // Set a mandatory empty password for new users created via API.
            pUser->SetPass("", CUser::HASH_NONE);
        }

        // Apply parameters.
        auto ApplyStr = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (pUser->*setter)(sVal);
        };
        auto ApplyBool = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (pUser->*setter)(sVal.ToBool());
        };
        auto ApplyUInt = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (pUser->*setter)(sVal.ToUInt());
        };

        ApplyStr("Nick",             &CUser::SetNick);
        ApplyStr("AltNick",          &CUser::SetAltNick);
        ApplyStr("Ident",            &CUser::SetIdent);
        ApplyStr("RealName",         &CUser::SetRealName);
        ApplyStr("BindHost",         &CUser::SetBindHost);
        ApplyBool("Admin",           &CUser::SetAdmin);
        ApplyBool("MultiClients",    &CUser::SetMultiClients);
        ApplyBool("AuthOnlyViaModule", &CUser::SetAuthOnlyViaModule);
        ApplyBool("DenyLoadMod",     &CUser::SetDenyLoadMod);
        ApplyBool("DenySetBindHost", &CUser::SetDenySetBindHost);
        ApplyUInt("MaxNetworks",     &CUser::SetMaxNetworks);
        ApplyStr("Timezone",         &CUser::SetTimezone);

        // Password update (optional).
        const CString sPass = WebSock.GetParam("Password");
        if (!sPass.empty()) {
            pUser->SetPass(CUtils::SaltedSHA256Hash(sPass, CUtils::GetSalt()),
                           CUser::HASH_SHA256,
                           CUtils::GetSalt());
        }

        if (bNew) {
            CString sErr;
            if (!CZNC::Get().AddUser(pUser, sErr)) {
                delete pUser;
                return SendJson(WebSock, 409,
                                JsonError("Could not add user: " + sErr, 409));
            }
        }

        CZNC::Get().WriteConfig();

        return SendJson(
            WebSock, 200,
            JsonOk(JsonStr(bNew ? "User created: " + sUsername
                                : "User updated: " + sUsername)));
    }

    // -----------------------------------------------------------------------
    // /users/<name>/networks  GET
    // -----------------------------------------------------------------------

    bool HandleNetworksList(CWebSock& WebSock, const CString& sUsername) {
        CUser* pUser = CZNC::Get().FindUser(sUsername);
        if (!pUser) {
            return SendJson(WebSock, 404,
                            JsonError("User not found: " + sUsername, 404));
        }
        VCString vNets;
        for (const CIRCNetwork* pNet : pUser->GetNetworks()) {
            vNets.push_back(JsonStr(pNet->GetName()));
        }
        return SendJson(WebSock, 200, JsonOk(JsonArr(vNets)));
    }

    // -----------------------------------------------------------------------
    // /users/<name>/networks/<net>  GET
    // -----------------------------------------------------------------------

    bool HandleNetworkRead(CWebSock& WebSock, const CString& sUsername,
                           const CString& sNetwork) {
        CUser* pUser = CZNC::Get().FindUser(sUsername);
        if (!pUser) {
            return SendJson(WebSock, 404,
                            JsonError("User not found: " + sUsername, 404));
        }
        CIRCNetwork* pNet = pUser->FindNetwork(sNetwork);
        if (!pNet) {
            return SendJson(WebSock, 404,
                            JsonError("Network not found: " + sNetwork, 404));
        }

        // Servers
        VCString vServers;
        for (const CServer* pSrv : pNet->GetServers()) {
            vServers.push_back(JsonStr(pSrv->GetString()));
        }

        // Trusted fingerprints
        VCString vFPs;
        for (const CString& sFP : pNet->GetTrustedFingerprints()) {
            vFPs.push_back(JsonStr(sFP));
        }

        return SendJson(WebSock, 200, JsonOk(JsonObj({
            {"Name",               JsonStr(pNet->GetName())},
            {"Nick",               JsonStr(pNet->GetNick())},
            {"AltNick",            JsonStr(pNet->GetAltNick())},
            {"Ident",              JsonStr(pNet->GetIdent())},
            {"RealName",           JsonStr(pNet->GetRealName())},
            {"BindHost",           JsonStr(pNet->GetBindHost())},
            {"IRCConnectEnabled",  JsonBool(pNet->GetIRCConnectEnabled())},
            {"TrustAllCerts",      JsonBool(pNet->GetTrustAllCerts())},
            {"TrustPKI",           JsonBool(pNet->GetTrustPKI())},
            {"FloodRate",          JsonNum(pNet->GetFloodRate())},
            {"FloodBurst",         JsonNum((unsigned int)pNet->GetFloodBurst())},
            {"JoinDelay",          JsonNum((unsigned int)pNet->GetJoinDelay())},
            {"Encoding",           JsonStr(pNet->GetEncoding())},
            {"Servers",            JsonArr(vServers)},
            {"TrustedFingerprints",JsonArr(vFPs)},
        })));
    }

    // -----------------------------------------------------------------------
    // /users/<name>/networks/<net>  POST – create, update or delete network
    // -----------------------------------------------------------------------

    bool HandleNetworkWrite(CWebSock& WebSock, const CString& sUsername,
                            const CString& sNetwork) {
        CUser* pUser = CZNC::Get().FindUser(sUsername);
        if (!pUser) {
            return SendJson(WebSock, 404,
                            JsonError("User not found: " + sUsername, 404));
        }

        const CString sAction = WebSock.GetParam("action");
        if (sAction.Equals("delete") ||
            WebSock.GetParam("_method").Equals("delete")) {
            if (!pUser->DeleteNetwork(sNetwork)) {
                return SendJson(
                    WebSock, 404,
                    JsonError("Network not found: " + sNetwork, 404));
            }
            CZNC::Get().WriteConfig();
            return SendJson(WebSock, 200,
                            JsonOk(JsonStr("Network deleted: " + sNetwork)));
        }

        CIRCNetwork* pNet = pUser->FindNetwork(sNetwork);
        bool bNew = (pNet == nullptr);

        if (bNew) {
            CString sErr;
            pNet = pUser->AddNetwork(sNetwork, sErr);
            if (!pNet) {
                return SendJson(WebSock, 409,
                                JsonError("Could not add network: " + sErr, 409));
            }
        }

        // Apply parameters.
        auto ApplyStr = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (pNet->*setter)(sVal);
        };
        auto ApplyBool = [&](const CString& sKey, auto setter) {
            CString sVal = WebSock.GetParam(sKey);
            if (!sVal.empty()) (pNet->*setter)(sVal.ToBool());
        };

        ApplyStr("Nick",               &CIRCNetwork::SetNick);
        ApplyStr("AltNick",            &CIRCNetwork::SetAltNick);
        ApplyStr("Ident",              &CIRCNetwork::SetIdent);
        ApplyStr("RealName",           &CIRCNetwork::SetRealName);
        ApplyStr("BindHost",           &CIRCNetwork::SetBindHost);
        ApplyStr("Encoding",           &CIRCNetwork::SetEncoding);
        ApplyBool("IRCConnectEnabled", &CIRCNetwork::SetIRCConnectEnabled);
        ApplyBool("TrustAllCerts",     &CIRCNetwork::SetTrustAllCerts);
        ApplyBool("TrustPKI",          &CIRCNetwork::SetTrustPKI);

        // Server management.
        const CString sServer = WebSock.GetParam("AddServer");
        if (!sServer.empty()) {
            pNet->AddServer(sServer);
        }
        const CString sRemoveServer = WebSock.GetParam("RemoveServer");
        if (!sRemoveServer.empty()) {
            pNet->DelServer(sRemoveServer, 0, "");
        }

        CZNC::Get().WriteConfig();

        return SendJson(
            WebSock, 200,
            JsonOk(JsonStr(bNew ? "Network created: " + sNetwork
                                : "Network updated: " + sNetwork)));
    }
};

// ---------------------------------------------------------------------------

template <>
void TModInfo<CConfigAPIMod>(CModInfo& Info) {
    Info.SetWikiPage("configapi");
}

GLOBALMODULEDEFS(CConfigAPIMod,
                 t_s("JSON REST API for ZNC configuration management. "
                     "Endpoints: GET/POST /mods/global/configapi/config, "
                     "/users, /users/<name>, /users/<name>/networks, "
                     "/users/<name>/networks/<net>"))
