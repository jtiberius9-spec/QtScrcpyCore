#include <QDebug>
#include <QCursor>
#include <QGuiApplication>
#include <QTimer>
#include <QTime>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "inputconvertgame.h"

#define CURSOR_POS_CHECK 50

static bool qtKeyMatches(int qtKey, const QString &name);   // defined below

InputConvertGame::InputConvertGame(Controller *controller) : InputConvertNormal(controller) {
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout, this, &InputConvertGame::onSteerWheelTimer);

    m_recoil.timer = new QTimer(this);
    m_recoil.timer->setInterval(10);   // ~100 Hz
    connect(m_recoil.timer, &QTimer::timeout, this, &InputConvertGame::onRecoilTimer);
}

InputConvertGame::~InputConvertGame()
{
    // If we're destroyed while in game mode (updateScript recreates the converter
    // on every recoil/keymap change), Wraith has hidden + grabbed the cursor.
    // Undo that here, or the cursor stays invisible until the app restarts.
    if (m_gameMap) {
        hideMouseCursor(false);
#ifdef QT_NO_DEBUG
        emit grabCursor(false);
#endif
    }
}

void InputConvertGame::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 处理开关按键
    if (m_keyMap.isSwitchOnKeyboard() == false && m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() != QEvent::MouseButtonPress) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    if (!m_needBackMouseMove && m_gameMap) {
        updateSize(frameSize, showSize);
        // recoil control: arm/disarm on fire (left mouse), without consuming the click
        if (m_recoil.enabled && from->button() == Qt::LeftButton) {
            if (QEvent::MouseButtonPress == from->type()) {
                startRecoil();
            } else if (QEvent::MouseButtonRelease == from->type()) {
                stopRecoil();
            }
        }
        // mouse move
        if (m_keyMap.isValidMouseMoveMap()) {
            if (processMouseMove(from)) {
                return;
            }
        }
        // mouse click
        if (processMouseClick(from)) {
            return;
        }
    }
    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_gameMap) {
        updateSize(frameSize, showSize);
        // mouse wheel cycles the active scope preset (up = next, down = prev, wraps)
        if (m_recoil.enabled && !m_recoil.scopes.isEmpty() && from->angleDelta().y() != 0) {
            const int n = m_recoil.scopes.size();
            if (from->angleDelta().y() > 0) {
                m_recoil.activeScope = (m_recoil.activeScope + 1) % n;
            } else {
                m_recoil.activeScope = (m_recoil.activeScope - 1 + n) % n;
            }
            emit recoilHint(QString("Scope: %1").arg(m_recoil.scopes[m_recoil.activeScope].name));
        }
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 处理开关按键
    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (QEvent::KeyPress != from->type()) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeKey(from->key());
    // 处理特殊按键：可以释放出鼠标的按键
    if (m_needBackMouseMove && KeyMap::KMT_CLICK == node.type && node.data.click.switchMap) {
        updateSize(frameSize, showSize);
        // Qt::Key_Tab Qt::Key_M for PUBG mobile
        processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
        return;
    }

    if (m_gameMap) {
        updateSize(frameSize, showSize);
        if (!from || from->isAutoRepeat()) {
            return;
        }

        // gun slot follows your weapon keys (not consumed - the game still swaps).
        // Enable/strength/pattern are tuned in the F8 recoil editor, not by hotkeys.
        if (QEvent::KeyPress == from->type()) {
            if (Qt::Key_1 == from->key()) {
                m_recoil.slot = 1;
                emit recoilHint(QString("Gun 1: %1").arg(m_recoil.slot1Gun));
            } else if (Qt::Key_2 == from->key()) {
                m_recoil.slot = 2;
                emit recoilHint(QString("Gun 2: %1").arg(m_recoil.slot2Gun));
            }
            // scope presets: pressing a scope's hotkey makes it the active layer
            for (int i = 0; i < m_recoil.scopes.size(); ++i) {
                if (qtKeyMatches(from->key(), m_recoil.scopes[i].hotkey)) {
                    m_recoil.activeScope = i;
                    emit recoilHint(QString("Scope: %1").arg(m_recoil.scopes[i].name));
                }
            }
        }

        // small eyes
        if (m_keyMap.isValidMouseMoveMap() && from->key() == m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.key) {
            m_ctrlMouseMove.smallEyes = (QEvent::KeyPress == from->type());

            if (QEvent::KeyPress == from->type()) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });

                stopMouseMoveTimer();
            } else {
                mouseMoveStopTouch();
                mouseMoveStartTouch(nullptr);
            }
            return;
        }

        switch (node.type) {
        // 处理方向盘
        case KeyMap::KMT_STEER_WHEEL:
            processSteerWheel(node, from);
            return;
        // 处理普通按键
        case KeyMap::KMT_CLICK:
            processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
            processAndroidKey(node.data.click.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_TWICE:
            processKeyClick(node.data.clickTwice.keyNode.pos, true, false, from);
            processAndroidKey(node.data.clickTwice.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_MULTI:
            processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes, node.data.clickMulti.keyNode.delayClickNodesCount, from);
            return;
        case KeyMap::KMT_DRAG:
            processKeyDrag(node.data.drag.keyNode.pos, node.data.drag.keyNode.extendPos,
                         node.data.drag.startDelay, node.data.drag.dragSpeed, from);
            return;
        case KeyMap::KMT_ANDROID_KEY:
            processAndroidKey(node.data.androidKey.keyNode.androidKey, from);
        default:
            break;
        }
    } else {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    m_keyMap.loadKeyMap(json);
    loadRecoil(json);
}

