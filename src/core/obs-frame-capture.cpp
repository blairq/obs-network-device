#include "obs-frame-capture.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs.h>

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <cstring>

namespace netdev {

namespace {

constexpr uint32_t kMaxCaptureWidth = 1280;

struct CaptureTaskContext {
    obs_weak_source_t *weakSource = nullptr;
    QPointer<QObject> receiver;
    std::function<void(const QImage &)> callback;
};

void deliverImage(CaptureTaskContext *ctx, const QImage &image)
{
    if (!ctx) {
        return;
    }
    QPointer<QObject> receiver = ctx->receiver;
    auto cb = std::move(ctx->callback);
    if (ctx->weakSource) {
        obs_weak_source_release(ctx->weakSource);
        ctx->weakSource = nullptr;
    }
    delete ctx;

    if (!receiver || !cb) {
        return;
    }

    QMetaObject::invokeMethod(
        receiver.data(),
        [receiver, cb = std::move(cb), image]() {
            if (receiver) {
                cb(image);
            }
        },
        Qt::QueuedConnection);
}

void graphicsCaptureTask(void *param)
{
    auto *ctx = static_cast<CaptureTaskContext *>(param);
    if (!ctx || !ctx->weakSource || !ctx->receiver) {
        deliverImage(ctx, QImage());
        return;
    }

    obs_source_t *src = obs_weak_source_get_source(ctx->weakSource);
    if (!src) {
        deliverImage(ctx, QImage());
        return;
    }

    QImage result;
    obs_enter_graphics();

    const uint32_t srcW = obs_source_get_width(src);
    const uint32_t srcH = obs_source_get_height(src);
    if (srcW > 0 && srcH > 0) {
        uint32_t dstW = srcW;
        uint32_t dstH = srcH;
        if (dstW > kMaxCaptureWidth) {
            dstH = std::max<uint32_t>(1, (dstH * kMaxCaptureWidth) / dstW);
            dstW = kMaxCaptureWidth;
        }

        gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        gs_stagesurf_t *stagesurf = gs_stagesurface_create(dstW, dstH, GS_RGBA);

        if (texrender && stagesurf) {
            if (gs_texrender_begin_with_color_space(texrender, dstW, dstH, GS_CS_SRGB)) {
                struct vec4 zero {};
                vec4_zero(&zero);
                gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
                gs_ortho(0.0f, static_cast<float>(srcW), 0.0f, static_cast<float>(srcH),
                         -100.0f, 100.0f);

                gs_blend_state_push();
                gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

                obs_source_inc_showing(src);
                obs_source_video_render(src);
                obs_source_dec_showing(src);

                gs_blend_state_pop();
                gs_texrender_end(texrender);

                gs_texture_t *tex = gs_texrender_get_texture(texrender);
                if (tex) {
                    gs_stage_texture(stagesurf, tex);

                    uint8_t *videoData = nullptr;
                    uint32_t videoLinesize = 0;
                    if (gs_stagesurface_map(stagesurf, &videoData, &videoLinesize)) {
                        QImage captured(static_cast<int>(dstW), static_cast<int>(dstH),
                                        QImage::Format_RGBX8888);
                        if (!captured.isNull()) {
                            const int copyBytes = std::min<int>(
                                captured.bytesPerLine(), static_cast<int>(videoLinesize));
                            for (uint32_t y = 0; y < dstH; ++y) {
                                std::memcpy(captured.scanLine(static_cast<int>(y)),
                                            videoData + static_cast<size_t>(y) * videoLinesize,
                                            static_cast<size_t>(copyBytes));
                            }
                            result = std::move(captured);
                        }
                        gs_stagesurface_unmap(stagesurf);
                    }
                }
            }
        }

        if (stagesurf) {
            gs_stagesurface_destroy(stagesurf);
        }
        if (texrender) {
            gs_texrender_destroy(texrender);
        }
    }

    obs_leave_graphics();
    obs_source_release(src);

    deliverImage(ctx, result);
}

} // namespace

void captureObsSourceFrameAsync(const QString &sourceUuid,
                                QObject *context,
                                std::function<void(const QImage &)> callback)
{
    if (!context || !callback) {
        return;
    }

    const QByteArray uuidUtf8 = sourceUuid.toUtf8();
    obs_source_t *src =
        uuidUtf8.isEmpty() ? nullptr : obs_get_source_by_uuid(uuidUtf8.constData());
    if (!src) {
        QPointer<QObject> guard(context);
        QMetaObject::invokeMethod(
            context,
            [guard, callback = std::move(callback)]() {
                if (guard) {
                    callback(QImage());
                }
            },
            Qt::QueuedConnection);
        return;
    }

    auto *ctx = new CaptureTaskContext();
    ctx->weakSource = obs_source_get_weak_source(src);
    ctx->receiver = context;
    ctx->callback = std::move(callback);
    obs_source_release(src);

    obs_queue_task(OBS_TASK_GRAPHICS, graphicsCaptureTask, ctx, false);
}

} // namespace netdev
