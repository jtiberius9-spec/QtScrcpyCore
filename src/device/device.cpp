#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>

#include "controller.h"
#include "devicemsg.h"
#include "decoder.h"
#include "device.h"
#include "filehandler.h"
#include "recorder.h"
#include "server.h"
#include "demuxer.h"

namespace qsc {

Device::Device(DeviceParams params, QObject *parent) : IDevice(parent), m_params(params)
{
    if (!params.display && !m_params.recordFile) {
        qCritical("not display must be recorded");
        return;
    }

    if (params.display) {
        m_decoder = new Decoder([this](int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV) {
            for (const auto& item : m_deviceObservers) {
                item->onFrame(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
            }
        }, this);
        m_fileHandler = new FileHandler(this);
        m_controller = new Controller([this](const QByteArray& buffer) -> qint64 {
            if (!m_server || !m_server->getControlSocket()) {
                return 0;
            }

            return m_server->getControlSocket()->write(buffer.data(), buffer.length());
        }, params.gameScript, this);
    }

    m_stream = new Demuxer(this);

    m_server = new Server(this);
    // Recording is now driven mid-session by the F12 toggle (startRecord/
    // stopRecord), NOT created at connect time. The "record screen" checkbox
    // no longer auto-creates a recorder; the GUI owns recording orchestration.
    initSignals();
}

Device::~Device()
{
    Device::disconnectDevice();
}

void Device::setUserData(void *data)
{
    m_userData = data;
}

void *Device::getUserData()
{
    return m_userData;
}

void Device::registerDeviceObserver(DeviceObserver *observer)
{
    m_deviceObservers.insert(observer);
}

void Device::deRegisterDeviceObserver(DeviceObserver *observer)
{
    m_deviceObservers.erase(observer);
}

const QString &Device::getSerial()
{
    return m_params.serial;
}

void Device::updateScript(QString script)
{
    if (m_controller) {
        m_controller->updateScript(script);
    }
}

void Device::screenshot()
{
    if (!m_decoder) {
        return;
    }

    // screenshot
    m_decoder->peekFrame([this](int width, int height, uint8_t* dataRGB32) {
       saveFrame(width, height, dataRGB32);
    });
}

void Device::showTouch(bool show)
{
    AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->setShowTouchesEnabled(getSerial(), show);

    qInfo() << getSerial() << " show touch " << (show ? "enable" : "disable");
}

bool Device::isReversePort(quint16 port)
{
    if (m_server && m_server->isReverse() && port == m_server->getParams().localPort) {
        return true;
    }

    return false;
}

void Device::initSignals()
{
    if (m_controller) {
        connect(m_controller, &Controller::grabCursor, this, [this](bool grab){
            for (const auto& item : m_deviceObservers) {
                item->grabCursor(grab);
            }
        });
        connect(m_controller, &Controller::recoilHint, this, [this](QString hint){
            for (const auto& item : m_deviceObservers) {
                item->recoilHint(hint);
            }
        });
    }
    if (m_fileHandler) {
        connect(m_fileHandler, &FileHandler::fileHandlerResult, this, [this](FileHandler::FILE_HANDLER_RESULT processResult, bool isApk) {
            QString tipsType = "";
            if (isApk) {
                tipsType = "install apk";
            } else {
                tipsType = "file transfer";
            }
            QString tips;
            if (FileHandler::FAR_IS_RUNNING == processResult) {
                tips = QString("wait current %1 to complete").arg(tipsType);
            }
            if (FileHandler::FAR_SUCCESS_EXEC == processResult) {
                tips = QString("%1 complete, save in %2").arg(tipsType).arg(m_params.pushFilePath);
            }
            if (FileHandler::FAR_ERROR_EXEC == processResult) {
                tips = QString("%1 failed").arg(tipsType);
            }
            qInfo() << tips;
        });
    }

    if (m_server) {
        connect(m_server, &Server::serverStarted, this, [this](bool success, const QString &deviceName, const QSize &size) {
            m_serverStartSuccess = success;
            emit deviceConnected(success, m_params.serial, deviceName, size);
            if (success) {
                double diff = m_startTimeCount.elapsed() / 1000.0;
                qInfo() << QString("server start finish in %1s").arg(diff).toStdString().c_str();

                // cache negotiated codec + frame size so a mid-session
                // startRecord() can build the recorder on the next config packet
                m_videoCodecId = m_server->getVideoCodecId();   // h264/h265/av1
                m_videoSize = size;

                // init decoder
                if (m_decoder) {
                    m_decoder->setCodec(m_server->getVideoCodecId());   // h264/h265/av1
                    m_decoder->open();
                }

                // init stream
                m_stream->installVideoSocket(m_server->removeVideoSocket());
                m_stream->setFrameSize(size);
                m_stream->setCodec(m_server->getVideoCodecId());   // h264/h265/av1
                m_stream->startDecode();

                // recv device msg
                connect(m_server->getControlSocket(), &QTcpSocket::readyRead, this, [this](){
                    if (!m_controller) {
                        return;
                    }

                    auto controlSocket = m_server->getControlSocket();
                    while (controlSocket->bytesAvailable()) {
                        QByteArray byteArray = controlSocket->peek(controlSocket->bytesAvailable());
                        DeviceMsg deviceMsg;
                        qint32 consume = deviceMsg.deserialize(byteArray);
                        if (0 >= consume) {
                            break;
                        }
                        controlSocket->read(consume);
                        m_controller->recvDeviceMsg(&deviceMsg);
                    }
                });

                // 显示界面时才自动息屏（m_params.display）
                if (m_params.closeScreen && m_params.display && m_controller) {
                    m_controller->setDisplayPower(false);
                }
            } else {
                m_server->stop();
            }
        });
        connect(m_server, &Server::serverStoped, this, [this]() {
            disconnectDevice();
            qDebug() << "server process stop";
        });
    }

    if (m_stream) {
        connect(m_stream, &Demuxer::onStreamStop, this, [this]() {
            disconnectDevice();
            qDebug() << "stream thread stop";
        });
        connect(m_stream, &Demuxer::getFrame, this, [this](AVPacket *packet) {
            if (m_decoder && !m_decoder->push(packet)) {
                qCritical("Could not send packet to decoder");
            }

            if (m_recorder) {
                if (m_recordWaitingKeyframe) {
                    // Recording was just started mid-session: wait for the next
                    // keyframe so the file begins on a clean GOP. Write the file
                    // header from the cached config packet, then start at this
                    // keyframe.
                    if (packet->flags & AV_PKT_FLAG_KEY) {
                        if (m_cachedConfig && !m_recorder->push(m_cachedConfig)) {
                            qCritical("Could not send cached config packet to recorder");
                        }
                        m_recordWaitingKeyframe = false;
                        // First video frame is being written now; the audio tee/
                        // mic started back at startRecord(). Stamp this moment so
                        // the GUI can trim the leading-audio gap for A/V sync.
                        m_recordFirstFrameMs = QDateTime::currentMSecsSinceEpoch();
                        if (!m_recorder->push(packet)) {
                            qCritical("Could not send packet to recorder");
                        }
                    }
                    // else: drop pre-keyframe P-frames (don't record a broken GOP)
                } else if (!m_recorder->push(packet)) {
                    qCritical("Could not send packet to recorder");
                }
            }
        }, Qt::DirectConnection);
        connect(m_stream, &Demuxer::getConfigFrame, this, [this](AVPacket *packet) {
            // scrcpy emits the codec-config (SPS/PPS) packet essentially once at
            // stream start (and on resolution change), NOT before every keyframe.
            // Cache a fresh deep copy so a mid-session F12 startRecord() can seed
            // the file header from it (the Recorder requires the first packet to
            // be a config packet). Runs on the demuxer thread; ownership is freed
            // on teardown in disconnectDevice().
            if (m_cachedConfig) {
                av_packet_free(&m_cachedConfig);
            }
            m_cachedConfig = av_packet_clone(packet);

            // If a recorder already exists and is past its keyframe wait, keep
            // feeding it config packets as before (e.g. resolution change).
            if (m_recorder && !m_recordWaitingKeyframe && !m_recorder->push(packet)) {
                qCritical("Could not send config packet to recorder");
            }
        }, Qt::DirectConnection);
    }

    if (m_decoder) {
        connect(m_decoder, &Decoder::updateFPS, this, [this](quint32 fps) {
            for (const auto& item : m_deviceObservers) {
                item->updateFPS(fps);
            }
        });
    }
}

bool Device::connectDevice()
{
    if (!m_server || m_serverStartSuccess) {
        return false;
    }

    // fix: macos cant recv finished signel, timer is ok
    QTimer::singleShot(0, this, [this]() {
        m_startTimeCount.start();
        // max size support 480p 720p 1080p 设备原生分辨率
        // support wireless connect, example:
        //m_server->start("192.168.0.174:5555", 27183, m_maxSize, m_bitRate, "");
        // only one devices, serial can be null
        // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
        Server::ServerParams params;
        params.serverLocalPath = m_params.serverLocalPath;
        params.serverRemotePath = m_params.serverRemotePath;
        params.serial = m_params.serial;
        params.localPort = m_params.localPort;
        params.maxSize = m_params.maxSize;
        params.bitRate = m_params.bitRate;
        params.maxFps = m_params.maxFps;
        params.useReverse = m_params.useReverse;
        params.captureOrientationLock = m_params.captureOrientationLock;
        params.captureOrientation = m_params.captureOrientation;
        params.stayAwake = m_params.stayAwake;
        params.serverVersion = m_params.serverVersion;
        params.logLevel = m_params.logLevel;
        // Force a short keyframe/config interval (~1s) so that when the user
        // hits F12 mid-session a config packet (and key frame) arrives quickly,
        // letting startRecord() align the recording to a clean file header.
        // Preserve any user-provided codec options; append ours.
        {
            QString codecOptions = m_params.codecOptions;
            if (!codecOptions.trimmed().isEmpty()) {
                codecOptions += ",";
            }
            codecOptions += "i-frame-interval:int=1";
            params.codecOptions = codecOptions;
        }
        params.codecName = m_params.codecName;
        params.videoCodec = m_params.videoCodec;
        params.scid = m_params.scid;

        params.crop = "";
        params.control = true;
        m_server->start(params);
    });

    return true;
}

void Device::disconnectDevice()
{
    if (!m_server) {
        return;
    }
    m_server->stop();
    m_server = Q_NULLPTR;

    if (m_stream) {
        m_stream->stopDecode();
    }

    // server must stop before decoder, because decoder block main thread
    if (m_decoder) {
        m_decoder->close();
    }

    // finalize any in-progress recording (safety: F12-stop normally does this).
    // The demuxer thread is already stopped (m_stream->stopDecode() waited above),
    // so no getFrame/getConfigFrame push can race with this teardown.
    m_recordWaitingKeyframe = false;
    if (m_recorder) {
        Recorder *recorder = m_recorder;
        m_recorder = Q_NULLPTR;
        if (recorder->isRunning()) {
            recorder->stopRecorder();
            recorder->wait();
        }
        recorder->close();
        delete recorder;
    }

    // free the cached codec-config packet (demuxer thread is stopped above, so
    // no getConfigFrame push can race this).
    if (m_cachedConfig) {
        av_packet_free(&m_cachedConfig);
    }

    if (m_serverStartSuccess) {
        emit deviceDisconnected(m_params.serial);
    }
    m_serverStartSuccess = false;
}

void Device::postGoBack()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoBack();

    for (const auto& item : m_deviceObservers) {
        item->postGoBack();
    }
}

void Device::postGoHome()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoHome();

