////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#include <Basics/ResultT.h>
#include <Basics/Exceptions.h>
#include <Basics/voc-errors.h>
#include <Futures/Future.h>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "Replication2/StateMachines/Prototype/PrototypeStateMachine.h"
#include "Network/Methods.h"
#include "VocBase/vocbase.h"

#include "PrototypeStateMethods.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::prototype;

struct PrototypeStateMethodsDBServer final : PrototypeStateMethods {
  explicit PrototypeStateMethodsDBServer(TRI_vocbase_t& vocbase)
      : _vocbase(vocbase) {}

  [[nodiscard]] auto insert(
      LogId id,
      std::unordered_map<std::string, std::string> const& entries) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->set(entries);
  };

  [[nodiscard]] auto get(LogId id, std::string key) const
      -> futures::Future<std::optional<std::string>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->get(key);
  }

  [[nodiscard]] auto get(LogId id, std::vector<std::string> keys) const
      -> futures::Future<
          std::unordered_map<std::string, std::string>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->get(keys.begin(), keys.end());
  }

  [[nodiscard]] auto getSnapshot(LogId id, LogIndex waitForIndex) const
      -> futures::Future<
          ResultT<std::unordered_map<std::string, std::string>>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->getSnapshot(waitForIndex);
  }

  [[nodiscard]] auto remove(LogId id, std::string key) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->remove(std::move(key));
  }

  [[nodiscard]] auto remove(LogId id, std::vector<std::string> keys) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto leader = getPrototypeStateLeaderById(id);
    return leader->remove(std::move(keys));
  }

 private:
  [[nodiscard]] auto getPrototypeStateLeaderById(LogId id) const
      -> std::shared_ptr<PrototypeLeaderState> {
    auto stateMachine =
        std::dynamic_pointer_cast<ReplicatedState<PrototypeState>>(
            _vocbase.getReplicatedStateById(id));
    if (stateMachine == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, basics::StringUtils::concatT(
                                  "Failed to get ProtoypeState with id ", id));
    }
    auto leader = stateMachine->getLeader();
    if (leader == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          basics::StringUtils::concatT(
              "Failed to get leader of ProtoypeState with id ", id));
    }
    return leader;
  }

 private:
  TRI_vocbase_t& _vocbase;
};

