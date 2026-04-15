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

#pragma once

#include <znc/IConfigProvider.h>
#include <znc/FileUtils.h>
#include <memory>

/**
 * @brief File-based IConfigProvider implementation.
 *
 * Reads and writes a CConfig tree to/from a text file in ZNC's native .conf
 * format.  This is the default backend used by the ZNC core.
 *
 * The provider maintains an exclusive file lock on the config file for the
 * lifetime of the object so that two ZNC instances cannot share the same
 * config simultaneously.
 */
class FileConfigProvider : public IConfigProvider {
  public:
    /**
     * @param sFilePath  Absolute path to the znc.conf file.
     */
    explicit FileConfigProvider(const CString& sFilePath);
    ~FileConfigProvider() override = default;

    FileConfigProvider(const FileConfigProvider&) = delete;
    FileConfigProvider& operator=(const FileConfigProvider&) = delete;

    bool Load(CConfig& config, CString& sError) override;
    bool Save(const CConfig& config, CString& sError) override;

    /** Return the path to the configuration file. */
    const CString& GetFilePath() const { return m_sFilePath; }

    /**
     * Return a reference to the lock file maintained by this provider.
     * The lock file is opened for read/write and held with an exclusive
     * fcntl lock for the lifetime of this provider.
     */
    CFile* GetLockFile() const { return m_pLockFile.get(); }

  private:
    CString m_sFilePath;
    std::unique_ptr<CFile> m_pLockFile;
};