// ---------------- recoil control ----------------
// "recoilControl" in the keymap JSON:
//   "recoilControl": {
//     "enabled": true, "strength": 1.0, "strengthX": 1.0,
//     "slot1Gun": "M16", "slot2Gun": "Ray Gun",
//     "guns": [
//       { "name":"M16", "vertical":0.25, "horizontal":0.0,
//         "pattern":[ {"ms":150,"dx":0.1,"dy":0.35}, ... ] }   // pattern optional
//     ]
//   }
// vertical/horizontal = SUSTAINED pull (px/s, whole burst -> long mags stay flat).
// pattern = optional initial overlay; after it ends, sustained continues.
static QVector<RecoilSeg> parsePattern(const QJsonValue &v)
{
    QVector<RecoilSeg> out;
    for (const QJsonValue &e : v.toArray()) {
        QJsonObject o = e.toObject();
        RecoilSeg s;
        s.ms = o.value("ms").toDouble(150.0);
        s.dx = o.value("dx").toDouble(0.0);
        s.dy = o.value("dy").toDouble(0.0);
        out.append(s);
    }
    return out;
}

void InputConvertGame::loadRecoil(const QString &json)
{
    m_recoil.guns.clear();
    m_recoil.slot1Gun.clear();
    m_recoil.slot2Gun.clear();
    QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    QJsonObject rc = root.value("recoilControl").toObject();
    m_recoil.enabled = rc.value("enabled").toBool(false);
    m_recoil.strength = rc.value("strength").toDouble(1.0);
    m_recoil.strengthX = rc.value("strengthX").toDouble(m_recoil.strength);

    const QJsonArray guns = rc.value("guns").toArray();
    if (!guns.isEmpty()) {
        for (const QJsonValue &gv : guns) {
            QJsonObject go = gv.toObject();
            RecoilGun g;
            g.name = go.value("name").toString();
            g.vertical = go.value("vertical").toDouble(0.0);
            g.horizontal = go.value("horizontal").toDouble(0.0);
            g.mulY = go.value("mulY").toDouble(1.0);
            g.mulX = go.value("mulX").toDouble(1.0);
            g.pattern = parsePattern(go.value("pattern"));
            m_recoil.guns.append(g);
        }
        m_recoil.slot1Gun = rc.value("slot1Gun").toString();
        m_recoil.slot2Gun = rc.value("slot2Gun").toString();
    } else {
        // backward compat with the old slot1/slot2 shape
        auto mk = [&](const QString &name, const QJsonValue &v) {
            RecoilGun g;
            g.name = name;
            if (v.isObject()) {
                g.vertical = v.toObject().value("dy").toDouble(0.0);
                g.horizontal = v.toObject().value("dx").toDouble(0.0);
            } else if (v.isArray()) {
                g.pattern = parsePattern(v);
                if (!g.pattern.isEmpty()) {
                    g.vertical = g.pattern.last().dy;
                    g.horizontal = g.pattern.last().dx;
                }
            }
            m_recoil.guns.append(g);
        };
        if (rc.contains("slot1")) mk("Slot 1", rc.value("slot1"));
        if (rc.contains("slot2")) mk("Slot 2", rc.value("slot2"));
        if (!m_recoil.guns.isEmpty()) m_recoil.slot1Gun = m_recoil.guns.first().name;
        if (m_recoil.guns.size() > 1) m_recoil.slot2Gun = m_recoil.guns[1].name;
    }
    if (m_recoil.slot1Gun.isEmpty() && !m_recoil.guns.isEmpty()) {
        m_recoil.slot1Gun = m_recoil.guns.first().name;
    }

    m_recoil.scopes.clear();
    for (const QJsonValue &sv : rc.value("scopes").toArray()) {
        QJsonObject so = sv.toObject();
        RecoilScope s;
        s.name = so.value("name").toString();
        s.mulY = so.value("mulY").toDouble(1.0);
        s.mulX = so.value("mulX").toDouble(1.0);
        s.hotkey = so.value("hotkey").toString();
        m_recoil.scopes.append(s);
    }
    m_recoil.activeScope = rc.value("activeScope").toInt(0);

    qInfo() << "recoil:" << (m_recoil.enabled ? "ON" : "off") << "guns" << m_recoil.guns.size()
            << "scopes" << m_recoil.scopes.size()
            << "slot1" << m_recoil.slot1Gun << "slot2" << m_recoil.slot2Gun;
}