struct PrototypeStateMethodsCoordinator final : PrototypeStateMethods {
  [[nodiscard]] auto insert(
      LogId id,
      std::unordered_map<std::string, std::string> const& entries) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto path =
        basics::StringUtils::joinT("/", "_api/prototype-state", id, "insert");
    network::RequestOptions opts;
    opts.database = _vocbase.name();
    VPackBuilder builder{};
    {
      VPackObjectBuilder ob{&builder};
      for (auto const& [key, value] : entries) {
        builder.add(key, VPackValue(value));
      }
    }
    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path,
                                builder.bufferRef(), opts)
        .thenValue([](network::Response&& resp) -> ResultT<LogIndex> {
          return processLogIndexResponse(std::move(resp));
        });
  }

  [[nodiscard]] auto get(LogId id, std::string key) const
      -> futures::Future<std::optional<std::string>> override {
    auto path = basics::StringUtils::joinT("/", "_api/prototype-state", id,
                                           "entry", key);
    network::RequestOptions opts;
    opts.database = _vocbase.name();
    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue([](network::Response&& resp) -> std::optional<std::string> {
          if (resp.statusCode() == fuerte::StatusNotFound) {
            return std::nullopt;
          } else if (resp.fail() ||
                     !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          } else {
            auto slice = resp.slice();
            if (auto result = slice.get("result");
                result.isObject() && result.length() == 1) {
              return result.valueAt(0).copyString();
            }
            THROW_ARANGO_EXCEPTION_MESSAGE(
                TRI_ERROR_INTERNAL, basics::StringUtils::concatT(
                                        "expected result containing key-value "
                                        "pair in leader response: ",
                                        slice.toJson()));
          }
        });
  }

  [[nodiscard]] auto get(LogId id, std::vector<std::string> keys) const
      -> futures::Future<
          std::unordered_map<std::string, std::string>> override {
    auto path = basics::StringUtils::joinT("/", "_api/prototype-state", id,
                                           "multi-get");
    network::RequestOptions opts;
    opts.database = _vocbase.name();

    VPackBuilder builder{};
    {
      VPackArrayBuilder ab{&builder};
      for (auto const& key : keys) {
        builder.add(VPackValue(key));
      }
    }

    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path,
                                builder.bufferRef(), opts)
        .thenValue([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          } else {
            auto slice = resp.slice();
            if (auto result = slice.get("result"); result.isObject()) {
              std::unordered_map<std::string, std::string> map;
              for (auto it : VPackObjectIterator{result}) {
                map.emplace(it.key.copyString(), it.value.copyString());
              }
              return map;
            }
            THROW_ARANGO_EXCEPTION_MESSAGE(
                TRI_ERROR_INTERNAL,
                basics::StringUtils::concatT("expected result containing map "
                                             "in leader response: ",
                                             slice.toJson()));
          }
        });
  }

  [[nodiscard]] auto getSnapshot(LogId id, LogIndex waitForIndex) const
      -> futures::Future<
          ResultT<std::unordered_map<std::string, std::string>>> override {
    auto path =
        basics::StringUtils::joinT("/", "_api/prototype-state", id, "snapshot");
    network::RequestOptions opts;
    opts.database = _vocbase.name();
    opts.param("waitForIndex", std::to_string(waitForIndex.value));

    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue(
            [](network::Response&& resp)
                -> ResultT<std::unordered_map<std::string, std::string>> {
              if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
                THROW_ARANGO_EXCEPTION(resp.combinedResult());
              } else {
                auto slice = resp.slice();
                if (auto result = slice.get("result"); result.isObject()) {
                  std::unordered_map<std::string, std::string> map;
                  for (auto it : VPackObjectIterator{result}) {
                    map.emplace(it.key.copyString(), it.value.copyString());
                  }
                  return map;
                }
                THROW_ARANGO_EXCEPTION_MESSAGE(
                    TRI_ERROR_INTERNAL, basics::StringUtils::concatT(
                                            "expected result containing map "
                                            "in leader response: ",
                                            slice.toJson()));
              }
            });
  }

  [[nodiscard]] auto remove(LogId id, std::string key) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto path = basics::StringUtils::joinT("/", "_api/prototype-state", id,
                                           "entry", key);
    network::RequestOptions opts;
    opts.database = _vocbase.name();
    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Delete, path, {}, opts)
        .thenValue([](network::Response&& resp) -> ResultT<LogIndex> {
          return processLogIndexResponse(std::move(resp));
        });
  }

  [[nodiscard]] auto remove(LogId id, std::vector<std::string> keys) const
      -> futures::Future<ResultT<LogIndex>> override {
    auto path = basics::StringUtils::joinT("/", "_api/prototype-state", id,
                                           "multi-remove");
    network::RequestOptions opts;
    opts.database = _vocbase.name();
    VPackBuilder builder{};
    {
      VPackArrayBuilder ab{&builder};
      for (auto const& key : keys) {
        builder.add(VPackValue(key));
      }
    }
    return network::sendRequest(_pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Delete, path,
                                builder.bufferRef(), opts)
        .thenValue([](network::Response&& resp) -> ResultT<LogIndex> {
          return processLogIndexResponse(std::move(resp));
        });
  }

  explicit PrototypeStateMethodsCoordinator(TRI_vocbase_t& vocbase)
      : _vocbase(vocbase),
        _clusterInfo(
            vocbase.server().getFeature<ClusterFeature>().clusterInfo()),
        _pool(vocbase.server().getFeature<NetworkFeature>().pool()) {}

 private:
  [[nodiscard]] auto getLogLeader(LogId id) const -> ServerID {
    auto leader = _clusterInfo.getReplicatedLogLeader(_vocbase.name(), id);
    if (leader.fail()) {
      if (leader.is(TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED)) {
        throw ParticipantResignedException(leader.result(), ADB_HERE);
      } else {
        THROW_ARANGO_EXCEPTION(leader.result());
      }
    }
    return *leader;
  }

  static auto processLogIndexResponse(network::Response&& resp) -> LogIndex {
    if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
      THROW_ARANGO_EXCEPTION(resp.combinedResult());
    } else {
      auto slice = resp.slice();
      if (auto result = slice.get("result");
          result.isObject() && result.length() == 1) {
        return result.get("index").extract<LogIndex>();
      }
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          basics::StringUtils::concatT(
              "expected result containing index in leader response: ",
              slice.toJson()));
    }
  }

 public:
  TRI_vocbase_t& _vocbase;
  ClusterInfo& _clusterInfo;
  network::ConnectionPool* _pool;
};

auto PrototypeStateMethods::createInstance(TRI_vocbase_t& vocbase)
    -> std::shared_ptr<PrototypeStateMethods> {
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_COORDINATOR:
      return std::make_shared<PrototypeStateMethodsCoordinator>(vocbase);
    case ServerState::ROLE_DBSERVER:
      return std::make_shared<PrototypeStateMethodsDBServer>(vocbase);
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "api only on available coordinators or dbservers");
  }
}