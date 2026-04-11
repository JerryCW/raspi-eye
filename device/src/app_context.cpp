// app_context.cpp
// AppContext implementation: three-phase lifecycle management.
#include "app_context.h"

#include <spdlog/spdlog.h>
#include <unordered_map>

#include "credential_provider.h"
#include "kvs_sink_factory.h"
#include "pipeline_builder.h"
#include "pipeline_health.h"
#include "pipeline_manager.h"
#include "shutdown_handler.h"
#include "webrtc_media.h"
#include "webrtc_signaling.h"

// ============================================================
// Impl definition
// ============================================================

struct AppContext::Impl {
    // Configs
    AwsConfig aws_config;
    KvsSinkFactory::KvsConfig kvs_config;
    WebRtcConfig webrtc_config;
    CameraSource::CameraConfig cam_config;

    // Modules (declaration order determines destruction order)
    std::unique_ptr<WebRtcSignaling> signaling;
    std::unique_ptr<WebRtcMediaManager> media_manager;
    std::unique_ptr<PipelineManager> pipeline_manager;
    std::unique_ptr<PipelineHealthMonitor> health_monitor;

    // Cleanup manager
    ShutdownHandler shutdown_handler;
};

// ============================================================
// Constructor / Destructor
// ============================================================

AppContext::AppContext() : impl_(std::make_unique<Impl>()) {}
AppContext::~AppContext() = default;

// ============================================================
// init()
// ============================================================

bool AppContext::init(const std::string& config_path,
                     const CameraSource::CameraConfig& cam_config,
                     std::string* error_msg) {
    auto logger = spdlog::get("app");

    impl_->cam_config = cam_config;

    // --- Parse [aws] section ---
    std::unordered_map<std::string, std::string> kv;
    kv = parse_toml_section(config_path, "aws", error_msg);
    if (kv.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!build_aws_config(kv, impl_->aws_config, error_msg)) {
        return false;
    }

    // --- Parse [kvs] section ---
    kv = parse_toml_section(config_path, "kvs", error_msg);
    if (kv.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!KvsSinkFactory::build_kvs_config(kv, impl_->kvs_config, error_msg)) {
        return false;
    }

    // --- Parse [webrtc] section ---
    kv = parse_toml_section(config_path, "webrtc", error_msg);
    if (kv.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!build_webrtc_config(kv, impl_->webrtc_config, error_msg)) {
        return false;
    }

    // --- Create Signaling ---
    impl_->signaling = WebRtcSignaling::create(
        impl_->webrtc_config, impl_->aws_config, error_msg);
    if (!impl_->signaling) {
        return false;
    }

    // --- Create MediaManager ---
    impl_->media_manager = WebRtcMediaManager::create(
        *impl_->signaling, impl_->webrtc_config.aws_region, error_msg);
    if (!impl_->media_manager) {
        return false;
    }

    // --- Register callbacks ---
    impl_->signaling->set_offer_callback(
        [this](const std::string& peer_id, const std::string& sdp) {
            impl_->media_manager->on_viewer_offer(peer_id, sdp);
        });

    impl_->signaling->set_ice_candidate_callback(
        [this](const std::string& peer_id, const std::string& candidate) {
            impl_->media_manager->on_viewer_ice_candidate(peer_id, candidate);
        });

    // --- Register shutdown steps (registered first = executed last) ---
    impl_->shutdown_handler.register_step("media_manager", [this]() {
        impl_->media_manager.reset();
    });
    impl_->shutdown_handler.register_step("signaling", [this]() {
        impl_->signaling->disconnect();
    });

    // --- Info log (only resource identifiers, no secrets) ---
    if (logger) {
        logger->info("AppContext init ok: kvs_stream={}, webrtc_channel={}",
                     impl_->kvs_config.stream_name,
                     impl_->webrtc_config.channel_name);
    }

    return true;
}

// ============================================================
// start()
// ============================================================

bool AppContext::start(std::string* error_msg) {
    auto logger = spdlog::get("app");

    // --- Build tee pipeline ---
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        error_msg,
        impl_->cam_config,
        &impl_->kvs_config,
        &impl_->aws_config,
        impl_->media_manager.get());
    if (!pipeline) {
        return false;
    }

    // --- Create PipelineManager ---
    impl_->pipeline_manager = PipelineManager::create(pipeline, error_msg);
    if (!impl_->pipeline_manager) {
        return false;
    }

    // --- Start pipeline ---
    if (!impl_->pipeline_manager->start(error_msg)) {
        return false;
    }

    // --- Create HealthMonitor ---
    impl_->health_monitor = std::make_unique<PipelineHealthMonitor>(
        impl_->pipeline_manager->pipeline());

    // --- Register rebuild callback ---
    impl_->health_monitor->set_rebuild_callback([this]() -> GstElement* {
        std::string err;
        GstElement* p = PipelineBuilder::build_tee_pipeline(
            &err,
            impl_->cam_config,
            &impl_->kvs_config,
            &impl_->aws_config,
            impl_->media_manager.get());
        if (!p) return nullptr;

        auto new_pm = PipelineManager::create(p, &err);
        if (!new_pm) {
            gst_object_unref(p);
            return nullptr;
        }
        if (!new_pm->start(&err)) {
            return nullptr;
        }
        impl_->pipeline_manager = std::move(new_pm);
        return impl_->pipeline_manager->pipeline();
    });

    // --- Register health callback (log state changes, no FATAL exit here) ---
    impl_->health_monitor->set_health_callback(
        [](HealthState old_s, HealthState new_s) {
            auto lg = spdlog::get("app");
            if (lg) {
                lg->info("Health state: {} -> {}",
                         health_state_name(old_s),
                         health_state_name(new_s));
            }
        });

    // --- Start health monitoring ---
    impl_->health_monitor->start("src");

    // --- Register shutdown steps for pipeline and health monitor ---
    impl_->shutdown_handler.register_step("health_monitor", [this]() {
        impl_->health_monitor->stop();
    });
    impl_->shutdown_handler.register_step("pipeline", [this]() {
        impl_->pipeline_manager->stop();
        impl_->pipeline_manager.reset();
    });

    // --- Connect signaling (warn on failure, do not block startup) ---
    std::string connect_err;
    if (!impl_->signaling->connect(&connect_err)) {
        if (logger) {
            logger->warn("Signaling connect failed (will retry): {}",
                         connect_err);
        }
    }

    // --- Info log ---
    if (logger) {
        logger->info("AppContext started: pipeline and modules running");
    }

    return true;
}

// ============================================================
// stop()
// ============================================================

void AppContext::stop() {
    impl_->shutdown_handler.execute();
}
