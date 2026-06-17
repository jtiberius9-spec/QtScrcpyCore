#include <QDebug>

extern "C"
{
#include "libavutil/hwcontext.h"
#include "libavutil/imgutils.h"
}

#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

Decoder::Decoder(std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> onFrame, QObject *parent)
    : QObject(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder() {
    // defensive: close() is the canonical cleanup, but free hw resources here
    // too in case the decoder is destroyed without a prior close(). All of
    // these are null-guarded / idempotent.
    m_convert.deInit();
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    if (m_yuvFrame) {
        av_frame_free(&m_yuvFrame);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_vb->deInit();
    delete m_vb;
}

void Decoder::setCodec(quint32 codecId)
{
    m_codecId = codecId;
}

enum AVPixelFormat Decoder::getHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    // ctx->opaque points at the owning Decoder; m_hwPixFmt holds the surface
    // format we set up in open() (AV_PIX_FMT_D3D11 or AV_PIX_FMT_DXVA2_VLD).
    Decoder *self = reinterpret_cast<Decoder *>(ctx->opaque);
    AVPixelFormat want = self ? self->m_hwPixFmt : AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat *p = pixFmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == want) {
            return *p;
        }
    }
    // hw surface format not offered: fall back to the first (software) format
    qWarning("decoder: requested hw pixfmt not available, using software format");
    return pixFmts ? pixFmts[0] : AV_PIX_FMT_NONE;
}

bool Decoder::open()
{
    // pick the decoder by the codec the server negotiated (default H264).
    // "h264"=0x68323634, "h265"=0x68323635, "av1"=0x00617631.
    AVCodecID avCodecId = AV_CODEC_ID_H264;
    if (m_codecId == 0x68323635) {
        avCodecId = AV_CODEC_ID_HEVC;
    } else if (m_codecId == 0x00617631) {
        avCodecId = AV_CODEC_ID_AV1;
    }

    // codec
    const AVCodec* codec = avcodec_find_decoder(avCodecId);
    if (!codec) {
        qCritical("video decoder not found (codec id 0x%x)", m_codecId);
        return false;
    }

    // codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }

    // Try to enable hardware-accelerated decoding (D3D11VA preferred, DXVA2 as
    // fallback). On a weak Intel HD620 iGPU this offloads H.265/HEVC@1920 to
    // Quick Sync and removes the live-mirror latency the software path causes.
    // If neither is available we keep going in pure software mode.
    m_codecCtx->opaque = this;
    int hwRet = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (hwRet >= 0) {
        m_hwPixFmt = AV_PIX_FMT_D3D11;
        m_useHw = true;
    } else {
        hwRet = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
        if (hwRet >= 0) {
            m_hwPixFmt = AV_PIX_FMT_DXVA2_VLD;
            m_useHw = true;
        }
    }
    if (m_useHw) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecCtx->get_format = Decoder::getHwFormat;
    } else {
        qWarning("decoder: no hw device (d3d11va/dxva2), falling back to software");
    }

    // reduce latency for live mirroring
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open %s codec", codec->name);
        return false;
    }

    // scratch frames used only by the hardware path
    m_swFrame = av_frame_alloc();
    m_yuvFrame = av_frame_alloc();
    if (!m_swFrame || !m_yuvFrame) {
        qCritical("Could not allocate decoder scratch frames");
        return false;
    }

    qInfo("video decoder opened: %s", codec->name);
    qInfo("decoder: %s", m_useHw ? "hardware (d3d11va)" : "software");
    m_isCodecCtxOpen = true;
    return true;
}