    for (const auto& item : m_deviceObservers) {
        item->postGoHome();
    }
}

void Device::postGoMenu()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoMenu();

    for (const auto& item : m_deviceObservers) {
        item->postGoMenu();
    }
}

void Device::postAppSwitch()
{
    if (!m_controller) {
        return;
    }
    m_controller->postAppSwitch();

    for (const auto& item : m_deviceObservers) {
        item->postAppSwitch();
    }
}

void Device::postPower()
{
    if (!m_controller) {
        return;
    }
    m_controller->postPower();

    for (const auto& item : m_deviceObservers) {
        item->postPower();
    }
}

void Device::postVolumeUp()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeUp();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeUp();
    }
}

void Device::postVolumeDown()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeDown();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeDown();
    }
}

void Device::postCopy()
{
    if (!m_controller) {
        return;
    }
    m_controller->copy();

    for (const auto& item : m_deviceObservers) {
        item->postCopy();
    }
}

void Device::postCut()
{
    if (!m_controller) {
        return;
    }
    m_controller->cut();

    for (const auto& item : m_deviceObservers) {
        item->postCut();
    }
}

void Device::setDisplayPower(bool on)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDisplayPower(on);

    for (const auto& item : m_deviceObservers) {
        item->setDisplayPower(on);
    }
}