void InputConvertGame::startRecoil()
{
    if (!m_keyMap.isValidMouseMoveMap()) {
        return;
    }
    m_recoil.firing = true;
    m_recoil.elapsedMs = 0.0;
    if (!m_recoil.timer->isActive()) {
        m_recoil.timer->start();
    }
}

void InputConvertGame::stopRecoil()
{
    m_recoil.firing = false;
    m_recoil.timer->stop();
}

QPointF InputConvertGame::recoilVelocity()
{
    const QString &name = (m_recoil.slot == 2) ? m_recoil.slot2Gun : m_recoil.slot1Gun;
    const RecoilGun *gun = nullptr;
    for (const RecoilGun &g : m_recoil.guns) {
        if (g.name == name) {
            gun = &g;
            break;
        }
    }
    if (!gun) {
        return QPointF(0.0, 0.0);
    }
    // sustained pull is the default; it runs the WHOLE burst (fixes long mags)
    double dx = gun->horizontal, dy = gun->vertical;
    if (!gun->pattern.isEmpty()) {
        double dur = 0.0;
        for (const RecoilSeg &s : gun->pattern) {
            dur += s.ms;
        }
        if (m_recoil.elapsedMs <= dur) {           // still in the initial overlay
            double acc = 0.0;
            const RecoilSeg *seg = &gun->pattern.last();
            for (const RecoilSeg &s : gun->pattern) {
                acc += s.ms;
                if (m_recoil.elapsedMs <= acc) {
                    seg = &s;
                    break;
                }
            }
            dx = seg->dx;
            dy = seg->dy;
        }
    }
    double scopeY = 1.0, scopeX = 1.0;
    if (m_recoil.activeScope >= 0 && m_recoil.activeScope < m_recoil.scopes.size()) {
        scopeY = m_recoil.scopes[m_recoil.activeScope].mulY;
        scopeX = m_recoil.scopes[m_recoil.activeScope].mulX;
    }
    return QPointF(dx * m_recoil.strengthX * gun->mulX * scopeX,
                   dy * m_recoil.strength * gun->mulY * scopeY);
}