void Decoder::close()
{
    if (m_vb) {
        m_vb->interrupt();
    }

    // tear down hardware-decode resources (guarded for null / software path)
    m_convert.deInit();
    m_convertInit = false;
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    if (m_yuvFrame) {
        av_frame_free(&m_yuvFrame);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_useHw = false;

    if (!m_codecCtx) {
        return;
    }
    if (m_isCodecCtxOpen) {
        avcodec_close(m_codecCtx);
    }
    avcodec_free_context(&m_codecCtx);
}

bool Decoder::push(const AVPacket *packet)
{
    if (!m_codecCtx || !m_vb) {
        return false;
    }
    AVFrame *decodingFrame = m_vb->decodingFrame();
#ifdef QTSCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
    int ret = -1;
    if ((ret = avcodec_send_packet(m_codecCtx, packet)) < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical("Could not send video packet: %s", errorbuf);
        return false;
    }
    if (decodingFrame) {
        ret = avcodec_receive_frame(m_codecCtx, decodingFrame);
    }
    if (!ret) {
        // a frame was received.
        // If it is a GPU surface (D3D11 / DXVA2), download it to CPU memory
        // (NV12) and convert to YUV420P so the GL renderer keeps receiving the
        // exact same 3-plane YUV420P frame the software path produces.
        if (decodingFrame && (decodingFrame->format == AV_PIX_FMT_D3D11 || decodingFrame->format == AV_PIX_FMT_DXVA2_VLD)) {
            if (!processHwFrame(decodingFrame)) {
                return false;
            }
        }
        pushFrame();

        //emit getOneFrame(yuvDecoderFrame->data[0], yuvDecoderFrame->data[1], yuvDecoderFrame->data[2],
        //        yuvDecoderFrame->linesize[0], yuvDecoderFrame->linesize[1], yuvDecoderFrame->linesize[2]);

        /*
        // m_conver转换yuv为rgb是使用cpu转的，占用cpu太高，改用opengl渲染yuv
        // QImage的copy也非常占用内存，此方案不考虑
        if (!m_conver.isInit()) {
            qDebug() << "decoder frame format" << decodingFrame->format;
            m_conver.setSrcFrameInfo(codecCtx->width, codecCtx->height, AV_PIX_FMT_YUV420P);
            m_conver.setDstFrameInfo(codecCtx->width, codecCtx->height, AV_PIX_FMT_RGB32);
            m_conver.init();
        }
        if (!outBuffer) {
            outBuffer=new quint8[avpicture_get_size(AV_PIX_FMT_RGB32, codecCtx->width, codecCtx->height)];
            avpicture_fill((AVPicture *)rgbDecoderFrame, outBuffer, AV_PIX_FMT_RGB32, codecCtx->width, codecCtx->height);
        }
        m_conver.convert(decodingFrame, rgbDecoderFrame);
        //QImage tmpImg((uchar *)outBuffer, codecCtx->width, codecCtx->height, QImage::Format_RGB32);
        //QImage image = tmpImg.copy();
        //emit getOneImage(image);
        */
    } else if (ret != AVERROR(EAGAIN)) {
        qCritical("Could not receive video frame: %d", ret);
        return false;
    }
#else
    int gotPicture = 0;
    int len = -1;
    if (decodingFrame) {
        len = avcodec_decode_video2(m_codecCtx, decodingFrame, &gotPicture, packet);
    }
    if (len < 0) {
        qCritical("Could not decode video packet: %d", len);
        return false;
    }
    if (gotPicture) {
        pushFrame();
    }
#endif
    return true;
}

bool Decoder::processHwFrame(AVFrame *decodingFrame)
{
    // a. download the GPU surface to CPU memory (m_swFrame becomes NV12)
    av_frame_unref(m_swFrame);
    int ret = av_hwframe_transfer_data(m_swFrame, decodingFrame, 0);
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical("decoder: hw frame transfer failed: %s", errorbuf);
        return false;
    }
    av_frame_copy_props(m_swFrame, decodingFrame);

    const int w = m_swFrame->width;
    const int h = m_swFrame->height;

    // b. (re)init the NV12 -> YUV420P swscale context when dims/format change.
    //    The context is kept alive across frames (cheap path); only rebuilt on
    //    a resolution change. m_convSrcW/H/Fmt track what it was built for.
    if (!m_convertInit || m_convSrcW != w || m_convSrcH != h || m_convSrcFmt != m_swFrame->format) {
        m_convert.deInit();
        m_convert.setSrcFrameInfo(w, h, (AVPixelFormat)m_swFrame->format);
        m_convert.setDstFrameInfo(w, h, AV_PIX_FMT_YUV420P);
        if (!m_convert.init()) {
            qCritical("decoder: could not init NV12->YUV420P converter");
            return false;
        }
        m_convSrcW = w;
        m_convSrcH = h;
        m_convSrcFmt = m_swFrame->format;
        m_convertInit = true;
    }

    // (re)allocate the destination YUV420P buffers. m_yuvFrame is empty after a
    // move_ref on the previous frame, so it gets fresh buffers each iteration.
    if (!m_yuvFrame->data[0] || m_yuvFrame->width != w || m_yuvFrame->height != h) {
        av_frame_unref(m_yuvFrame);
        m_yuvFrame->format = AV_PIX_FMT_YUV420P;
        m_yuvFrame->width = w;
        m_yuvFrame->height = h;
        if ((ret = av_frame_get_buffer(m_yuvFrame, 32)) < 0) {
            qCritical("decoder: could not allocate YUV420P frame buffers: %d", ret);
            return false;
        }
    }

    // c. convert NV12 (CPU) -> YUV420P
    if (!m_convert.convert(m_swFrame, m_yuvFrame)) {
        qCritical("decoder: NV12->YUV420P conversion failed");
        return false;
    }
    av_frame_copy_props(m_yuvFrame, m_swFrame);

    // d. hand the standalone YUV420P frame back through decodingFrame so the
    //    VideoBuffer/renderer see exactly what the software path produces.
    av_frame_unref(decodingFrame);
    av_frame_move_ref(decodingFrame, m_yuvFrame);
    return true;
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (!m_vb) {
        return;
    }
    m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) {
        return;
    }
    bool previousFrameSkipped = true;
    m_vb->offerDecodedFrame(previousFrameSkipped);
    if (previousFrameSkipped) {
        // the previous newFrame will consume this frame
        return;
    }
    emit newFrame();
}

void Decoder::onNewFrame() {
    if (!m_onFrame) {
        return;
    }

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_onFrame(frame->width, frame->height, frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    m_vb->unLock();
}