void Device::expandNotificationPanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->expandNotificationPanel();

    for (const auto& item : m_deviceObservers) {
        item->expandNotificationPanel();
    }
}

void Device::collapsePanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->collapsePanel();

    for (const auto& item : m_deviceObservers) {
        item->collapsePanel();
    }
}

void Device::postBackOrScreenOn(bool down)
{
    if (!m_controller) {
        return;
    }
    m_controller->postBackOrScreenOn(down);

    for (const auto& item : m_deviceObservers) {
        item->postBackOrScreenOn(down);
    }
}

void Device::postTextInput(QString &text)
{
    if (!m_controller) {
        return;
    }
    m_controller->postTextInput(text);

    for (const auto& item : m_deviceObservers) {
        item->postTextInput(text);
    }
}

void Device::requestDeviceClipboard()
{
    if (!m_controller) {
        return;
    }
    m_controller->requestDeviceClipboard();

    for (const auto& item : m_deviceObservers) {
        item->requestDeviceClipboard();
    }
}

void Device::setDeviceClipboard(bool pause)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDeviceClipboard(pause);

    for (const auto& item : m_deviceObservers) {
        item->setDeviceClipboard(pause);
    }
}

void Device::clipboardPaste()
{
    if (!m_controller) {
        return;
    }
    m_controller->clipboardPaste();

    for (const auto& item : m_deviceObservers) {
        item->clipboardPaste();
    }
}

