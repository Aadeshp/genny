// Copyright 2019-present MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gennylib/context.hpp>

#include <memory>
#include <set>
#include <filesystem>
#include <sstream>
#include <boost/algorithm/string.hpp>

#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <gennylib/Cast.hpp>
#include <gennylib/parallel.hpp>
#include <gennylib/v1/Sleeper.hpp>
#include <metrics/metrics.hpp>

#include <boost/algorithm/string.hpp>
using namespace boost::algorithm;

namespace genny {

// Default value selected from random.org, by selecting 2 random numbers
// between 1 and 10^9 and concatenating.
auto RNG_SEED_BASE = 269849313357703264;

WorkloadContext::WorkloadContext(const Node& node,
                                 Orchestrator& orchestrator,
                                 const Cast& cast,
                                 v1::PoolManager::OnCommandStartCallback apmCallback,
                                 bool dryRun)
    : v1::HasNode{node},
      _orchestrator{&orchestrator},
      _rateLimiters{10},
      _poolManager{apmCallback, dryRun},
      _workloadPath{node.key()} ,
      _coordinator{"",4400} {
    std::set<std::string> validSchemaVersions{"2018-07-01"};

    // This is good enough for now. Later can add a WorkloadContextValidator concept
    // and wire in a vector of those similar to how we do with the vector of Producers.
    if (const std::string schemaVersion = (*this)["SchemaVersion"].to<std::string>();
        validSchemaVersions.count(schemaVersion) != 1) {
        std::ostringstream errMsg;
        errMsg << "Invalid Schema Version: " << schemaVersion;
        throw InvalidConfigurationException(errMsg.str());
    }

    // Make sure we have a valid mongocxx instance happening here
    mongocxx::instance::current();

    // Set the metrics format information.
    auto format = ((*this)["Metrics"]["Format"])
                      .maybe<metrics::MetricsFormat>()
                      .value_or(metrics::MetricsFormat("ftdc"));

    if (format != genny::metrics::MetricsFormat("ftdc")) {
        BOOST_LOG_TRIVIAL(info) << "Metrics format " << format.toString()
                                << " is deprecated in favor of ftdc.";
    }

    auto metricsPath =
        ((*this)["Metrics"]["Path"]).maybe<std::string>().value_or("build/WorkloadOutput/CedarMetrics");

    _registry = genny::metrics::Registry(std::move(format), std::move(metricsPath));

    _seedGenerator.seed((*this)["RandomSeed"].maybe<long>().value_or(RNG_SEED_BASE));

    // Make a bunch of actor contexts
    for (const auto& [k, actor] : (*this)["Actors"]) {
        _actorContexts.emplace_back(std::make_unique<genny::ActorContext>(actor, *this));
    }

    ActorBucket bucket;
    parallelRun(_actorContexts,
                   [&](const auto& actorContext) {
                       auto rawActors = _constructActors(cast, actorContext);
                       for (auto&& actor : rawActors) {
                           bucket.addItem(std::move(actor));
                       }
                   });

    _actors = std::move(bucket.extractItems());
    this->_orchestrator->addPrePhaseStartHook(
        [&](const Orchestrator*orchestrator, PhaseNumber phase) { this->_coordinator.onPhaseStart(phase); });
    this->_orchestrator->addPostPhaseStopHook(
        [&](const Orchestrator*orchestrator, PhaseNumber phase) { this->_coordinator.onPhaseStop(phase); });
    _done = true;
}

ActorVector WorkloadContext::_constructActors(const Cast& cast,
                                              const std::unique_ptr<ActorContext>& actorContext) {
    auto actors = ActorVector{};
    auto name = (*actorContext)["Type"].to<std::string>();

    std::shared_ptr<ActorProducer> producer;
    try {
        producer = cast.getProducer(name);
    } catch (const std::out_of_range&) {
        std::ostringstream stream;
        stream << "Unable to construct actors: No producer for '" << name << "'." << std::endl;
        cast.streamProducersTo(stream);
        throw InvalidConfigurationException(stream.str());
    }

    auto rawActors = producer->produce(*actorContext);
    for (auto&& actor : rawActors) {
        actors.emplace_back(std::forward<std::unique_ptr<Actor>>(actor));
    }
    return actors;
}

mongocxx::pool::entry WorkloadContext::client(const std::string& name, size_t instance) {
    return _poolManager.client(name, instance, this->_node);
}

GlobalRateLimiter* WorkloadContext::getRateLimiter(const std::string& name, const RateSpec& spec) {
    if (this->isDone()) {
        BOOST_THROW_EXCEPTION(
            std::logic_error("Cannot create rate-limiters after setup. Name tried: " + name));
    }

    std::lock_guard<std::mutex> lk(_limiterLock);
    if (_rateLimiters.count(name) == 0) {
        _rateLimiters.emplace(std::make_pair(name, std::make_unique<GlobalRateLimiter>(spec)));
    }
    auto rl = _rateLimiters[name].get();
    rl->addUser();

    // Reset the rate-limiter at the start of every Phase
    this->_orchestrator->addPrePhaseStartHook(
        [rl](const Orchestrator*, PhaseNumber phase) { rl->resetLastEmptied(); });
    return rl;
}

std::map<PhaseNumber, std::vector<std::reference_wrapper<const PhaseContext>>>
WorkloadContext::getActivePhaseContexts() const {
    auto phasesMap =
        std::map<PhaseNumber, std::vector<std::reference_wrapper<const PhaseContext>>>();
    for (const auto& ac : this->_actorContexts) {
        for (const auto& [phaseNum, phaseContext] : ac->phases()) {
            if (!phaseContext->isNop()) {
                phasesMap[phaseNum].emplace_back(std::cref(*phaseContext));
            }
        }
    }
    return phasesMap;
}

DefaultRandom& WorkloadContext::getRNGForThread(ActorId id) {
    if (this->isDone()) {
        BOOST_THROW_EXCEPTION(std::logic_error("Cannot create RNGs after setup"));
    }

    if (id < 1) {
        BOOST_THROW_EXCEPTION(std::logic_error("ActorId must be 1 or greater."));
    }

    return _rngRegistry[id - 1];
}

// Helper method that constructs all the IDs up to the given ID.
void WorkloadContext::_constructRngsToId(ActorId id) {
    for (auto i = _rngRegistry.size(); i < id; i++) {
        _rngRegistry.emplace_back(_seedGenerator());
    }
}

// Helper method to convert Phases:[...] to PhaseContexts
std::unordered_map<PhaseNumber, std::unique_ptr<PhaseContext>> ActorContext::constructPhaseContexts(
    const Node&, ActorContext* actorContext) {
    std::unordered_map<PhaseNumber, std::unique_ptr<PhaseContext>> out;
    auto& phases = (*actorContext)["Phases"];
    if (!phases) {
        return out;
    }
    PhaseNumber lastPhaseNumber = 0;
    for (const auto& [k, phase] : phases) {
        // If we don't have a node or we are a null type, then we are a Nop
        if (!phase || phase.isNull()) {
            std::ostringstream ss;
            ss << "Encountered a null/empty phase. "
                  "Every phase should have at least be an empty map.";
            throw InvalidConfigurationException(ss.str());
        }
        PhaseRangeSpec configuredRange = phase["Phase"].maybe<PhaseRangeSpec>().value_or(
            PhaseRangeSpec{IntegerSpec{lastPhaseNumber}});
        for (PhaseNumber rangeIndex = configuredRange.start; rangeIndex <= configuredRange.end;
             rangeIndex++) {
            auto [it, success] = out.try_emplace(
                rangeIndex, std::make_unique<PhaseContext>(phase, lastPhaseNumber, *actorContext));
            if (!success) {
                std::stringstream msg;
                msg << "Duplicate phase " << rangeIndex;
                throw InvalidConfigurationException(msg.str());
            }
            lastPhaseNumber++;
        }
    }
    actorContext->orchestrator().phasesAtLeastTo(out.size() - 1);
    return out;
}

// The SleepContext class is basically an actor-friendly adapter
// for the Sleeper.
void SleepContext::sleep_for(Duration duration) const {
    // We don't care about the before/after op distinction here.
    v1::Sleeper sleeper(duration, Duration::zero());
    sleeper.before(_orchestrator, _phase);
}

bool PhaseContext::isNop() const {
    auto& nop = (*this)["Nop"];
    return nop.maybe<bool>().value_or(false);
}

// Connect to the specified IP Address (use INADDR_ANY if the IP is blank) and port. If the connect
// call succeeds then the socket is returned, otherwise return -1.
int connect(std::string ipaddress, int port) {
    struct sockaddr_in addr = {0};
    addr.sin_addr.s_addr = INADDR_ANY;
    if (!ipaddress.empty()) {
        addr.sin_addr.s_addr = inet_addr(ipaddress.c_str());
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons( port );

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if(connect(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        BOOST_LOG_TRIVIAL(warning) << "connect('" << ipaddress << "',"
                                   << port << "') failed --> '"<< ::strerror(errno) << "'";
        return -1;
    }
    return sd;
}

// Note: this constructor calls connect to ensure that the socket connection is acquired before the
// instance is available to use.
ExternalPhaseCoordinator::ExternalPhaseCoordinator(std::string host, int port) :
      m_host(host), m_port(port), m_socket(connect(host, port)){
    BOOST_LOG_TRIVIAL(info) << "ExternalPhaseCoordinator::ExternalPhaseCoordinator('"
                            << m_host << "'," << m_port << "') --> " << m_socket;
}
void ExternalPhaseCoordinator::_onPhase(std::string event){
    BOOST_LOG_TRIVIAL(debug) << "ExternalPhaseCoordinator::onPhase('" << event << "') start";
    if(m_socket >= 0) {
        write(m_socket, event.c_str(), event.length());
        char buf[1024];
        bzero(buf, sizeof(buf));
        int rval = read(m_socket, buf, 1024);
        std::string response(buf);
        boost::trim(response);
        BOOST_LOG_TRIVIAL(debug) << "ExternalPhaseCoordinator::onPhase' read '"
                                 << response << "' " << rval ;
    }
    BOOST_LOG_TRIVIAL(debug) << "ExternalPhaseCoordinator::onPhase('" << event << "') end";
}
void ExternalPhaseCoordinator::onPhaseStart(PhaseNumber phase){
    std::stringstream event;
    event << R"({ "message": ")" << "Beginning phase" << R"(","phase":)"
          <<  phase << R"(,"request":)" <<  1 << "}" << std::endl;
    _onPhase(event.str());
}
void ExternalPhaseCoordinator::onPhaseStop(PhaseNumber phase){
    std::stringstream event;
    event << R"({ "message": ")" << "Ended phase" << R"(","phase":)"
          <<  phase << R"(,"request":)" <<  1 << "}" << std::endl;
    _onPhase(event.str());
}

}  // namespace genny
