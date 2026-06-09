// app_context.cpp
// AppContext implementation: three-phase lifecycle management.
#include "app_context.h"

#include <spdlog/spdlog.h>

#include "bitrate_adapter.h"
#include "bandwidth_probe.h"
#include "config_manager.h"
#include "credential_provider.h"
#include "kvs_sink_factory.h"
#include "network_monitor.h"
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
    AiConfig ai_config;

    // Modules (declaration order determines destruction order)
    std::unique_ptr<WebRtcSignaling> signaling;
    std::unique_ptr<WebRtcMediaManager> media_manager;
    std::unique_ptr<PipelineManager> pipeline_manager;
    std::unique_ptr<PipelineHealthMonitor> health_monitor;
    std::unique_ptr<StreamModeController> stream_controller;
    std::unique_ptr<BitrateAdapter> bitrate_adapter;
    std::unique_ptr<NetworkMonitor> network_monitor;
    std::unique_ptr<BandwidthProbe> bandwidth_probe;

#ifdef ENABLE_YOLO
    std::unique_ptr<AiPipelineHandler> ai_handler_;
#endif

    std::unique_ptr<S3Uploader> s3_uploader_;

    // rebuild 暂存新管道（成功后转正），FATAL 优雅退出回调
    std::unique_ptr<PipelineManager> pending_pm_;
    std::function<void()> shutdown_requester;

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
    impl_->ai_config = config.ai_config();

    // --- Create Signaling & MediaManager (skip when WebRTC disabled) ---
    if (impl_->webrtc_config.enabled) {
        impl_->signaling = WebRtcSignaling::create(
            impl_->webrtc_config, impl_->aws_config, error_msg);
        if (!impl_->signaling) {
            return false;
        }

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
    } else {
        if (logger) {
            logger->info("WebRTC disabled, skipping signaling and media_manager");
        }
    }

    // --- Create AiPipelineHandler (optional, ENABLE_YOLO only) ---
#ifdef ENABLE_YOLO
    const auto& ai_cfg = config.ai_config();
    if (ai_cfg.enabled && !ai_cfg.model_path.empty() && std::filesystem::exists(ai_cfg.model_path)) {
        std::string det_err;
        auto detector = YoloDetector::create(ai_cfg.model_path,
            DetectorConfig{
                .confidence_threshold = ai_cfg.confidence_threshold,
                .num_threads = ai_cfg.num_threads,
                .use_xnnpack = ai_cfg.use_xnnpack,
            }, &det_err);
        if (detector) {
            std::string ai_err;
            impl_->ai_handler_ = AiPipelineHandler::create(std::move(detector), ai_cfg, &ai_err);
            if (!impl_->ai_handler_) {
                if (logger) logger->warn("Failed to create AiPipelineHandler: {}", ai_err);
            } else {
                if (logger) logger->info("AiPipelineHandler created: model={}", ai_cfg.model_path);
            }
        } else {
            if (logger) logger->warn("Failed to create YoloDetector: {}", det_err);
        }
    } else {
        if (logger) logger->info("AI pipeline skipped: enabled={}, model_path='{}', exists={}",
                                  ai_cfg.enabled, ai_cfg.model_path,
                                  !ai_cfg.model_path.empty() && std::filesystem::exists(ai_cfg.model_path));
    }
#endif

    // --- Create S3Uploader (optional, requires S3 bucket config) ---
    // Note: S3Uploader is created before the callback connection below.
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

    // --- Connect event close callback: AiPipelineHandler -> S3Uploader ---
#ifdef ENABLE_YOLO
    if (impl_->ai_handler_ && impl_->s3_uploader_) {
        impl_->ai_handler_->set_event_close_callback([this]() {
            impl_->s3_uploader_->notify_upload();
        });
        if (logger) logger->info("Event close callback connected: AiPipelineHandler -> S3Uploader");
    }
#endif

    // --- Info log (only resource identifiers, no secrets) ---
    if (logger) {
        logger->info("AppContext init ok: kvs_stream={} (enabled={}), webrtc_channel={} (enabled={})",
                     impl_->kvs_config.stream_name,
                     impl_->kvs_config.enabled,
                     impl_->webrtc_config.channel_name,
                     impl_->webrtc_config.enabled);
    }

    return true;
}

// ============================================================
// start()
// ============================================================

