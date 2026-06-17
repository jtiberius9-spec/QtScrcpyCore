#ifndef DECODER_H
#define DECODER_H
#include <QObject>

extern "C"
{
#include "libavcodec/avcodec.h"
}

#include <functional>

#include "avframeconvert.h"

class VideoBuffer;
class Decoder : public QObject
{
    Q_OBJECT
public:
    Decoder(std::function<void(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)> onFrame, QObject *parent = Q_NULLPTR);
    virtual ~Decoder();

    void setCodec(quint32 codecId);   // scrcpy 视频编码 fourcc: h264/h265/av1
    bool open();
    void close();
    bool push(const AVPacket *packet);
    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

private:
    // get_format callback: pick the hardware surface format when available
    static enum AVPixelFormat getHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts);

signals:
    void updateFPS(quint32 fps);

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    void pushFrame();
    // download a GPU surface to CPU and convert it to YUV420P in-place in
    // decodingFrame. Returns false on error.
    bool processHwFrame(AVFrame *decodingFrame);

private:
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    quint32 m_codecId = 0;   // scrcpy 视频编码 fourcc; 0 -> 默认 H264
    bool m_isCodecCtxOpen = false;
    std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> m_onFrame = Q_NULLPTR;

    // hardware-accelerated decode (D3D11VA / DXVA2, falls back to software)
    AVBufferRef *m_hwDeviceCtx = nullptr;   // owned hw device context
    AVFrame *m_swFrame = nullptr;           // CPU NV12 after GPU transfer
    AVFrame *m_yuvFrame = nullptr;          // YUV420P after swscale convert
    AVFrameConvert m_convert;               // NV12 -> YUV420P
    bool m_convertInit = false;             // m_convert swscale ctx built
    int m_convSrcW = 0;                     // dims/fmt the swscale ctx was built for
    int m_convSrcH = 0;
    int m_convSrcFmt = AV_PIX_FMT_NONE;
    bool m_useHw = false;                   // hw decode active
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE; // expected GPU surface format
};

#endif // DECODER_H
