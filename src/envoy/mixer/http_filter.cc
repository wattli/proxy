/* Copyright 2017 Istio Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "precompiled/precompiled.h"

#include "common/common/base64.h"
#include "common/common/logger.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "envoy/server/instance.h"
#include "envoy/ssl/connection.h"
#include "server/config/network/http_connection_manager.h"
#include "src/envoy/mixer/http_control.h"
#include "src/envoy/mixer/utils.h"

using ::google::protobuf::util::Status;
using StatusCode = ::google::protobuf::util::error::Code;
using ::istio::mixer_client::DoneFunc;

namespace Http {
namespace Mixer {
namespace {

// The Json object name for mixer-server.
const std::string kJsonNameMixerServer("mixer_server");

// The Json object name for static attributes.
const std::string kJsonNameMixerAttributes("mixer_attributes");

// The Json object name to specify attributes which will be forwarded
// to the upstream istio proxy.
const std::string kJsonNameForwardAttributes("forward_attributes");

// Switch to turn off attribute forwarding
const std::string kJsonNameForwardSwitch("mixer_forward");

// Switch to turn off mixer check/report/quota
const std::string kJsonNameMixerSwitch("mixer_control");

// Convert Status::code to HTTP code
int HttpCode(int code) {
  // Map Canonical codes to HTTP status codes. This is based on the mapping
  // defined by the protobuf http error space.
  switch (code) {
    case StatusCode::OK:
      return 200;
    case StatusCode::CANCELLED:
      return 499;
    case StatusCode::UNKNOWN:
      return 500;
    case StatusCode::INVALID_ARGUMENT:
      return 400;
    case StatusCode::DEADLINE_EXCEEDED:
      return 504;
    case StatusCode::NOT_FOUND:
      return 404;
    case StatusCode::ALREADY_EXISTS:
      return 409;
    case StatusCode::PERMISSION_DENIED:
      return 403;
    case StatusCode::RESOURCE_EXHAUSTED:
      return 429;
    case StatusCode::FAILED_PRECONDITION:
      return 400;
    case StatusCode::ABORTED:
      return 409;
    case StatusCode::OUT_OF_RANGE:
      return 400;
    case StatusCode::UNIMPLEMENTED:
      return 501;
    case StatusCode::INTERNAL:
      return 500;
    case StatusCode::UNAVAILABLE:
      return 503;
    case StatusCode::DATA_LOSS:
      return 500;
    case StatusCode::UNAUTHENTICATED:
      return 401;
    default:
      return 500;
  }
}

}  // namespace

class Config : public Logger::Loggable<Logger::Id::http> {
 private:
  std::shared_ptr<HttpControl> http_control_;
  Upstream::ClusterManager& cm_;
  std::string forward_attributes_;

 public:
  Config(const Json::Object& config, Server::Instance& server)
      : cm_(server.clusterManager()) {
    std::string mixer_server;
    if (config.hasObject(kJsonNameMixerServer)) {
      mixer_server = config.getString(kJsonNameMixerServer);
    } else {
      log().error(
          "mixer_server is required but not specified in the config: {}",
          __func__);
    }

    Utils::StringMap attributes =
        Utils::ExtractStringMap(config, kJsonNameForwardAttributes);
    if (!attributes.empty()) {
      std::string serialized_str = Utils::SerializeStringMap(attributes);
      forward_attributes_ =
          Base64::encode(serialized_str.c_str(), serialized_str.size());
      log().debug("Mixer forward attributes set: ", serialized_str);
    }

    std::map<std::string, std::string> mixer_attributes =
        Utils::ExtractStringMap(config, kJsonNameMixerAttributes);

    http_control_ = std::make_shared<HttpControl>(mixer_server,
                                                  std::move(mixer_attributes));
    log().debug("Called Mixer::Config constructor with mixer_server: ",
                mixer_server);
  }

  std::shared_ptr<HttpControl>& http_control() { return http_control_; }
  const std::string& forward_attributes() const { return forward_attributes_; }
};

typedef std::shared_ptr<Config> ConfigPtr;

class Instance : public Http::StreamDecoderFilter,
                 public Http::AccessLog::Instance {
 private:
  std::shared_ptr<HttpControl> http_control_;
  ConfigPtr config_;
  std::shared_ptr<HttpRequestData> request_data_;

  enum State { NotStarted, Calling, Complete, Responded };
  State state_;

  StreamDecoderFilterCallbacks* decoder_callbacks_;

  bool initiating_call_;
  int check_status_code_;

  bool mixer_disabled_;

  // mixer control switch (off by default)
  bool mixer_disabled() {
    auto route = decoder_callbacks_->route();
    if (route != nullptr) {
      auto entry = route->routeEntry();
      if (entry != nullptr) {
        auto key = entry->opaqueConfig().find(kJsonNameMixerSwitch);
        if (key != entry->opaqueConfig().end() && key->second == "on") {
          return false;
        }
      }
    }
    return true;
  }

  // attribute forward switch (on by default)
  bool forward_disabled() {
    auto route = decoder_callbacks_->route();
    if (route != nullptr) {
      auto entry = route->routeEntry();
      if (entry != nullptr) {
        auto key = entry->opaqueConfig().find(kJsonNameForwardSwitch);
        if (key != entry->opaqueConfig().end() && key->second == "off") {
          return true;
        }
      }
    }
    return false;
  }

 public:
  Instance(ConfigPtr config)
      : http_control_(config->http_control()),
        config_(config),
        state_(NotStarted),
        initiating_call_(false),
        check_status_code_(HttpCode(StatusCode::UNKNOWN)) {
    Log().debug("Called Mixer::Instance : {}", __func__);
  }

  // Jump thread; on_done will be called at the dispatcher thread.
  DoneFunc wrapper(DoneFunc on_done) {
    auto& dispatcher = decoder_callbacks_->dispatcher();
    return [&dispatcher, on_done](const Status& status) {
      dispatcher.post([status, on_done]() { on_done(status); });
    };
  }

  FilterHeadersStatus decodeHeaders(HeaderMap& headers,
                                    bool end_stream) override {
    Log().debug("Called Mixer::Instance : {}", __func__);

    if (!config_->forward_attributes().empty() && !forward_disabled()) {
      headers.addStatic(Utils::kIstioAttributeHeader,
                        config_->forward_attributes());
    }

    mixer_disabled_ = mixer_disabled();
    if (mixer_disabled_) {
      return FilterHeadersStatus::Continue;
    }

    state_ = Calling;
    initiating_call_ = true;
    request_data_ = std::make_shared<HttpRequestData>();

    std::string origin_user;
    Ssl::Connection* ssl =
        const_cast<Ssl::Connection*>(decoder_callbacks_->ssl());
    if (ssl != nullptr) {
      origin_user = ssl->uriSanPeerCertificate();
    }

    http_control_->Check(
        request_data_, headers, origin_user,
        wrapper([this](const Status& status) { completeCheck(status); }));
    initiating_call_ = false;

    if (state_ == Complete) {
      return FilterHeadersStatus::Continue;
    }
    Log().debug("Called Mixer::Instance : {} Stop", __func__);
    return FilterHeadersStatus::StopIteration;
  }

  FilterDataStatus decodeData(Buffer::Instance& data,
                              bool end_stream) override {
    if (mixer_disabled_) {
      return FilterDataStatus::Continue;
    }

    Log().debug("Called Mixer::Instance : {} ({}, {})", __func__, data.length(),
                end_stream);
    if (state_ == Calling) {
      return FilterDataStatus::StopIterationAndBuffer;
    }
    return FilterDataStatus::Continue;
  }

  FilterTrailersStatus decodeTrailers(HeaderMap& trailers) override {
    if (mixer_disabled_) {
      return FilterTrailersStatus::Continue;
    }

    Log().debug("Called Mixer::Instance : {}", __func__);
    if (state_ == Calling) {
      return FilterTrailersStatus::StopIteration;
    }
    return FilterTrailersStatus::Continue;
  }

  void setDecoderFilterCallbacks(
      StreamDecoderFilterCallbacks& callbacks) override {
    Log().debug("Called Mixer::Instance : {}", __func__);
    decoder_callbacks_ = &callbacks;
    decoder_callbacks_->addResetStreamCallback(
        [this]() { state_ = Responded; });
  }

  void completeCheck(const Status& status) {
    Log().debug("Called Mixer::Instance : check complete {}",
                status.ToString());

    if (!status.ok() && state_ != Responded) {
      state_ = Responded;
      check_status_code_ = HttpCode(status.error_code());
      Utility::sendLocalReply(*decoder_callbacks_, Code(check_status_code_),
                              status.ToString());
      return;
    }

    state_ = Complete;
    if (!initiating_call_) {
      decoder_callbacks_->continueDecoding();
    }
  }

  virtual void log(const HeaderMap* request_headers,
                   const HeaderMap* response_headers,
                   const AccessLog::RequestInfo& request_info) override {
    Log().debug("Called Mixer::Instance : {}", __func__);
    // If decodeHaeders() is not called, not to call Mixer report.
    if (!request_data_) return;
    // Make sure not to use any class members at the callback.
    // The class may be gone when it is called.
    // Log() is a static function so it is OK.
    http_control_->Report(request_data_, response_headers, request_info,
                          check_status_code_, [](const Status& status) {
                            Log().debug("Report returns status: {}",
                                        status.ToString());
                          });
  }

  static spdlog::logger& Log() {
    static spdlog::logger& instance =
        Logger::Registry::getLog(Logger::Id::http);
    return instance;
  }
};

}  // namespace Mixer
}  // namespace Http

namespace Server {
namespace Configuration {

class MixerConfig : public HttpFilterConfigFactory {
 public:
  HttpFilterFactoryCb tryCreateFilterFactory(
      HttpFilterType type, const std::string& name, const Json::Object& config,
      const std::string&, Server::Instance& server) override {
    if (type != HttpFilterType::Decoder || name != "mixer") {
      return nullptr;
    }

    Http::Mixer::ConfigPtr mixer_config(
        new Http::Mixer::Config(config, server));
    return [mixer_config](
               Http::FilterChainFactoryCallbacks& callbacks) -> void {
      std::shared_ptr<Http::Mixer::Instance> instance(
          new Http::Mixer::Instance(mixer_config));
      callbacks.addStreamDecoderFilter(Http::StreamDecoderFilterPtr(instance));
      callbacks.addAccessLogHandler(Http::AccessLog::InstancePtr(instance));
    };
  }
};

static RegisterHttpFilterConfigFactory<MixerConfig> register_;

}  // namespace Configuration
}  // namespace server
