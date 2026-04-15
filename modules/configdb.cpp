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
 * @file configdb.cpp
 * @brief Database-backed configuration backend (architecture stub).
 *
 * This module demonstrates how an external config backend can be registered
 * via the IConfigProvider interface introduced in Stufe 2 of the config-layer
 * refactoring.  It hooks into ZNC's module lifecycle and replaces the default
 * FileConfigProvider with a custom IConfigProvider implementation.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * CURRENT STATUS: stub / proof-of-concept
 * ──────────────────────────────────────────────────────────────────────────
 * To build a real database backend, replace the TODO sections below with
 * actual database calls (SQLite, PostgreSQL, etc.) and add the corresponding
 * library to CMakeLists.txt:
 *
 *   set(modlink_configdb sqlite3)   # or libpqxx, etc.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * HOW IT WORKS
 * ──────────────────────────────────────────────────────────────────────────
 * 1. On load (OnLoad), the module creates a DbConfigProvider and registers
 *    it with CZNC::SetConfigProvider().
 * 2. On the *next* rehash (/znc rehash), CZNC will call
 *    m_pConfigProvider->Load() which routes to DbConfigProvider::Load().
 * 3. On every WriteConfig(), CZNC calls m_pConfigProvider->Save() which
 *    routes to DbConfigProvider::Save().
 *
 * ──────────────────────────────────────────────────────────────────────────
 * USAGE
 * ──────────────────────────────────────────────────────────────────────────
 *   LoadModule = configdb /path/to/znc.db
 *
 * Load it as a global module so it is available before any user-level
 * operations take place.
 */

#include <znc/znc.h>
#include <znc/IConfigProvider.h>
#include <znc/Config.h>
#include <znc/Modules.h>
#include <znc/FileUtils.h>

// ---------------------------------------------------------------------------
// DbConfigProvider – IConfigProvider backed by a key-value database
// ---------------------------------------------------------------------------

class DbConfigProvider : public IConfigProvider {
  public:
    explicit DbConfigProvider(const CString& sDbPath) : m_sDbPath(sDbPath) {}

    bool Load(CConfig& config, CString& sError) override {
        // TODO: Open the database at m_sDbPath and populate 'config'.
        //
        // Example schema (SQLite):
        //   CREATE TABLE IF NOT EXISTS global_kv (key TEXT, value TEXT);
        //   CREATE TABLE IF NOT EXISTS users    (name TEXT PRIMARY KEY, ...);
        //   CREATE TABLE IF NOT EXISTS networks (user TEXT, name TEXT, ...);
        //
        // After reading rows, translate them into CConfig key-value pairs and
        // sub-configs so that the CZNC core can call its existing
        // LoadGlobal/LoadUsers helpers without modification.
        //
        // For example:
        //   config.AddKeyValuePair("Version", "1.11.0");
        //   CConfig userConfig;
        //   userConfig.AddKeyValuePair("Nick", "alice");
        //   config.AddSubConfig("User", "alice", userConfig);

        CUtils::PrintError("configdb: Load() is not yet implemented. "
                           "Falling back to file-based config.");
        sError =
            "configdb module is a stub; no real database load was performed.";
        return false;  // Return false → core falls back to FileConfigProvider
    }

    bool Save(const CConfig& config, CString& sError) override {
        // TODO: Iterate 'config' and persist each key-value pair and sub-config
        // into the database.
        //
        // Tip: Use config.BeginEntries()/EndEntries() and
        //      config.BeginSubConfigs()/EndSubConfigs() to traverse the tree.

        CUtils::PrintError("configdb: Save() is not yet implemented.");
        sError = "configdb module is a stub; nothing was saved.";
        return false;
    }

  private:
    CString m_sDbPath;
};

// ---------------------------------------------------------------------------
// CConfigDBMod – the global ZNC module that wires up the provider
// ---------------------------------------------------------------------------

class CConfigDBMod : public CModule {
  public:
    MODCONSTRUCTOR(CConfigDBMod) {}

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        if (sArgs.empty()) {
            sMessage =
                "Usage: LoadModule = configdb <path-to-database-file>\n"
                "Example: LoadModule = configdb /etc/znc/znc.db";
            return false;
        }

        m_sDbPath = sArgs.Token(0);

        // Register our custom provider.  Ownership is transferred to CZNC.
        // Starting from the next rehash, all Load()/Save() calls will be
        // routed through DbConfigProvider.
        CZNC::Get().SetConfigProvider(
            std::make_unique<DbConfigProvider>(m_sDbPath));

        PutModule("Registered DbConfigProvider for database: " + m_sDbPath);
        PutModule(
            "NOTE: This module is a stub.  Implement Load()/Save() in "
            "modules/configdb.cpp to connect a real database backend.");
        return true;
    }

    void OnModCommand(const CString& sCommand) override {
        if (sCommand.Token(0).Equals("status")) {
            PutModule("DbConfigProvider is registered.");
            PutModule("Database path: " + m_sDbPath);
            PutModule("NOTE: stub – no real DB operations are performed.");
        } else if (sCommand.Token(0).Equals("help")) {
            PutModule("status  – Show current database provider status.");
        } else {
            PutModule("Unknown command. Try 'help'.");
        }
    }

  private:
    CString m_sDbPath;
};

template <>
void TModInfo<CConfigDBMod>(CModInfo& Info) {
    Info.SetWikiPage("configdb");
    Info.SetHasArgs(true);
    Info.SetArgsHelpText("Path to the database file (e.g. /etc/znc/znc.db)");
}

GLOBALMODULEDEFS(
    CConfigDBMod,
    t_s("Database-backed configuration backend (stub). "
        "Implements IConfigProvider to replace the default file-based config."))
