/*
 * Copyright 2014 CyberVision, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kaa/channel/impl/DefaultOperationLongPollChannel.hpp"
#include "kaa/logging/Log.hpp"
#include "kaa/security/RsaEncoderDecoder.hpp"
#include "kaa/common/AvroByteArrayConverter.hpp"
#include "kaa/http/IHttpResponse.hpp"
#include "kaa/http/IHttpRequest.hpp"
#include "kaa/http/MultipartPostHttpRequest.hpp"

namespace kaa {

const std::string DefaultOperationLongPollChannel::CHANNEL_ID = "default_operations_long_poll_channel";
const std::map<TransportType, ChannelDirection> DefaultOperationLongPollChannel::SUPPORTED_TYPES =
        {
                { TransportType::PROFILE, ChannelDirection::BIDIRECTIONAL },
                { TransportType::CONFIGURATION, ChannelDirection::BIDIRECTIONAL },
                { TransportType::NOTIFICATION, ChannelDirection::BIDIRECTIONAL },
                { TransportType::USER, ChannelDirection::BIDIRECTIONAL },
                { TransportType::EVENT, ChannelDirection::DOWN }
        };

DefaultOperationLongPollChannel::DefaultOperationLongPollChannel(IKaaChannelManager *channelManager, const KeyPair& clientKeys)
    : clientKeys_(clientKeys), work_(io_), pollThread_()
    , stopped_(true), connectionInProgress_(false), taskPosted_(false), firstStart_(true)
    , multiplexer_(nullptr), demultiplexer_(nullptr), channelManager_(channelManager) {}

void DefaultOperationLongPollChannel::startPoll()
{
    KAA_LOG_INFO("Starting poll scheduler..");

    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    if (firstStart_) {
        KAA_LOG_INFO(boost::format("First start for channel %1%. Creating a thread...") % getId());
        pollThread_ = boost::thread([this](){ this->io_.run(); });
        firstStart_ = false;
    }
    if (stopped_) {
        stopped_ = false;
        postTask();
        KAA_LOG_INFO("Poll scheduler started");
    } else {
        KAA_LOG_INFO("Poll scheduler is already started");
    }
}

void DefaultOperationLongPollChannel::stopPoll()
{
    KAA_LOG_INFO("Stopping poll future..");

    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    if (!stopped_) {
        stopped_ = true;
        if (connectionInProgress_) {
            httpClient_.closeConnection();
            waitCondition_.wait(lock, [this](){ return !this->connectionInProgress_; });
        }
    }
}

void DefaultOperationLongPollChannel::postTask()
{
    io_.post([this](){ this->executeTask(); });
    taskPosted_ = true;
}

void DefaultOperationLongPollChannel::executeTask()
{
    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    taskPosted_ = false;
    if (stopped_) {
        return;
    }
    connectionInProgress_ = true;

    OperationServerLongPollInfoPtr server = currentServer_;

    const auto& bodyRaw = multiplexer_->compileRequest(getSupportedTransportTypes());
    // Creating HTTP request using the given data
    boost::shared_ptr<IHttpRequest> postRequest = httpDataProcessor_.createOperationRequest(server->getUrl(), bodyRaw);

    KAA_MUTEX_UNLOCKING("channelGuard_");
    lock.unlock();
    KAA_MUTEX_UNLOCKED("channelGuard_");
    try {
        // Sending http request
        auto response = httpClient_.sendRequest(*postRequest);
        KAA_MUTEX_LOCKING("channelGuard_");
        boost::unique_lock<boost::mutex> lockInternal(channelGuard_);
        KAA_MUTEX_LOCKED("channelGuard_");
        // Retrieving the avro data from the HTTP response
        connectionInProgress_ = false;
        const std::string& processedResponse = httpDataProcessor_.retrieveOperationResponse(*response);
        KAA_MUTEX_UNLOCKING("channelGuard_");
        lockInternal.unlock();
        KAA_MUTEX_UNLOCKED("channelGuard_");
        demultiplexer_->processResponse(
                std::vector<boost::uint8_t>(reinterpret_cast<const boost::uint8_t *>(processedResponse.data()),
                                            reinterpret_cast<const boost::uint8_t *>(processedResponse.data() + processedResponse.size())));
        waitCondition_.notify_all();
    } catch (std::exception& e) {
        KAA_MUTEX_LOCKING("channelGuard_");
        boost::unique_lock<boost::mutex> lockException(channelGuard_);
        KAA_MUTEX_LOCKED("channelGuard_");

        bool isServerFailed = false;
        connectionInProgress_ = false;
        if (stopped_) {
            KAA_LOG_INFO(boost::format("Connection for channel %1% was aborted") % getId());
        } else {
            KAA_LOG_ERROR(boost::format("Connection failed, server %1%:%2%: %3%") % server->getHost() % server->getPort() % e.what());
            isServerFailed = true;
            stopped_ = true;
        }
        KAA_MUTEX_UNLOCKING("channelGuard_");
        lockException.unlock();
        KAA_MUTEX_UNLOCKED("channelGuard_");

        waitCondition_.notify_all();
        if (isServerFailed) {
            channelManager_->onServerFailed(server);
        }
        return;
    }

    KAA_MUTEX_LOCKING("channelGuard_");
    lock.lock();
    KAA_MUTEX_LOCKED("channelGuard_");
    if (!stopped_ && !taskPosted_) {
        postTask();
    }
}

void DefaultOperationLongPollChannel::sync(TransportType type)
{
    auto it = SUPPORTED_TYPES.find(type);
    if (it != SUPPORTED_TYPES.end() && (it->second == ChannelDirection::UP || it->second == ChannelDirection::BIDIRECTIONAL)) {
        KAA_MUTEX_LOCKING("channelGuard_");
        boost::unique_lock<boost::mutex> lock(channelGuard_);
        KAA_MUTEX_LOCKED("channelGuard_");
        if (currentServer_) {
            KAA_MUTEX_UNLOCKING("channelGuard_");
            lock.unlock();
            KAA_MUTEX_UNLOCKED("channelGuard_");

            stopPoll();
            startPoll();
        } else {
            KAA_LOG_WARN(boost::format("Can't sync channel %1%. Server is null") % getId());
        }
    } else {
        KAA_LOG_ERROR(boost::format("Unsupported transport type for channel %1%") % getId());
    }
}

void DefaultOperationLongPollChannel::syncAll()
{
    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    if (currentServer_) {
        KAA_MUTEX_UNLOCKING("channelGuard_");
        lock.unlock();
        KAA_MUTEX_UNLOCKED("channelGuard_");

        stopPoll();
        startPoll();
    } else {
        KAA_LOG_WARN(boost::format("Can't sync channel %1%. Server is null") % getId());
    }
}

void DefaultOperationLongPollChannel::setMultiplexer(IKaaDataMultiplexer *multiplexer)
{
    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    multiplexer_ = multiplexer;
}

void DefaultOperationLongPollChannel::setDemultiplexer(IKaaDataDemultiplexer *demultiplexer)
{
    KAA_MUTEX_LOCKING("channelGuard_");
    boost::unique_lock<boost::mutex> lock(channelGuard_);
    KAA_MUTEX_LOCKED("channelGuard_");
    demultiplexer_ = demultiplexer;
}

void DefaultOperationLongPollChannel::setServer(IServerInfoPtr server)
{
    if (server->getType() == ChannelType::HTTP_LP) {
        stopPoll();
        KAA_MUTEX_LOCKING("channelGuard_");
        boost::unique_lock<boost::mutex> lock(channelGuard_);
        KAA_MUTEX_LOCKED("channelGuard_");
        currentServer_ = boost::dynamic_pointer_cast<OperationServerLongPollInfo, IServerInfo>(server);
        boost::shared_ptr<IEncoderDecoder> encDec(new RsaEncoderDecoder(clientKeys_.first, clientKeys_.second, currentServer_->getPublicKey()));
        httpDataProcessor_.setEncoderDecoder(encDec);
        KAA_MUTEX_UNLOCKING("channelGuard_");
        lock.unlock();
        KAA_MUTEX_UNLOCKED("channelGuard_");
        startPoll();
    } else {
        KAA_LOG_ERROR(boost::format("Invalid server info for channel %1%") % getId());
    }
}

}
