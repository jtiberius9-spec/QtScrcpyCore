#ifndef DEVICE_H
#define DEVICE_H

#include <set>
#include <QElapsedTimer>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTime>

#include "../../include/QtScrcpyCore.h"

extern "C"
{
#include "libavcodec/avcodec.h"
}

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class Recorder;
class Server;
class VideoBuffer;
class Decoder;
class FileHandler;
class Demuxer;
class VideoForm;
class Controller;
struct AVFrame;

namespace qsc {

class Device : public IDevice
{
    Q_OBJECT
public:
    explicit Device(DeviceParams params, QObject *parent = nullptr);
    virtual ~Device();

    void setUserData(void* data) override;
    void* getUserData() override;

    void registerDeviceObserver(DeviceObserver* observer) override;
    void deRegisterDeviceObserver(DeviceObserver* observer) override;

    bool connectDevice() override;
    void disconnectDevice() override;

    // key map
    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) override;

    void postGoBack() override;
    void postGoHome() override;
    void postGoMenu() override;
    void postAppSwitch() override;
    void postPower() override;
    void postVolumeUp() override;
    void postVolumeDown() override;
    void postCopy() override;
    void postCut() override;
    void setDisplayPower(bool on) override;
    void expandNotificationPanel() override;
    void collapsePanel() override;
    void postBackOrScreenOn(bool down) override;
    void postTextInput(QString &text) override;
    void requestDeviceClipboard() override;
    void setDeviceClipboard(bool pause = true) override;
    void clipboardPaste() override;
    void pushFileRequest(const QString &file, const QString &devicePath = "") override;
    void installApkRequest(const QString &apkFile) override;

    void screenshot() override;
    void showTouch(bool show) override;

    bool isReversePort(quint16 port) override;
    const QString &getSerial() override;

    void updateScript(QString script) override;
    bool isCurrentCustomKeymap() override;

    // mid-session recording (F12 toggle)
    bool startRecord(const QString &filePath, const QString &format) override;
    void stopRecord() override;
    bool isRecording() override;
    qint64 getRecordAudioSkipMs() override;

private:
    void initSignals();
    bool saveFrame(int width, int height, uint8_t* dataRGB32);

private:
    // server relevant
    QPointer<Server> m_server;
    bool m_serverStartSuccess = false;
    QPointer<Decoder> m_decoder;
    QPointer<Controller> m_controller;
    QPointer<FileHandler> m_fileHandler;
    QPointer<Demuxer> m_stream;
    QPointer<Recorder> m_recorder;

    QElapsedTimer m_startTimeCount;
    DeviceParams m_params;
    std::set<DeviceObserver*> m_deviceObservers;
    void* m_userData = nullptr;

    // mid-session recording (F12 toggle):
    // cached at connect so the recorder can be built on the next config packet.
    QSize m_videoSize;                 // negotiated device video size
    quint32 m_videoCodecId = 0;        // negotiated codec fourcc (h264/h265/av1)
    QString m_recordFilePath;          // target output path for active recording
    QString m_recordFormat;            // "mp4"/"mkv"
    // Latest codec-config (SPS/PPS) packet, refreshed in getConfigFrame. scrcpy
    // sends config essentially once at stream start, so we cache it to seed the
    // file header when a mid-session F12 recording begins. Deep copy; freed on
    // teardown.
    AVPacket *m_cachedConfig = nullptr;
    // After startRecord() the recorder exists but must skip data packets until
    // the next keyframe so it starts on a clean GOP (header written from the
    // cached config first).
    bool m_recordWaitingKeyframe = false;
    // A/V sync alignment: wall-clock at startRecord() (audio tee/mic also start
    // here) vs. wall-clock when the recorder's first video frame is written (at
    // the next keyframe). Their difference is the leading-audio skip the GUI mux
    // applies. Not reset in stopRecord() so the GUI can query it while muxing.
    qint64 m_recordStartMs = 0;
    qint64 m_recordFirstFrameMs = 0;
};

}

#endif // DEVICE_H