void InputConvertGame::onRecoilTimer()
{
    if (!m_recoil.enabled || !m_recoil.firing || !m_gameMap || !m_keyMap.isValidMouseMoveMap()) {
        return;
    }
    QPointF v = recoilVelocity();
    m_recoil.elapsedMs += m_recoil.timer->interval();
    if (v.isNull()) {
        return;
    }
    const double dt = m_recoil.timer->interval() / 1000.0;

    // keep the look-touch alive (don't let the 500ms idle timer lift it)
    if (!m_ctrlMouseMove.touching) {
        mouseMoveStartTouch(nullptr);
    }
    startMouseMoveTimer();

    // link to sensitivity: same speedRatio the mouse-look uses, so the numbers
    // are in your aim's units. Convention: v.x + = pull LEFT, v.y + = pull DOWN.
    QPointF sr = m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio;
    const double srx = (sr.x() != 0.0) ? sr.x() : 1.0;
    const double sry = (sr.y() != 0.0) ? sr.y() : 1.0;
    m_ctrlMouseMove.lastConverPos.setX(m_ctrlMouseMove.lastConverPos.x() - (v.x() / srx) * dt);
    m_ctrlMouseMove.lastConverPos.setY(m_ctrlMouseMove.lastConverPos.y() + (v.y() / sry) * dt);

    // When the pull nears an edge, lift the touch; the next tick re-plants it at
    // the safe mouse-look anchor (startPos) and keeps pulling.
    // NOTE: do NOT "seamlessly re-plant near the top/bottom" - those corners
    // overlap the game's on-screen buttons, so it pressed random controls and
    // broke WASD. The safe anchor is worth the tiny re-center.
    if (m_ctrlMouseMove.lastConverPos.x() < 0.05 || m_ctrlMouseMove.lastConverPos.x() > 0.95
        || m_ctrlMouseMove.lastConverPos.y() < 0.05 || m_ctrlMouseMove.lastConverPos.y() > 0.95) {
        mouseMoveStopTouch();
        return;
    }
    sendTouchMoveEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (showSize != m_showSize) {
        if (m_gameMap && m_keyMap.isValidMouseMoveMap()) {
#ifdef QT_NO_DEBUG
            // show size change, resize grab cursor
            emit grabCursor(true);
#endif
        }
    }
    m_frameSize = frameSize;
    m_showSize = showSize;
}

void InputConvertGame::sendTouchDownEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

void InputConvertGame::sendTouchMoveEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

void InputConvertGame::sendTouchUpEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

void InputConvertGame::sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        Q_ASSERT(0);
        return;
    }
    //qDebug() << "id:" << id << " pos:" << pos << " action" << action;
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        return;
    }

    QPoint absolutePos = calcFrameAbsolutePos(pos).toPoint();
    static QPoint lastAbsolutePos = absolutePos;
    if (AMOTION_EVENT_ACTION_MOVE == action && lastAbsolutePos == absolutePos) {
        delete controlMsg;
        return;
    }
    lastAbsolutePos = absolutePos;

    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(absolutePos, m_frameSize),
        AMOTION_EVENT_ACTION_DOWN == action ? 1.0f : 0.0f);
    sendControlMsg(controlMsg);
}

// Does a Qt key event key match a stored hotkey name ("3","C","F1","Space")?
static bool qtKeyMatches(int qtKey, const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    const QString n = name.trimmed().toUpper();
    if (n.size() == 1) {
        const QChar c = n.at(0);
        if (c >= 'A' && c <= 'Z') return qtKey == (Qt::Key_A + (c.unicode() - 'A'));
        if (c >= '0' && c <= '9') return qtKey == (Qt::Key_0 + (c.unicode() - '0'));
    }
    if (n.startsWith('F')) {
        bool ok = false;
        const int f = n.mid(1).toInt(&ok);
        if (ok && f >= 1 && f <= 12) return qtKey == (Qt::Key_F1 + f - 1);
    }
    if (n == "SPACE") return qtKey == Qt::Key_Space;
    if (n == "TAB") return qtKey == Qt::Key_Tab;
    return false;
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode) {
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlMsg) {
        return;
    }

    controlMsg->setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    sendControlMsg(controlMsg);
}

