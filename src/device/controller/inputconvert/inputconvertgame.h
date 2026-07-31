#ifndef INPUTCONVERTGAME_H
#define INPUTCONVERTGAME_H

#include <QPointF>
#include <QQueue>
#include <QVector>
#include <QString>

#include "inputconvertnormal.h"
#include "keymap.h"

#define MULTI_TOUCH_MAX_NUM 10

// One step of a gun's recoil pattern: hold (dx,dy) relative-units/sec for `ms`.
struct RecoilSeg {
    double ms = 100000.0;
    double dx = 0.0;
    double dy = 0.0;
};

// A saved gun. `vertical`/`horizontal` is the SUSTAINED pull that runs the whole
// burst (so a 100-round LMG stays controlled); `pattern` is an optional initial
// overlay for the first ~second (the snappy climb / zig-zag) that then hands off
// to the sustained pull.
struct RecoilGun {
    QString name;
    double vertical = 0.0;      // sustained pull (px/s), + = down
    double horizontal = 0.0;    // sustained pull (px/s), + = left
    double mulY = 1.0;          // per-gun fine-tune, scales pattern + sustained (vertical)
    double mulX = 1.0;          // per-gun fine-tune (horizontal)
    QVector<RecoilSeg> pattern;
};

// A scope/optic preset: an extra multiplier layer on top of the active gun, so
// one recorded BASE recoil covers Red-dot / 2x / 4x / ... by just scaling it.
struct RecoilScope {
    QString name;
    double mulY = 1.0;
    double mulX = 1.0;
    QString hotkey;   // optional key to make this the active scope in-game
};
class InputConvertGame : public InputConvertNormal
{
    Q_OBJECT
public:
    InputConvertGame(Controller *controller);
    virtual ~InputConvertGame();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual bool isCurrentCustomKeymap();

    void loadKeyMap(const QString &json);

    // recoil control (native): inject a downward/horizontal look-drag while firing
    void loadRecoil(const QString &json);
    void startRecoil();
    void stopRecoil();
    QPointF recoilVelocity();           // (dx,dy) relative-units/sec for active slot

protected:
    void updateSize(const QSize &frameSize, const QSize &showSize);
    void sendTouchDownEvent(int id, QPointF pos);
    void sendTouchMoveEvent(int id, QPointF pos);
    void sendTouchUpEvent(int id, QPointF pos);
    void sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action);
    void sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode);
    QPointF calcFrameAbsolutePos(QPointF relativePos);
    QPointF calcScreenAbsolutePos(QPointF relativePos);

    // multi touch id
    int attachTouchID(int key);
    void detachTouchID(int key);
    int getTouchID(int key);

    // steer wheel
    void processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from);

    // click
    void processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from);

    // click mutil
    void processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from);

    // drag
    void processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from);

    // android key
    void processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from);

    // mouse
    bool processMouseClick(const QMouseEvent *from);
    bool processMouseMove(const QMouseEvent *from);
    void moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel);
    void mouseMoveStartTouch(const QMouseEvent *from);
    void mouseMoveStopTouch();
    void startMouseMoveTimer();
    void stopMouseMoveTimer();

    bool switchGameMap();
    bool checkCursorPos(const QMouseEvent *from);
    void hideMouseCursor(bool hide);

    void getDelayQueue(const QPointF& start, const QPointF& end,
                       const double& distanceStep, const double& posStepconst,
                       quint32 lowestTimer, quint32 highestTimer,
                       QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer);

protected:
    void timerEvent(QTimerEvent *event);

private slots:
    void onSteerWheelTimer();
    void onDragTimer();
    void onRecoilTimer();

private:
    QSize m_frameSize;
    QSize m_showSize;
    bool m_gameMap = false;
    bool m_needBackMouseMove = false;
    int m_multiTouchID[MULTI_TOUCH_MAX_NUM] = { 0 };
    KeyMap m_keyMap;

    bool m_processMouseMove = true;

    // steer wheel
    struct
    {
        // the first key pressed
        int touchKey = Qt::Key_unknown;
        bool pressedUp = false;
        bool pressedDown = false;
        bool pressedLeft = false;
        bool pressedRight = false;

        // for delay
        struct {
            QPointF currentPos;
            QTimer* timer = nullptr;
            QQueue<QPointF> queuePos;
            QQueue<quint32> queueTimer;
            int pressedNum = 0;
        } delayData;
    } m_ctrlSteerWheel;

    // mouse move
    struct
    {
        QPointF lastConverPos;
        QPointF lastPos = { 0.0, 0.0 };
        bool touching = false;
        int timer = 0;
        bool smallEyes = false;
        int ignoreCount = 0;
    } m_ctrlMouseMove;

    // for drag delay
    struct {
        QPointF currentPos;
        QTimer* timer = nullptr;
        QQueue<QPointF> queuePos;
        QQueue<quint32> queueTimer;
        int pressKey = 0;
    } m_dragDelayData;

    // recoil control
    struct {
        bool enabled = false;       // master on/off
        double strength = 1.0;      // vertical multiplier
        double strengthX = 1.0;     // horizontal multiplier (tune zig-zag apart)
        bool firing = false;        // left mouse held in game mode
        int slot = 1;               // active slot (keys 1/2)
        double elapsedMs = 0.0;     // time since this burst started
        QTimer* timer = nullptr;    // ~100 Hz tick
        QVector<RecoilGun> guns;    // saved library
        QString slot1Gun;           // gun bound to key 1
        QString slot2Gun;           // gun bound to key 2
        QVector<RecoilScope> scopes;
        int activeScope = 0;        // index into scopes (extra multiplier layer)
    } m_recoil;
};

#endif // INPUTCONVERTGAME_H
