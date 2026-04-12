// app_context.cpp
// AppContext implementation: three-phase lifecycle management.
#include "app_context.h"

#include <spdlog/spdlog.h>

#include "bitrate_adapter.h"
#include "config_manager.h"
#include "credential_provider.h"
#include "kvs_sink_factory.h"
#include "pipeline_builder.h"
#include "pipeline_health.h"
#include "pipeline_manager.h"
#include "shutdown_handler.h"
#include "stream_mode_controller.h"
#include "s3_uploader.h"
#include "webrtc_media.h"
#include "webrtc_signaling.h"

#ifdef ENABLE_YOLO
#include "ai_pipeline_handler.h"
#include "yolo_detector.h"
#include <filesystem>
#endif

// ============================================================
// Impl definition
// ============================================================

struct AppContext::Impl {
    // Configs
    AwsConfig aws_config;
    KvsSinkFactory::KvsConfig kvs_config;
    WebRtcConfig webrtc_config;
    CameraSource::CameraConfig cam_config;
    StreamingConfig streaming_config;
    LoggingConfig logging_config;

    // Modules (declaration order determines destruction order)
    std::unique_ptr<WebRtcSignaling> signaling;
    std::unique_ptr<WebRtcMediaManager> media_manager;
    std::unique_ptr<PipelineManager> pipeline_manager;
    std::unique_ptr<PipelineHealthMonitor> health_monitor;
    std::unique_ptr<StreamModeController> stream_controller;
    std::unique_ptr<BitrateAdapter> bitrate_adapter;

#ifdef ENABLE_YOLO
    std::unique_ptr<AiPipelineHandler> ai_handler_;
#endif

    std::unique_ptr<S3Uploader> s3_uploader_;

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
                     const ConfigOverrides& overrides,
                     std::string* error_msg) {
    auto logger = spdlog::get("app");

    // --- Load all config via ConfigManager ---
    ConfigManager config;
    if (!config.load(config_path, error_msg)) {
        return false;
    }
    if (!config.apply_overrides(overrides, error_msg)) {
        return false;
    }

    // --- Store configs from ConfigManager ---
    impl_->aws_config = config.aws_config();
    impl_->kvs_config = config.kvs_config();
    impl_->webrtc_config = config.webrtc_config();
    impl_->cam_config = config.camera_config();
    impl_->streaming_config = config.streaming_config();
    impl_->logging_config = config.logging_config();

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

    // --- Create AiPipelineHandler (optional, ENABLE_YOLO only) ---
#ifdef ENABLE_YOLO
    const auto& ai_cfg = config.ai_config();
    if (!ai_cfg.model_path.empty() && std::filesystem::exists(ai_cfg.model_path)) {
        std::string det_err;
        auto detector = YoloDetector::create(ai_cfg.model_path, DetectorConfig{}, &det_err);
        if (detector) {
            std::string ai_err;
            impl_->ai_handler_ = AiPipelineHandler::create(std::move(detector), ai_cfg, &ai_err);
            if (!impl_->ai_handler_) {
                spdlog::warn("Failed to create AiPipelineHandler: {}", ai_err);
            }
        } else {
            spdlog::warn("Failed to create YoloDetector: {}", det_err);
        }
    } else {
        spdlog::info("AI pipeline skipped: model_path empty or file not found");
    }
#endif

    // --- Create S3Uploader (optional, requires S3 bucket config) ---
    const auto& s3_cfg = config.s3_config();
    if (!s3_cfg.bucket.empty()) {
        auto http_client = std::make_shared<CurlHttpClient>();
        std::string cred_err;
        auto cred_provider = CredentialProvider::create(config_path, http_client, &cred_err);
        if (cred_provider) {
            std::string s3_err;
            impl_->s3_uploader_ = S3Uploader::create(
                s3_cfg,
                config.ai_config().snapshot_dir,
                impl_->aws_config.thing_name,
                std::shared_ptr<CredentialProvider>(cred_provider.release()),
                &s3_err);
            if (!impl_->s3_uploader_) {
                spdlog::warn("Failed to create S3Uploader: {}", s3_err);
            }
        } else {
            spdlog::info("S3Uploader skipped: credentials not available ({})", cred_err);
        }
    } else {
        spdlog::info("S3Uploader skipped: S3 bucket not configured");
    }

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
    AiPipelineHandler* ai_ptr = nullptr;
#ifdef ENABLE_YOLO
    ai_ptr = impl_->ai_handler_.get();
#endif
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        error_msg,
        impl_->cam_config,
        &impl_->kvs_config,
        &impl_->aws_config,
        impl_->media_manager.get(),
        ai_ptr);
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

    // --- Start AI pipeline handler (after pipeline is running) ---
