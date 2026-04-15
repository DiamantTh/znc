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

#include <znc/ZNCString.h>
#include <functional>

class CConfig;

/**
 * @brief Abstract interface for loading and saving ZNC configuration data.
 *
 * Implementations can back configuration by a flat file (FileConfigProvider),
 * a database, a remote service, or any other storage mechanism.  The core only
 * depends on this interface; concrete backends are provided either by the built-
 * in FileConfigProvider or by external modules that register a custom provider
 * via CZNC::SetConfigProvider().
 */
class IConfigProvider {
  public:
    virtual ~IConfigProvider() = default;

    /**
     * Load configuration data into @p config.
     *
     * Implementations should populate @p config so that the CZNC core can call
     * its LoadGlobal / LoadUsers helpers.
     *
     * @param config  Output: populated configuration tree.
     * @param sError  Output: human-readable error description on failure.
     * @return true on success, false on failure.
     */
    virtual bool Load(CConfig& config, CString& sError) = 0;

    /**
     * Persist the given configuration.
     *
     * @param config  The configuration tree to persist.
     * @param sError  Output: human-readable error description on failure.
     * @return true on success, false on failure.
     */
    virtual bool Save(const CConfig& config, CString& sError) = 0;

    /**
     * Optionally register a callback that is invoked when the configuration
     * changes externally (e.g. the backing file is modified on disk).
     *
     * The default implementation is a no-op; file-watching or polling logic
     * lives in concrete subclasses.
     */
    virtual void Watch(std::function<void()> /*callback*/) {}
};