QPointF InputConvertGame::calcFrameAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_frameSize.width() * relativePos.x());
    absolutePos.setY(m_frameSize.height() * relativePos.y());
    return absolutePos;
}

QPointF InputConvertGame::calcScreenAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_showSize.width() * relativePos.x());
    absolutePos.setY(m_showSize.height() * relativePos.y());
    return absolutePos;
}

int InputConvertGame::attachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (0 == m_multiTouchID[i]) {
            m_multiTouchID[i] = key;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            m_multiTouchID[i] = 0;
            return;
        }
    }
}

int InputConvertGame::getTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            return i;
        }
    }
    return -1;
}

// -------- steer wheel event --------

void InputConvertGame::getDelayQueue(const QPointF& start, const QPointF& end,
                                     const double& distanceStep, const double& posStepconst,
                                     quint32 lowestTimer, quint32 highestTimer,
                                     QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer) {
    double x1 = start.x();
    double y1 = start.y();
    double x2 = end.x();
    double y2 = end.y();

    double dx=x2-x1;
    double dy=y2-y1;
    double e=(fabs(dx)>fabs(dy))?fabs(dx):fabs(dy);
    e /= distanceStep;
    dx/=e;
    dy/=e;

    QQueue<QPointF> queue;
    QQueue<quint32> queue2;
    for(int i=1;i<=e;i++) {
        QPointF pos(x1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst), y1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst));
        queue.enqueue(pos);
        queue2.enqueue(QRandomGenerator::global()->bounded(lowestTimer, highestTimer));
        x1+=dx;
        y1+=dy;
    }

    queuePos = queue;
    queueTimer = queue2;
}