bool AppContext::start(std::string* error_msg) {
    auto logger = spdlog::get("app");

    // --- Build tee pipeline ---
    // KVS: enabled=false 时传 nullptr（PipelineBuilder 用 fakesink）
    const KvsSinkFactory::KvsConfig* kvs_ptr =
        impl_->kvs_config.enabled ? &impl_->kvs_config : nullptr;
    const AwsConfig* aws_ptr =
        impl_->kvs_config.enabled ? &impl_->aws_config : nullptr;
    // WebRTC: init 阶段已决定，直接用 media_manager 指针（可能为 nullptr）
    WebRtcMediaManager* webrtc_ptr = impl_->media_manager.get();

    // 计算 KvsSinkConfig（在 pipeline 构建前准备好）
    auto bitrate_config = to_bitrate_config(impl_->streaming_config);
    auto kvs_sink_config = to_kvssink_config(impl_->streaming_config, bitrate_config);
    const KvsSinkConfig* kvs_sink_config_ptr =
        impl_->kvs_config.enabled ? &kvs_sink_config : nullptr;

    AiPipelineHandler* ai_ptr = nullptr;
#ifdef ENABLE_YOLO
    ai_ptr = impl_->ai_handler_.get();
#endif
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        error_msg,
        impl_->cam_config,
        kvs_ptr,
        aws_ptr,
        webrtc_ptr,
        ai_ptr,
        kvs_sink_config_ptr);
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
        bitrate_config);

    // --- Register mode change callback: notify BitrateAdapter ---
    impl_->stream_controller->set_mode_change_callback(
        [this](StreamMode old_mode, StreamMode new_mode,
               const std::string& /*reason*/) {
            impl_->bitrate_adapter->on_mode_changed(old_mode, new_mode);
        });

    // --- Create NetworkMonitor ---
    NetworkConfig net_config;
    net_config.latency_pressure_threshold = impl_->streaming_config.latency_pressure_threshold;
    net_config.latency_pressure_cooldown_sec = impl_->streaming_config.latency_pressure_cooldown_sec;
    net_config.writeframe_fail_threshold = impl_->streaming_config.writeframe_fail_threshold;
    net_config.writeframe_recovery_count = 50;  // 固定值
    impl_->network_monitor = std::make_unique<NetworkMonitor>(net_config);
    impl_->network_monitor->set_bitrate_adapter(impl_->bitrate_adapter.get());
    impl_->network_monitor->set_stream_mode_controller(impl_->stream_controller.get());

    // --- Create BandwidthProbe ---
    BandwidthProbe::ProbeConfig probe_config;
    probe_config.enabled = impl_->streaming_config.bandwidth_probe_enabled;
    probe_config.duration_sec = impl_->streaming_config.bandwidth_probe_duration_sec;
    impl_->bandwidth_probe = std::make_unique<BandwidthProbe>(probe_config);
    impl_->bandwidth_probe->start_probe(
        impl_->pipeline_manager->pipeline(),
        impl_->bitrate_adapter.get(),
        bitrate_config);

    // --- Set pipeline reference and writeframe threshold on WebRtcMediaManager ---
    if (impl_->media_manager) {
        impl_->media_manager->set_pipeline(impl_->pipeline_manager->pipeline());
        impl_->media_manager->set_writeframe_fail_threshold(
            impl_->streaming_config.writeframe_fail_threshold);
    }

    // --- Register rebuild callback (Spec 32 决策 B/C：detach -> 有界异步 teardown -> 带重试构建) ---
    impl_->health_monitor->set_rebuild_callback([this]() -> GstElement* {
        auto lg = spdlog::get("app");
#ifdef ENABLE_YOLO
        if (impl_->ai_handler_) {
            impl_->ai_handler_->stop();
        }
#endif
        // (1) 决策 C：先让 health monitor 与旧管道解绑（移除 probe + bus watch），
        //     避免在销毁旧管道期间出现悬空引用。
        impl_->health_monitor->detach();

        // (2) 决策 B：有界异步销毁旧管道。release() 转移所有权给 worker 线程，
        //     主循环最多等 state_reset_timeout_ms；超时则后台完成 NULL+unref。
        if (impl_->pipeline_manager) {
            GstElement* old = impl_->pipeline_manager->release();
            impl_->pipeline_manager.reset();  // 壳已空，安全析构
            teardown_pipeline_bounded(old, /*budget*/5000);
        }

        // rebuild 时根据 enabled 决定传递 nullptr
        const KvsSinkFactory::KvsConfig* kvs_ptr =
            impl_->kvs_config.enabled ? &impl_->kvs_config : nullptr;
        const AwsConfig* aws_ptr =
            impl_->kvs_config.enabled ? &impl_->aws_config : nullptr;
        WebRtcMediaManager* webrtc_ptr = impl_->media_manager.get();

        // rebuild 时重新计算 KvsSinkConfig
        auto rebuild_bitrate_config = to_bitrate_config(impl_->streaming_config);
        auto rebuild_kvs_sink_config = to_kvssink_config(impl_->streaming_config, rebuild_bitrate_config);
        const KvsSinkConfig* rebuild_kvs_sink_config_ptr =
            impl_->kvs_config.enabled ? &rebuild_kvs_sink_config : nullptr;

        AiPipelineHandler* ai_rebuild_ptr = nullptr;
#ifdef ENABLE_YOLO
        ai_rebuild_ptr = impl_->ai_handler_.get();
#endif
        // (3) 带重试构建+启动新管道（覆盖旧 v4l2 fd 释放窗口，Spec 32 需求 2）。
        //     重试粒度是整次 build + start。
        CameraSource::OpenRetryConfig retry_cfg;  // 默认 500ms × 6
        int attempts = 0;
        bool ok = CameraSource::open_with_retry([&]() -> bool {
            std::string err;
            GstElement* p = PipelineBuilder::build_tee_pipeline(
                &err,
                impl_->cam_config,
                kvs_ptr,
                aws_ptr,
                webrtc_ptr,
                ai_rebuild_ptr,
                rebuild_kvs_sink_config_ptr);
            if (!p) {
                if (lg) lg->warn("rebuild: build_tee_pipeline failed: {}", err);
                return false;
            }
            auto new_pm = PipelineManager::create(p, &err);
            if (!new_pm) {
                gst_object_unref(p);
                return false;
            }
            if (!new_pm->start(&err)) {
                if (lg) lg->warn("rebuild: pipeline start failed: {}", err);
                return false;  // new_pm 析构会 stop+unref 半启动的管道
            }
            impl_->pending_pm_ = std::move(new_pm);  // 暂存，成功后转正
            return true;
        }, retry_cfg, {}, &attempts);

        if (!ok) {
            if (lg) lg->error("rebuild failed after {} attempt(s)", attempts);
            return nullptr;  // 恢复失败 -> 计入 recovery 失败 -> 可能 FATAL
        }
        if (lg) lg->info("rebuild succeeded after {} attempt(s)", attempts);

        impl_->pipeline_manager = std::move(impl_->pending_pm_);

        // 更新各模块的 pipeline 引用
        impl_->stream_controller->set_pipeline(
            impl_->pipeline_manager->pipeline());
        impl_->bitrate_adapter->set_pipeline(
            impl_->pipeline_manager->pipeline());
        if (impl_->media_manager) {
            impl_->media_manager->set_pipeline(
                impl_->pipeline_manager->pipeline());
        }
#ifdef ENABLE_YOLO
        if (impl_->ai_handler_) {
            impl_->ai_handler_->start();
        }
#endif
        // health_monitor 在 try_full_rebuild 返回后 set_pipeline 重新 attach
        return impl_->pipeline_manager->pipeline();
    });

    // --- Register health callback (log + FATAL -> request graceful shutdown) ---
    impl_->health_monitor->set_health_callback(
        [this](HealthState old_s, HealthState new_s) {
            auto lg = spdlog::get("app");
            if (lg) {
                lg->info("Health state: {} -> {}",
                         health_state_name(old_s),
                         health_state_name(new_s));
            }
            if (new_s == HealthState::FATAL && impl_->shutdown_requester) {
                if (lg) {
                    lg->error("Pipeline FATAL -- requesting graceful shutdown for systemd restart");
                }
                impl_->shutdown_requester();  // 仅设 flag，由 main 的 timer 退主循环
            }
        });

    // --- Start stream controller and bitrate adapter ---
    impl_->stream_controller->start();
    impl_->bitrate_adapter->start();

    // --- Start network monitor ---
    impl_->network_monitor->start();

    // --- Start health monitoring ---
    impl_->health_monitor->start("src");

    // --- Register shutdown steps for pipeline and health monitor ---
    // Steps execute in reverse registration order, so register
    // stream_controller/bitrate_adapter after health_monitor to ensure
    // they stop before health_monitor.
    impl_->shutdown_handler.register_step("health_monitor", [this]() {
        impl_->health_monitor->stop();
    });
    impl_->shutdown_handler.register_step("network_monitor", [this]() {
        impl_->network_monitor->stop();
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
        if (impl_->pipeline_manager) {
            impl_->pipeline_manager->stop();
            impl_->pipeline_manager.reset();
        }
    });

    // --- Connect signaling (skip when WebRTC disabled, warn on failure) ---
    if (impl_->signaling) {
        std::string connect_err;
        if (!impl_->signaling->connect(&connect_err)) {
            if (logger) {
                logger->warn("Signaling connect failed (will retry): {}",
                             connect_err);
            }
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

// ============================================================
// set_shutdown_requester / is_healthy
// ============================================================

void AppContext::set_shutdown_requester(std::function<void()> fn) {
    impl_->shutdown_requester = std::move(fn);
}

bool AppContext::is_healthy() const {
    if (!impl_->health_monitor) return true;
    return impl_->health_monitor->state() != HealthState::FATAL;
}
