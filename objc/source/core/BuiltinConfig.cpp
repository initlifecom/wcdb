/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <WCDB/BuiltinConfig.hpp>
#include <WCDB/Database.hpp>
#include <WCDB/HandleStatement.hpp>
#include <WCDB/timed_queue.hpp>
#include <WCDB/utility.hpp>
#include <queue>
#include <thread>
#include <vector>

namespace WCDB {

const Config BuiltinConfig::basic(
    "basic",
    [](std::shared_ptr<Handle> &handle, Error &error) -> bool {

        if (handle->isReadonly()) {
            static const StatementPragma s_getJournalMode =
                StatementPragma().pragma(Pragma::JournalMode);

            //Get Journal Mode
            std::shared_ptr<HandleStatement> handleStatement =
                handle->prepare(s_getJournalMode);
            if (!handleStatement) {
                error = handle->getError();
                return false;
            }
            handleStatement->step();
            if (!handleStatement->isOK()) {
                error = handleStatement->getError();
                return false;
            }
            std::string journalMode =
                handleStatement->getValue<WCDB::ColumnType::Text>(0);
            handleStatement->finalize();

            if (strcasecmp(journalMode.c_str(), "WAL") == 0) {
                // See also: http://www.sqlite.org/wal.html#readonly
                Error::Abort("It is not possible to open read-only WAL "
                             "databases.");
                return false;
            }
            return true;
        }

        //Locking Mode
        {
            static const StatementPragma s_getLockingMode =
                StatementPragma().pragma(Pragma::LockingMode);
            static const StatementPragma s_setLockingModeNormal =
                StatementPragma().pragma(Pragma::LockingMode, "NORMAL");

            //Get Locking Mode
            std::shared_ptr<HandleStatement> handleStatement =
                handle->prepare(s_getLockingMode);
            if (!handleStatement) {
                error = handle->getError();
                return false;
            }
            handleStatement->step();
            if (!handleStatement->isOK()) {
                error = handleStatement->getError();
                return false;
            }
            std::string lockingMode =
                handleStatement->getValue<WCDB::ColumnType::Text>(0);
            handleStatement->finalize();

            //Set Locking Mode
            if (strcasecmp(lockingMode.c_str(), "NORMAL") != 0 &&
                !handle->exec(s_setLockingModeNormal)) {
                error = handle->getError();
                return false;
            }
        }

        //Synchronous
        {
            static const StatementPragma s_setSynchronousNormal =
                StatementPragma().pragma(Pragma::Synchronous, "NORMAL");

            if (!handle->exec(s_setSynchronousNormal)) {
                error = handle->getError();
                return false;
            }
        }

        //Journal Mode
        {
            static const StatementPragma s_getJournalMode =
                StatementPragma().pragma(Pragma::JournalMode);
            static const StatementPragma s_setJournalModeWAL =
                StatementPragma().pragma(Pragma::JournalMode, "WAL");

            //Get Journal Mode
            std::shared_ptr<HandleStatement> handleStatement =
                handle->prepare(s_getJournalMode);
            if (!handleStatement) {
                error = handle->getError();
                return false;
            }
            handleStatement->step();
            if (!handleStatement->isOK()) {
                error = handleStatement->getError();
                return false;
            }
            std::string journalMode =
                handleStatement->getValue<WCDB::ColumnType::Text>(0);
            handleStatement->finalize();

            //Set Journal Mode
            if (strcasecmp(journalMode.c_str(), "WAL") != 0 &&
                !handle->exec(s_setJournalModeWAL)) {
                error = handle->getError();
                return false;
            }
        }

        //Fullfsync
        {
            static const StatementPragma s_setFullFsync =
                StatementPragma().pragma(Pragma::Fullfsync, true);

            if (!handle->exec(s_setFullFsync)) {
                error = handle->getError();
                return false;
            }
        }

        error.reset();
        return true;
    },
    Order::Basic);

std::shared_ptr<PerformanceTrace> BuiltinConfig::s_globalPerformanceTrace;
std::shared_ptr<SQLTrace> BuiltinConfig::s_globalSQLTrace;

void BuiltinConfig::SetGlobalPerformanceTrace(
    const PerformanceTrace &globalTrace)
{
    s_globalPerformanceTrace.reset(new PerformanceTrace(globalTrace));
}

void BuiltinConfig::SetGlobalSQLTrace(const SQLTrace &globalTrace)
{
    s_globalSQLTrace.reset(new SQLTrace(globalTrace));
}

const Config BuiltinConfig::trace(
    "trace",
    [](std::shared_ptr<Handle> &handle, Error &error) -> bool {
        {
            std::shared_ptr<PerformanceTrace> trace = s_globalPerformanceTrace;
            if (trace) {
                handle->setPerformanceTrace(*trace.get());
            }
        }
        {
            std::shared_ptr<SQLTrace> trace = s_globalSQLTrace;
            if (trace) {
                handle->setSQLTrace(*trace.get());
            }
        }
        return true;
    },
    Order::Trace);

const Config
BuiltinConfig::cipherWithKey(const void *key, int keySize, int pageSize)
{
    std::shared_ptr<std::vector<unsigned char>> keys(
        new std::vector<unsigned char>(keySize));
    memcpy(keys->data(), key, keySize);
    return Config("cipher",
                  [keys, pageSize](std::shared_ptr<Handle> &handle,
                                   Error &error) -> bool {

                      //Set Cipher Key
                      bool result = handle->setCipherKey(keys->data(),
                                                         (int) keys->size());
                      if (!result) {
                          error = handle->getError();
                          return false;
                      }

                      //Set Cipher Page Size
                      if (!handle->exec(StatementPragma().pragma(
                              Pragma::CipherPageSize, pageSize))) {
                          error = handle->getError();
                          return false;
                      }

                      error.reset();
                      return true;
                  },
                  Order::Cipher);
}

const Config BuiltinConfig::checkpoint(
    "checkpoint",
    [](std::shared_ptr<Handle> &handle, Error &error) -> bool {
        handle->registerCommittedHook(
            [](Handle *handle, int pages, void *) {
                static TimedQueue<std::string> s_timedQueue(2);
                if (pages > 1000) {
                    s_timedQueue.reQueue(handle->path);
                }
                static std::thread s_checkpointThread([]() {
                    pthread_setname_np("WCDB-checkpoint");
                    while (true) {
                        //TODO atExit
                        s_timedQueue.waitUntilExpired(
                            [](const std::string &path) {
                                Database database(
                                    path,
                                    true); // Get Existing Database Only
                                if (database.getType() != CoreType::None) {
                                    WCDB::Error innerError;
                                    database.exec(StatementPragma().pragma(
                                                      Pragma::WalCheckpoint),
                                                  innerError);
                                }
                            });
                    }
                });
                static std::once_flag s_flag;
                std::call_once(s_flag, []() { s_checkpointThread.detach(); });
            },
            nullptr);
        return true;
    },
    Order::Checkpoint);

const Config
BuiltinConfig::tokenizeWithNames(const std::list<std::string> &names)
{
    return Config(
        "tokenize",
        [names](std::shared_ptr<Handle> &handle, Error &error) -> bool {
            for (const std::string &name : names) {
                std::vector<unsigned char> address =
                    FTS::Modules::SharedModules()->getAddress(name);

                //Tokenize
                {
                    static const StatementSelect FTS3Tokenizer =
                        StatementSelect().select(Expression::Function(
                            "fts3_tokenizer",
                            std::list<Expression>(2, BindParameter())));
                    std::shared_ptr<HandleStatement> handleStatement =
                        handle->prepare(FTS3Tokenizer);
                    if (!handleStatement) {
                        error = handle->getError();
                        return false;
                    }
                    handleStatement->bind<ColumnType::Text>(name.c_str(), 1);
                    handleStatement->bind<ColumnType::BLOB>(address, 2);
                    handleStatement->step();
                    if (!handleStatement->isOK()) {
                        error = handleStatement->getError();
                        return false;
                    }
                }
            }

            error.reset();
            return true;
        },
        Order::Tokenize);
}

const Configs BuiltinConfigs::default_({BuiltinConfig::trace,
                                        BuiltinConfig::basic,
                                        BuiltinConfig::checkpoint});

} //namespace WCDB