void InputConvertGame::onSteerWheelTimer() {
    if(m_ctrlSteerWheel.delayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_ctrlSteerWheel.touchKey);
    m_ctrlSteerWheel.delayData.currentPos = m_ctrlSteerWheel.delayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_ctrlSteerWheel.delayData.currentPos);

    if(m_ctrlSteerWheel.delayData.queuePos.empty() && m_ctrlSteerWheel.delayData.pressedNum == 0) {
        sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    if(!m_ctrlSteerWheel.delayData.queuePos.empty()) {
        m_ctrlSteerWheel.delayData.timer->start(m_ctrlSteerWheel.delayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    int key = from->key();
    bool flag = from->type() == QEvent::KeyPress;
    // identify keys
    if (key == node.data.steerWheel.up.key) {
        m_ctrlSteerWheel.pressedUp = flag;
    } else if (key == node.data.steerWheel.right.key) {
        m_ctrlSteerWheel.pressedRight = flag;
    } else if (key == node.data.steerWheel.down.key) {
        m_ctrlSteerWheel.pressedDown = flag;
    } else { // left
        m_ctrlSteerWheel.pressedLeft = flag;
    }

    // Build a direction vector from the pressed keys (screen space: +x right,
    // +y down) and count how many are held.
    int pressedNum = 0;
    double dirX = 0.0, dirY = 0.0;
    if (m_ctrlSteerWheel.pressedUp)    { ++pressedNum; dirY -= 1.0; }
    if (m_ctrlSteerWheel.pressedRight) { ++pressedNum; dirX += 1.0; }
    if (m_ctrlSteerWheel.pressedDown)  { ++pressedNum; dirY += 1.0; }
    if (m_ctrlSteerWheel.pressedLeft)  { ++pressedNum; dirX -= 1.0; }
    m_ctrlSteerWheel.delayData.pressedNum = pressedNum;

    // Project the touch onto a CIRCLE of uniform on-screen (pixel) radius.
    //
    // The keymap offsets are RELATIVE (0..1), but the device maps x by frame
    // WIDTH and y by frame HEIGHT (see calcFrameAbsolutePos). On a landscape
    // phone (width >> height) equal relative offsets are NOT equal on screen,
    // so summing them made diagonals (W+A / W+D) collapse toward horizontal
    // (you walked sideways instead of strafing) and forward never reached the
    // sprint rim (walk instead of run). We therefore compute everything in
    // pixels on a single circle, then convert back to relative coordinates so
    // every direction travels the same distance at the correct angle.
    QPointF offset(0.0, 0.0);
    const double fw = m_frameSize.width();
    const double fh = m_frameSize.height();
    if (pressedNum > 0 && fw > 0.0 && fh > 0.0) {
        // Circle radius in PIXELS = the largest configured reach in any
        // direction, so the user's drag-radius is honoured and the wheel
        // always pushes out to full tilt (run, not walk) in every direction.
        const double radiusPx = qMax(
            qMax(node.data.steerWheel.up.extendOffset   * fh,
                 node.data.steerWheel.down.extendOffset  * fh),
            qMax(node.data.steerWheel.left.extendOffset  * fw,
                 node.data.steerWheel.right.extendOffset * fw));
        const double len = sqrt(dirX * dirX + dirY * dirY);
        if (len > 0.0 && radiusPx > 0.0) {
            offset.setX((radiusPx * dirX / len) / fw);  // pixels -> relative x
            offset.setY((radiusPx * dirY / len) / fh);  // pixels -> relative y
        }
    }

    // last key release and timer no active, active timer to detouch
    if (pressedNum == 0) {
        if (m_ctrlSteerWheel.delayData.timer->isActive()) {
            m_ctrlSteerWheel.delayData.timer->stop();
            m_ctrlSteerWheel.delayData.queueTimer.clear();
            m_ctrlSteerWheel.delayData.queuePos.clear();
        }

        sendTouchUpEvent(getTouchID(m_ctrlSteerWheel.touchKey), m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    // process steer wheel key event
    m_ctrlSteerWheel.delayData.timer->stop();
    m_ctrlSteerWheel.delayData.queueTimer.clear();
    m_ctrlSteerWheel.delayData.queuePos.clear();

    // first press, get key and touch down
    if (pressedNum == 1 && flag) {
        m_ctrlSteerWheel.touchKey = from->key();
        int id = attachTouchID(m_ctrlSteerWheel.touchKey);
        sendTouchDownEvent(id, node.data.steerWheel.centerPos);
    }

    // Smoothly sweep the joystick from where it is to the new full-tilt point,
    // instead of teleporting straight to the rim. A one-hop snap reaches the
    // circle so fast it feels "boxed"/twitchy on taps and direction changes;
    // ramping across ~12 micro-steps (~3-6ms each, ~50ms total) restores the
    // smooth round-the-circle feel while staying responsive.
    if (pressedNum == 1 && flag) {
        m_ctrlSteerWheel.delayData.currentPos = node.data.steerWheel.centerPos;
    }
    const QPointF target = node.data.steerWheel.centerPos + offset;
    getDelayQueue(m_ctrlSteerWheel.delayData.currentPos, target,
                  0.008, 0.002, 3, 6,
                  m_ctrlSteerWheel.delayData.queuePos, m_ctrlSteerWheel.delayData.queueTimer);
    if (!m_ctrlSteerWheel.delayData.queueTimer.isEmpty()) {
        m_ctrlSteerWheel.delayData.timer->start(m_ctrlSteerWheel.delayData.queueTimer.dequeue());
    } else {
        sendTouchMoveEvent(getTouchID(m_ctrlSteerWheel.touchKey), target);
        m_ctrlSteerWheel.delayData.currentPos = target;
    }
    return;
}

// -------- key event --------

void InputConvertGame::processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from)
{
    if (switchMap && QEvent::KeyRelease == from->type()) {
        m_needBackMouseMove = !m_needBackMouseMove;
        hideMouseCursor(!m_needBackMouseMove);
    }

    if (QEvent::KeyPress == from->type()) {
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, clickPos);
        if (clickTwice) {
            sendTouchUpEvent(getTouchID(from->key()), clickPos);
            detachTouchID(from->key());
        }
    } else if (QEvent::KeyRelease == from->type()) {
        if (clickTwice) {
            int id = attachTouchID(from->key());
            sendTouchDownEvent(id, clickPos);
        }
        sendTouchUpEvent(getTouchID(from->key()), clickPos);
        detachTouchID(from->key());
    }
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from)
{
    if (QEvent::KeyPress != from->type()) {
        return;
    }

    int key = from->key();
    int delay = 0;
    QPointF clickPos;

    for (int i = 0; i < count; i++) {
        delay += nodes[i].delay;
        clickPos = nodes[i].pos;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = attachTouchID(key);
            sendTouchDownEvent(id, clickPos);
        });

        // Don't up it too fast
        delay += 20;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = getTouchID(key);
            sendTouchUpEvent(id, clickPos);
            detachTouchID(key);
        });
    }
}