void Device::pushFileRequest(const QString &file, const QString &devicePath)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onPushFileRequest(getSerial(), file, devicePath);

    for (const auto& item : m_deviceObservers) {
        item->pushFileRequest(file, devicePath);
    }
}

void Device::installApkRequest(const QString &apkFile)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onInstallApkRequest(getSerial(), apkFile);

    for (const auto& item : m_deviceObservers) {
        item->installApkRequest(apkFile);
    }
}

void Device::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->mouseEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->mouseEvent(from, frameSize, showSize);
    }
}

void Device::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->wheelEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->wheelEvent(from, frameSize, showSize);
    }
}

void Device::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->keyEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->keyEvent(from, frameSize, showSize);
    }
}

bool Device::isCurrentCustomKeymap()
{
    if (!m_controller) {
        return false;
    }
    return m_controller->isCurrentCustomKeymap();
}

void Device::setPCMode(bool on)
{
    if (!m_controller) {
        return;
    }
    m_controller->setPCMode(on);
}

bool Device::isPCMode()
{
    if (!m_controller) {
        return false;
    }
    return m_controller->isPCMode();
}

bool Device::startRecord(const QString &filePath, const QString &format)
{
    if (filePath.trimmed().isEmpty()) {
        qWarning() << "startRecord: empty file path";
        return false;
    }
    if (m_recorder) {
        // already recording
        return false;
    }
    // Ensure the parent folder exists before we open the output file.
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(dir.absolutePath())) {
            qCritical() << QString("Failed to create the save folder: %1").arg(dir.absolutePath());
            return false;
        }
    }
    m_recordFilePath = filePath;
    m_recordFormat = format;

    // Create + open + start the recorder IMMEDIATELY. scrcpy only sends the
    // codec-config packet once at stream start, so we cannot wait for a new
    // config packet to build the recorder (the old deferred design never fired
    // mid-session). Instead we arm m_recordWaitingKeyframe: the getFrame lambda
    // writes the header from the cached config and starts at the next keyframe
    // (~1s away thanks to i-frame-interval:int=1).
    Recorder *recorder = new Recorder(m_recordFilePath);
    recorder->setCodec(m_videoCodecId);   // h264/h265/av1
    recorder->setFrameSize(m_videoSize);
    if (!m_recordFormat.trimmed().isEmpty()) {
        if (0 == m_recordFormat.compare("mkv", Qt::CaseInsensitive)) {
            recorder->setFormat(Recorder::RECORDER_FORMAT_MKV);
        } else {
            recorder->setFormat(Recorder::RECORDER_FORMAT_MP4);
        }
    }
    if (!recorder->open()) {
        qCritical("Could not open recorder");
        delete recorder;
        return false;
    }
    if (!recorder->startRecorder()) {
        qCritical("Could not start recorder");
        recorder->close();
        delete recorder;
        return false;
    }
    m_recordWaitingKeyframe = true;
    // A/V sync: stamp the recording start (same instant the GUI starts the audio
    // tee + mic) and clear any stale first-frame stamp from a prior session.
    m_recordStartMs = QDateTime::currentMSecsSinceEpoch();
    m_recordFirstFrameMs = 0;
    m_recorder = recorder;
    qInfo("recording started -> %s", qUtf8Printable(filePath));
    return true;
}