#ifdef ENABLE_YOLO
    if (impl_->ai_handler_) {
        std::string ai_start_err;
        if (!impl_->ai_handler_->start(&ai_start_err)) {
            spdlog::warn("Failed to start AiPipelineHandler: {}", ai_start_err);
        }
    }
#endif

    // --- Create HealthMonitor ---
    impl_->health_monitor = std::make_unique<PipelineHealthMonitor>(
        impl_->pipeline_manager->pipeline());

    // --- Create StreamModeController ---
    impl_->stream_controller = std::make_unique<StreamModeController>(
        impl_->pipeline_manager->pipeline(),
        impl_->streaming_config.debounce_sec * 1000);

    // --- Create BitrateAdapter ---
    impl_->bitrate_adapter = std::make_unique<BitrateAdapter>(
        impl_->pipeline_manager->pipeline(),
        to_bitrate_config(impl_->streaming_config));

    // --- Register mode change callback: notify BitrateAdapter ---
    impl_->stream_controller->set_mode_change_callback(
        [this](StreamMode old_mode, StreamMode new_mode,
               const std::string& /*reason*/) {
            impl_->bitrate_adapter->on_mode_changed(old_mode, new_mode);
        });

    // --- Register rebuild callback ---
    impl_->health_monitor->set_rebuild_callback([this]() -> GstElement* {
#ifdef ENABLE_YOLO
        if (impl_->ai_handler_) {
            impl_->ai_handler_->stop();
        }
#endif
        std::string err;
        AiPipelineHandler* ai_rebuild_ptr = nullptr;
#ifdef ENABLE_YOLO
        ai_rebuild_ptr = impl_->ai_handler_.get();
#endif
        GstElement* p = PipelineBuilder::build_tee_pipeline(
            &err,
            impl_->cam_config,
            &impl_->kvs_config,
            &impl_->aws_config,
            impl_->media_manager.get(),
            ai_rebuild_ptr);
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
        impl_->stream_controller->set_pipeline(
            impl_->pipeline_manager->pipeline());
        impl_->bitrate_adapter->set_pipeline(
            impl_->pipeline_manager->pipeline());
#ifdef ENABLE_YOLO
        if (impl_->ai_handler_) {
            impl_->ai_handler_->start();
        }
#endif
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

    // --- Start stream controller and bitrate adapter ---
    impl_->stream_controller->start();
    impl_->bitrate_adapter->start();

    // --- Start health monitoring ---
    impl_->health_monitor->start("src");

    // --- Register shutdown steps for pipeline and health monitor ---
    // Steps execute in reverse registration order, so register
    // stream_controller/bitrate_adapter after health_monitor to ensure
    // they stop before health_monitor.
    impl_->shutdown_handler.register_step("health_monitor", [this]() {
        impl_->health_monitor->stop();
    });
    impl_->shutdown_handler.register_step("stream_controller", [this]() {
        impl_->stream_controller->stop();
    });
    impl_->shutdown_handler.register_step("bitrate_adapter", [this]() {
        impl_->bitrate_adapter->stop();
    });
#ifdef ENABLE_YOLO
    if (impl_->ai_handler_) {
        impl_->shutdown_handler.register_step("ai_handler", [this]() {
            impl_->ai_handler_->stop();
        });
    }
#endif
    if (impl_->s3_uploader_) {
        impl_->shutdown_handler.register_step("s3_uploader", [this]() {
            impl_->s3_uploader_->stop();
        });
    }
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

    // --- Start S3Uploader (independent of GStreamer pipeline) ---
    if (impl_->s3_uploader_) {
        std::string s3_start_err;
        if (!impl_->s3_uploader_->start(&s3_start_err)) {
            if (logger) {
                logger->warn("Failed to start S3Uploader: {}", s3_start_err);
            }
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

ShutdownSummary AppContext::stop() {
    return impl_->shutdown_handler.execute();
}
