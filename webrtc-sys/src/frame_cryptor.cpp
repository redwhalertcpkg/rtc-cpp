/*
 * Copyright 2023 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the “License”);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/frame_cryptor.h"

#include <memory>

#include "absl/types/optional.h"
#include "api/make_ref_counted.h"
#include "livekit/peer_connection.h"
#include "livekit/peer_connection_factory.h"
#include "livekit/webrtc.h"
#include "rtc_base/thread.h"
#include "webrtc-sys/src/frame_cryptor.rs.h"

namespace livekit {

webrtc::FrameCryptorTransformer::Algorithm AlgorithmToFrameCryptorAlgorithm(
    Algorithm algorithm) {
  switch (algorithm) {
    case Algorithm::AesGcm:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    case Algorithm::AesCbc:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesCbc;
    case Algorithm::Sm4Gcm:
      // return webrtc::FrameCryptorTransformer::Algorithm::kSm4Gcm;
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    case Algorithm::Sm4Cbc:
      // return webrtc::FrameCryptorTransformer::Algorithm::kSm4Cbc;
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    default:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
  }
}

KeyProvider::KeyProvider(KeyProviderOptions options) {
  webrtc::KeyProviderOptions rtc_options;
  rtc_options.shared_key = options.shared_key;

  std::vector<uint8_t> ratchet_salt;
  std::copy(options.ratchet_salt.begin(), options.ratchet_salt.end(),
            std::back_inserter(ratchet_salt));

  rtc_options.ratchet_salt = ratchet_salt;
  rtc_options.ratchet_window_size = options.ratchet_window_size;
  rtc_options.failure_tolerance = options.failure_tolerance;

  impl_ =
      new rtc::RefCountedObject<webrtc::DefaultKeyProviderImpl>(rtc_options);
}

FrameCryptor::FrameCryptor(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    const std::string participant_id,
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    rtc::scoped_refptr<webrtc::KeyProvider> key_provider,
    rtc::scoped_refptr<webrtc::RtpSenderInterface> sender)
    : rtc_runtime_(rtc_runtime),
      participant_id_(participant_id),
      key_provider_(key_provider),
      sender_(sender) {
  auto mediaType =
      sender->track()->kind() == "audio"
          ? webrtc::FrameCryptorTransformer::MediaType::kAudioFrame
          : webrtc::FrameCryptorTransformer::MediaType::kVideoFrame;
  e2ee_transformer_ = rtc::scoped_refptr<webrtc::FrameCryptorTransformer>(
      new webrtc::FrameCryptorTransformer(rtc_runtime->signaling_thread(),
                                          participant_id, mediaType, algorithm,
                                          key_provider_));
  sender->SetEncoderToPacketizerFrameTransformer(e2ee_transformer_);
  e2ee_transformer_->SetEnabled(false);
}

FrameCryptor::FrameCryptor(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    const std::string participant_id,
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    rtc::scoped_refptr<webrtc::KeyProvider> key_provider,
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)
    : rtc_runtime_(rtc_runtime),
      participant_id_(participant_id),
      key_provider_(key_provider),
      receiver_(receiver) {
  auto mediaType =
      receiver->track()->kind() == "audio"
          ? webrtc::FrameCryptorTransformer::MediaType::kAudioFrame
          : webrtc::FrameCryptorTransformer::MediaType::kVideoFrame;
  e2ee_transformer_ = rtc::scoped_refptr<webrtc::FrameCryptorTransformer>(
      new webrtc::FrameCryptorTransformer(rtc_runtime->signaling_thread(),
                                          participant_id, mediaType, algorithm,
                                          key_provider_));
  receiver->SetDepacketizerToDecoderFrameTransformer(e2ee_transformer_);
  e2ee_transformer_->SetEnabled(false);
}

FrameCryptor::~FrameCryptor() {
  if (observer_) {
    unregister_observer();
  }
}

void FrameCryptor::register_observer(
    rust::Box<RtcFrameCryptorObserverWrapper> observer) const {
  webrtc::MutexLock lock(&mutex_);
  observer_ = rtc::make_ref_counted<NativeFrameCryptorObserver>(
      std::move(observer), this);
  e2ee_transformer_->RegisterFrameCryptorTransformerObserver(observer_);
}

void FrameCryptor::unregister_observer() const {
  webrtc::MutexLock lock(&mutex_);
  observer_ = nullptr;
  e2ee_transformer_->UnRegisterFrameCryptorTransformerObserver();
}

NativeFrameCryptorObserver::NativeFrameCryptorObserver(
    rust::Box<RtcFrameCryptorObserverWrapper> observer,
    const FrameCryptor* fc)
    : observer_(std::move(observer)), fc_(fc) {}

NativeFrameCryptorObserver::~NativeFrameCryptorObserver() {}

void NativeFrameCryptorObserver::OnFrameCryptionStateChanged(
    const std::string participant_id,
    webrtc::FrameCryptionState state) {
  observer_->on_frame_cryption_state_change(
      participant_id, static_cast<FrameCryptionState>(state));
}

void FrameCryptor::set_enabled(bool enabled) const {
  webrtc::MutexLock lock(&mutex_);
  e2ee_transformer_->SetEnabled(enabled);
}

bool FrameCryptor::enabled() const {
  webrtc::MutexLock lock(&mutex_);
  return e2ee_transformer_->enabled();
}

void FrameCryptor::set_key_index(int32_t index) const {
  webrtc::MutexLock lock(&mutex_);
  e2ee_transformer_->SetKeyIndex(index);
}

int32_t FrameCryptor::key_index() const {
  webrtc::MutexLock lock(&mutex_);
  return e2ee_transformer_->key_index();
}

std::shared_ptr<KeyProvider> new_key_provider(KeyProviderOptions options) {
  return std::make_shared<KeyProvider>(options);
}

std::shared_ptr<FrameCryptor> new_frame_cryptor_for_rtp_sender(
    std::shared_ptr<PeerConnectionFactory> peer_factory,
    const ::rust::String participant_id,
    Algorithm algorithm,
    std::shared_ptr<KeyProvider> key_provider,
    std::shared_ptr<RtpSender> sender) {
  return std::make_shared<FrameCryptor>(
      peer_factory->rtc_runtime(),
      std::string(participant_id.data(), participant_id.size()),
      AlgorithmToFrameCryptorAlgorithm(algorithm),
      key_provider->rtc_key_provider(), sender->rtc_sender());
}

std::shared_ptr<FrameCryptor> new_frame_cryptor_for_rtp_receiver(
    std::shared_ptr<PeerConnectionFactory> peer_factory,
    const ::rust::String participant_id,
    Algorithm algorithm,
    std::shared_ptr<KeyProvider> key_provider,
    std::shared_ptr<RtpReceiver> receiver) {
  return std::make_shared<FrameCryptor>(
      peer_factory->rtc_runtime(),
      std::string(participant_id.data(), participant_id.size()),
      AlgorithmToFrameCryptorAlgorithm(algorithm),
      key_provider->rtc_key_provider(), receiver->rtc_receiver());
}

}  // namespace livekit