void InputConvertGame::onDragTimer() {
    if(m_dragDelayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_dragDelayData.pressKey);
    m_dragDelayData.currentPos = m_dragDelayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);

    if(m_dragDelayData.queuePos.empty()) {
        delete m_dragDelayData.timer;
        m_dragDelayData.timer = nullptr;

        sendTouchUpEvent(id, m_dragDelayData.currentPos);
        detachTouchID(m_dragDelayData.pressKey);

        m_dragDelayData.currentPos = QPointF();
        m_dragDelayData.pressKey = 0;
        return;
    }

    if(!m_dragDelayData.queuePos.empty()) {
        m_dragDelayData.timer->start(m_dragDelayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from)
{
    if (QEvent::KeyPress == from->type()) {
        // stop last
        if (m_dragDelayData.timer && m_dragDelayData.timer->isActive()) {
            m_dragDelayData.timer->stop();
            delete m_dragDelayData.timer;
            m_dragDelayData.timer = nullptr;
            m_dragDelayData.queuePos.clear();
            m_dragDelayData.queueTimer.clear();

            sendTouchUpEvent(getTouchID(m_dragDelayData.pressKey), m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);

            m_dragDelayData.currentPos = QPointF();
            m_dragDelayData.pressKey = 0;
        }

        // start this
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, startPos);

        m_dragDelayData.timer = new QTimer(this);
        m_dragDelayData.timer->setSingleShot(true);
        connect(m_dragDelayData.timer, &QTimer::timeout, this, &InputConvertGame::onDragTimer);
        m_dragDelayData.pressKey = from->key();
        m_dragDelayData.currentPos = startPos;
        m_dragDelayData.queuePos.clear();
        m_dragDelayData.queueTimer.clear();

        // Clamp dragSpeed to 0-1 range
        const float speed = qBound(0.0f, static_cast<float>(dragSpeed), 1.0f);
        
        // Calculate delays based on dragSpeed
        // dragSpeed = 1 -> minDelay = 1, maxDelay = 2 (fastest)
        // dragSpeed = 0 -> minDelay = 30, maxDelay = 40 (slowest)
        const quint32 minDelay = static_cast<quint32>(1 + (1.0f - speed) * 29);  // 1 to 30
        const quint32 maxDelay = minDelay + static_cast<quint32>((1.0f - speed) * 9) + 1;  // // min + (0 to 9) + 1

        getDelayQueue(startPos, endPos,
                      0.01f, 0.0005f,
                      minDelay,
                      maxDelay,
                      m_dragDelayData.queuePos,
                      m_dragDelayData.queueTimer);

        m_dragDelayData.timer->start(startDelay);
    }
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from)
{
    if (AKEYCODE_UNKNOWN == androidKey) {
        return;
    }

    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    sendKeyEvent(action, androidKey);
}

// -------- mouse event --------

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (KeyMap::KMT_INVALID == node.type) {
        return false;
    }

    if (QEvent::MouseButtonPress == from->type() || QEvent::MouseButtonDblClick == from->type()) {
        int id = attachTouchID(from->button());
        sendTouchDownEvent(id, node.data.click.keyNode.pos);
        return true;
    }
    if (QEvent::MouseButtonRelease == from->type()) {
        int id = getTouchID(from->button());
        sendTouchUpEvent(id, node.data.click.keyNode.pos);
        detachTouchID(from->button());
        return true;
    }
    return false;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    if (QEvent::MouseMove != from->type()) {
        return false;
    }

    if (checkCursorPos(from)) {
        m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);
        return true;
    }

    auto lastPos = m_ctrlMouseMove.lastPos;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_ctrlMouseMove.lastPos = from->localPos();