void Device::stopRecord()
{
    if (!m_recorder) {
        return;
    }
    // Detach the recorder pointer FIRST so the demuxer-thread push lambdas
    // (getFrame/getConfigFrame) stop feeding it before we stop+close it.
    Recorder *recorder = m_recorder;
    m_recorder = Q_NULLPTR;
    m_recordWaitingKeyframe = false;

    if (recorder->isRunning()) {
        recorder->stopRecorder();
        recorder->wait();
    }
    recorder->close();   // synchronous finalize: file is muxable right after
    // recorder->wait() above guarantees its thread has finished; we are not on
    // the recorder's own thread, so a direct delete is safe (no event loop runs
    // on it for deleteLater()).
    delete recorder;
    qInfo("recording stopped -> %s", qUtf8Printable(m_recordFilePath));
}

bool Device::isRecording()
{
    return m_recorder != Q_NULLPTR;
}

qint64 Device::getRecordAudioSkipMs()
{
    // Both stamps must be set (a recording started AND its first frame was
    // written). The difference is how long the audio led the video; the GUI
    // discards that many leading ms of audio so the muxed A/V lines up.
    if (m_recordStartMs > 0 && m_recordFirstFrameMs > 0) {
        const qint64 skip = m_recordFirstFrameMs - m_recordStartMs;
        return skip > 0 ? skip : 0;
    }
    return 0;
}

bool Device::saveFrame(int width, int height, uint8_t* dataRGB32)
{
    if (!dataRGB32) {
        return false;
    }

    QImage rgbImage(dataRGB32, width, height, QImage::Format_RGB32);

    // save
    QString absFilePath;
    QString fileDir(m_params.recordPath);
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        return false;
    }
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = dateTime.toString("_yyyyMMdd_hhmmss_zzz");
    fileName = m_params.serial + fileName;
    fileName.replace(":", "_");
    fileName.replace(".", "_");
    fileName += ".png";
    QDir dir(fileDir);
    absFilePath = dir.absoluteFilePath(fileName);
    int ret = rgbImage.save(absFilePath, "PNG", 100);
    if (!ret) {
        return false;
    }

    qInfo() << "screenshot save to " << absFilePath;
    return true;
}

}
