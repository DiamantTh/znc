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

#include <znc/Config.h>
#include <znc/FileUtils.h>
#include <stack>
#include <sstream>
#include <memory>

struct ConfigStackEntry {
    CString sTag;
    CString sName;
    CConfig Config;

    ConfigStackEntry(const CString& Tag, const CString Name)
        : sTag(Tag), sName(Name), Config() {}
};

CConfigEntry::CConfigEntry(const CConfig& Config)
    : m_pSubConfig(std::make_unique<CConfig>(Config)) {}

CConfigEntry::CConfigEntry(const CConfigEntry& other) {
    if (other.m_pSubConfig)
        m_pSubConfig = std::make_unique<CConfig>(*other.m_pSubConfig);
}

CConfigEntry& CConfigEntry::operator=(const CConfigEntry& other) {
    if (this != &other) {
        m_pSubConfig.reset();
        if (other.m_pSubConfig)
            m_pSubConfig = std::make_unique<CConfig>(*other.m_pSubConfig);
    }
    return *this;
}

bool CConfig::Parse(CFile& file, CString& sErrorMsg) {
    CString sLine;
    unsigned int uLineNum = 0;
    CConfig* pActiveConfig = this;
    std::stack<ConfigStackEntry> ConfigStack;
    bool bCommented = false;  // support for /**/ style comments

    // Inline error helper: sets sErrorMsg, clears state, returns false.
    auto Error = [&](const CString& sMsg) -> bool {
        sErrorMsg = "Error on line " + CString(uLineNum) + ": " + sMsg;
        m_SubConfigNameSets.clear();
        m_SubConfigs.clear();
        m_ConfigEntries.clear();
        return false;
    };

    if (!file.Seek(0)) {
        sErrorMsg = "Could not seek to the beginning of the config.";
        return false;
    }

    while (file.ReadLine(sLine)) {
        uLineNum++;

        // Remove all leading spaces and trailing line endings
        sLine.TrimLeft();
        sLine.TrimRight("\r\n");

        if (bCommented || sLine.StartsWith("/*")) {
            /* Does this comment end on the same line again? */
            bCommented = (!sLine.EndsWith("*/"));

            continue;
        }

        if ((sLine.empty()) || (sLine.StartsWith("#")) ||
            (sLine.StartsWith("//"))) {
            continue;
        }

        if ((sLine.StartsWith("<")) && (sLine.EndsWith(">"))) {
            sLine.LeftChomp();
            sLine.RightChomp();
            sLine.Trim();

            CString sTag = sLine.Token(0);
            CString sValue = sLine.Token(1, true);

            sTag.Trim();
            sValue.Trim();

            if (sTag.TrimPrefix("/")) {
                if (!sValue.empty())
                    return Error("Malformed closing tag. Expected \"</" +
                                 sTag + ">\".");
                if (ConfigStack.empty())
                    return Error("Closing tag \"" + sTag +
                                 "\" which is not open.");

                const struct ConfigStackEntry& entry = ConfigStack.top();
                CConfig myConfig(entry.Config);
                CString sName(entry.sName);

                if (!sTag.Equals(entry.sTag))
                    return Error("Closing tag \"" + sTag +
                                 "\" which is not open.");

                // This breaks entry
                ConfigStack.pop();

                if (ConfigStack.empty())
                    pActiveConfig = this;
                else
                    pActiveConfig = &ConfigStack.top().Config;

                const auto sTagLower = sTag.AsLower();
                auto& nameset = pActiveConfig->m_SubConfigNameSets[sTagLower];

                if (nameset.find(sName) != nameset.end())
                    return Error("Duplicate entry for tag \"" + sTag +
                                 "\" name \"" + sName + "\".");

                nameset.insert(sName);
                pActiveConfig->m_SubConfigs[sTagLower].emplace_back(sName,
                                                                    myConfig);
            } else {
                if (sValue.empty())
                    return Error("Empty block name at begin of block.");
                ConfigStack.push(ConfigStackEntry(sTag.AsLower(), sValue));
                pActiveConfig = &ConfigStack.top().Config;
            }

            continue;
        }

        // If we have a regular line, figure out where it goes
        CString sName = sLine.Token(0, false, "=");
        CString sValue = sLine.Token(1, true, "=");

        // Only remove the first space, people might want
        // leading spaces (e.g. in the MOTD).
        sValue.TrimPrefix(" ");

        // We don't have any names with spaces, trim all
        // leading/trailing spaces.
        sName.Trim();

        if (sName.empty() || sValue.empty()) return Error("Malformed line");

        // Handle the "Include = <path>" directive: recursively parse the
        // referenced file and merge its key-value pairs and subconfigs into
        // the current configuration level.  Relative paths are resolved
        // relative to the directory that contains the file being parsed.
        CString sNameLower = sName.AsLower();
        if (sNameLower == "include") {
            CString sIncludePath = sValue;
            if (!sIncludePath.empty() && sIncludePath[0] != '/') {
                // Resolve relative to the directory of the current file.
                CString sDir = CDir::ChangeDir(file.GetLongName(), "..");
                sIncludePath = sDir + "/" + sIncludePath;
            }
            CFile includeFile(sIncludePath);
            if (!includeFile.Open(O_RDONLY)) {
                sErrorMsg = "Error on line " + CString(uLineNum) +
                            ": Cannot open included file \"" + sIncludePath +
                            "\"";
                m_SubConfigNameSets.clear();
                m_SubConfigs.clear();
                m_ConfigEntries.clear();
                return false;
            }
            CString sIncludeError;
            if (!pActiveConfig->Parse(includeFile, sIncludeError)) {
                sErrorMsg = "In included file \"" + sIncludePath +
                            "\": " + sIncludeError;
                m_SubConfigNameSets.clear();
                m_SubConfigs.clear();
                m_ConfigEntries.clear();
                return false;
            }
            continue;
        }

        pActiveConfig->m_ConfigEntries[sNameLower].push_back(sValue);
    }

    if (bCommented) return Error("Comment not closed at end of file.");

    if (!ConfigStack.empty()) {
        const CString& sTag = ConfigStack.top().sTag;
        return Error(
            "Not all tags are closed at the end of the file. Inner-most open "
            "tag is \"" +
            sTag + "\".");
    }

    return true;
}

void CConfig::Write(CFile& File, unsigned int iIndentation) {
    CString sIndentation = CString(iIndentation, '\t');

    auto SingleLine = [](const CString& s) {
        return s.Replace_n("\r", "").Replace_n("\n", "");
    };

    for (const auto& it : m_ConfigEntries) {
        for (const CString& sValue : it.second) {
            File.Write(SingleLine(sIndentation + it.first + " = " + sValue) +
                       "\n");
        }
    }

    for (const auto& it : m_SubConfigs) {
        for (const auto& it2 : it.second) {
            File.Write("\n");

            File.Write(SingleLine(sIndentation + "<" + it.first + " " +
                                  it2.first + ">") +
                       "\n");
            it2.second.m_pSubConfig->Write(File, iIndentation + 1);
            File.Write(SingleLine(sIndentation + "</" + it.first + ">") + "\n");
        }
    }
}