#else
    m_ctrlMouseMove.lastPos = from->position();
#endif

    if (m_ctrlMouseMove.ignoreCount > 0) {
        --m_ctrlMouseMove.ignoreCount;
        return true;
    }

    if (!lastPos.isNull() && m_processMouseMove) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF distance_raw{from->localPos() - lastPos};
#else
        QPointF distance_raw{from->position() - lastPos};
#endif
        QPointF speedRatio  {m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio};
        QPointF distance    {distance_raw.x() / speedRatio.x(), distance_raw.y() / speedRatio.y()};

        mouseMoveStartTouch(from);
        startMouseMoveTimer();

        m_ctrlMouseMove.lastConverPos.setX(m_ctrlMouseMove.lastConverPos.x() + distance.x() / m_showSize.width());
        m_ctrlMouseMove.lastConverPos.setY(m_ctrlMouseMove.lastConverPos.y() + distance.y() / m_showSize.height());

        if (m_ctrlMouseMove.lastConverPos.x() < 0.05 || m_ctrlMouseMove.lastConverPos.x() > 0.95 || m_ctrlMouseMove.lastConverPos.y() < 0.05
            || m_ctrlMouseMove.lastConverPos.y() > 0.95) {
            if (m_ctrlMouseMove.smallEyes) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });
            } else {
                mouseMoveStopTouch();
                m_ctrlMouseMove.ignoreCount = 5;
                return true;
            }
        }

        sendTouchMoveEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
    }

    return true;
}

bool InputConvertGame::checkCursorPos(const QMouseEvent *from)
{
    bool moveCursor = false;
    QPoint pos = from->pos();
    if (pos.x() < CURSOR_POS_CHECK) {
        pos.setX(m_showSize.width() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.x() > m_showSize.width() - CURSOR_POS_CHECK) {
        pos.setX(CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() < CURSOR_POS_CHECK) {
        pos.setY(m_showSize.height() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() > m_showSize.height() - CURSOR_POS_CHECK) {
        pos.setY(CURSOR_POS_CHECK);
        moveCursor = true;
    }

    if (moveCursor) {
        moveCursorTo(from, pos);
    }

    return moveCursor;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint posOffset = from->pos() - localPosPixel;
    QPoint globalPos = from->globalPos();
#else
    QPoint posOffset = from->position().toPoint() - localPosPixel;
    QPoint globalPos = from->globalPosition().toPoint();
#endif
    globalPos -= posOffset;
    //qDebug()<<"move cursor to "<<globalPos<<" offset "<<posOffset;
    QCursor::setPos(globalPos);
}

void InputConvertGame::mouseMoveStartTouch(const QMouseEvent *from)
{
    Q_UNUSED(from)
    if (!m_ctrlMouseMove.touching) {
        QPointF mouseMoveStartPos
            = m_ctrlMouseMove.smallEyes ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
        int id = attachTouchID(Qt::ExtraButton24);
        sendTouchDownEvent(id, mouseMoveStartPos);
        m_ctrlMouseMove.lastConverPos = mouseMoveStartPos;
        m_ctrlMouseMove.touching = true;
    }
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (m_ctrlMouseMove.touching) {
        sendTouchUpEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
        detachTouchID(Qt::ExtraButton24);
        m_ctrlMouseMove.touching = false;
    }
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(500);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (0 != m_ctrlMouseMove.timer) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

bool InputConvertGame::switchGameMap()
{
    m_gameMap = !m_gameMap;
    qInfo() << QString("current keymap mode: %1").arg(m_gameMap ? "custom" : "normal");

    if (!m_keyMap.isValidMouseMoveMap()) {
        return m_gameMap;
    }
#ifdef QT_NO_DEBUG
    // grab cursor and set cursor only mouse move map
    emit grabCursor(m_gameMap);
#endif
    hideMouseCursor(m_gameMap);

    if (!m_gameMap) {
        stopRecoil();
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }

    return m_gameMap;
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    if (hide) {
#ifdef QT_NO_DEBUG
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
#else
        QGuiApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
#endif
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }
}
